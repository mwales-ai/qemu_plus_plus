#!/bin/bash
# run-smoke-tests.sh - Run functional smoke tests for all 5 target ISAs
#
# Prerequisites:
#   1. Build QEMU: mkdir build && cd build && ../configure && make -j$(nproc)
#   2. Download images: ./scripts/cpp-port/download-test-images.sh
#
# Usage: ./scripts/cpp-port/run-smoke-tests.sh [--build-dir /path/to/build] [--image-dir /path/to/images]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
IMAGE_DIR="${REPO_ROOT}/test-images"
TIMEOUT_SECONDS=120
PASSED=0
FAILED=0
SKIPPED=0

# Parse arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --image-dir) IMAGE_DIR="$2"; shift 2 ;;
        --timeout)   TIMEOUT_SECONDS="$2"; shift 2 ;;
        *) echo "Unknown argument: $1"; exit 1 ;;
    esac
done

echo "============================================"
echo "  QEMU++ Smoke Test Suite"
echo "============================================"
echo "Build dir:  $BUILD_DIR"
echo "Image dir:  $IMAGE_DIR"
echo "Timeout:    ${TIMEOUT_SECONDS}s per test"
echo ""

# Helper: check that a QEMU binary exists
check_qemu() {
    local binary="$1"
    if [ ! -x "${BUILD_DIR}/${binary}" ]; then
        echo "  [SKIP] ${binary} not found in build directory"
        SKIPPED=$((SKIPPED + 1))
        return 1
    fi
    return 0
}

# Helper: run a QEMU instance, capture serial output, check for expected string
run_boot_test() {
    local name="$1"
    local qemu_binary="$2"
    shift 2
    local qemu_args=("$@")

    echo "--- Test: ${name} ---"

    local logfile
    logfile=$(mktemp /tmp/qemu-smoke-XXXXXX.log)

    # Run QEMU with a timeout, capture serial to log
    timeout "${TIMEOUT_SECONDS}" \
        "${BUILD_DIR}/${qemu_binary}" \
        "${qemu_args[@]}" \
        -serial file:"${logfile}" \
        -monitor none \
        -display none \
        -no-reboot \
        2>/dev/null &
    local qemu_pid=$!

    # Wait for QEMU to finish or timeout
    local exit_code=0
    wait $qemu_pid 2>/dev/null || exit_code=$?

    # Check results
    if [ -f "$logfile" ] && [ -s "$logfile" ]; then
        # Look for signs of successful boot (kernel messages, login prompts, etc.)
        if grep -qi -E "(login:|kernel|boot|starting|init)" "$logfile" 2>/dev/null; then
            echo "  [PASS] Boot output detected"
            PASSED=$((PASSED + 1))
        else
            echo "  [FAIL] QEMU ran but no boot messages detected"
            echo "  Log tail:"
            tail -5 "$logfile" | sed 's/^/    /'
            FAILED=$((FAILED + 1))
        fi
    else
        if [ $exit_code -eq 124 ]; then
            # Timeout - might still be OK if we can check for output
            echo "  [WARN] Timed out after ${TIMEOUT_SECONDS}s (may still be booting)"
            FAILED=$((FAILED + 1))
        else
            echo "  [FAIL] No serial output produced (exit code: $exit_code)"
            FAILED=$((FAILED + 1))
        fi
    fi

    rm -f "$logfile"
    echo ""
}

# Helper: simple QEMU launch test (does the binary start at all?)
run_version_test() {
    local name="$1"
    local qemu_binary="$2"

    echo "--- Test: ${name} (version check) ---"

    if ! check_qemu "$qemu_binary"; then
        return
    fi

    local output
    output=$("${BUILD_DIR}/${qemu_binary}" --version 2>&1) || true

    if echo "$output" | grep -q "QEMU emulator"; then
        echo "  [PASS] $output" | head -1
        PASSED=$((PASSED + 1))
    else
        echo "  [FAIL] Unexpected version output"
        FAILED=$((FAILED + 1))
    fi
    echo ""
}

# Helper: test that QEMU can list machines
run_machine_list_test() {
    local name="$1"
    local qemu_binary="$2"
    local expected_machine="$3"

    echo "--- Test: ${name} (machine list) ---"

    if ! check_qemu "$qemu_binary"; then
        return
    fi

    local output
    output=$("${BUILD_DIR}/${qemu_binary}" -machine help 2>&1) || true

    if echo "$output" | grep -q "$expected_machine"; then
        echo "  [PASS] Found machine: $expected_machine"
        PASSED=$((PASSED + 1))
    else
        echo "  [FAIL] Machine '$expected_machine' not found"
        FAILED=$((FAILED + 1))
    fi
    echo ""
}

# ====================
# Version check tests
# ====================
echo "============================================"
echo "  Phase 1: Binary Version Checks"
echo "============================================"
echo ""

run_version_test "x86_64 version"  "qemu-system-x86_64"
run_version_test "aarch64 version" "qemu-system-aarch64"
run_version_test "arm version"     "qemu-system-arm"
run_version_test "ppc64 version"   "qemu-system-ppc64"
run_version_test "riscv64 version" "qemu-system-riscv64"

# ====================
# Machine list tests
# ====================
echo "============================================"
echo "  Phase 2: Machine List Checks"
echo "============================================"
echo ""

run_machine_list_test "x86_64 machines"  "qemu-system-x86_64"  "q35"
run_machine_list_test "aarch64 machines" "qemu-system-aarch64" "virt"
run_machine_list_test "arm machines"     "qemu-system-arm"     "virt"
run_machine_list_test "ppc64 machines"   "qemu-system-ppc64"   "pseries"
run_machine_list_test "riscv64 machines" "qemu-system-riscv64" "virt"

# ====================
# Boot tests (require downloaded images)
# ====================
echo "============================================"
echo "  Phase 3: Boot Tests"
echo "============================================"
echo ""

# x86_64 boot test
if check_qemu "qemu-system-x86_64" && [ -f "${IMAGE_DIR}/alpine-x86_64.iso" ]; then
    run_boot_test "x86_64 Alpine boot" "qemu-system-x86_64" \
        -machine q35 \
        -m 512 \
        -cdrom "${IMAGE_DIR}/alpine-x86_64.iso" \
        -nographic
else
    echo "  [SKIP] x86_64 boot test - missing binary or image"
    SKIPPED=$((SKIPPED + 1))
    echo ""
fi

# aarch64 boot test
if check_qemu "qemu-system-aarch64" && [ -f "${IMAGE_DIR}/alpine-aarch64.iso" ]; then
    run_boot_test "aarch64 Alpine boot" "qemu-system-aarch64" \
        -machine virt \
        -cpu cortex-a57 \
        -m 512 \
        -cdrom "${IMAGE_DIR}/alpine-aarch64.iso" \
        -nographic
else
    echo "  [SKIP] aarch64 boot test - missing binary or image"
    SKIPPED=$((SKIPPED + 1))
    echo ""
fi

# arm32 boot test
if check_qemu "qemu-system-arm" && [ -f "${IMAGE_DIR}/alpine-armv7.iso" ]; then
    run_boot_test "arm32 Alpine boot" "qemu-system-arm" \
        -machine virt \
        -cpu cortex-a15 \
        -m 512 \
        -cdrom "${IMAGE_DIR}/alpine-armv7.iso" \
        -nographic
else
    echo "  [SKIP] arm32 boot test - missing binary or image"
    SKIPPED=$((SKIPPED + 1))
    echo ""
fi

# ppc64 boot test
if check_qemu "qemu-system-ppc64" && [ -f "${IMAGE_DIR}/ppc64-vmlinux" ] && [ -f "${IMAGE_DIR}/ppc64-initrd.gz" ]; then
    run_boot_test "ppc64 Debian netboot" "qemu-system-ppc64" \
        -machine pseries \
        -m 512 \
        -kernel "${IMAGE_DIR}/ppc64-vmlinux" \
        -initrd "${IMAGE_DIR}/ppc64-initrd.gz" \
        -nographic \
        -append "console=hvc0"
else
    echo "  [SKIP] ppc64 boot test - missing binary or image"
    SKIPPED=$((SKIPPED + 1))
    echo ""
fi

# riscv64 boot test
if check_qemu "qemu-system-riscv64" && [ -f "${IMAGE_DIR}/riscv64-vmlinux" ] && [ -f "${IMAGE_DIR}/riscv64-initrd.gz" ]; then
    run_boot_test "riscv64 Debian netboot" "qemu-system-riscv64" \
        -machine virt \
        -m 512 \
        -kernel "${IMAGE_DIR}/riscv64-vmlinux" \
        -initrd "${IMAGE_DIR}/riscv64-initrd.gz" \
        -nographic \
        -append "console=ttyS0"
else
    echo "  [SKIP] riscv64 boot test - missing binary or image"
    SKIPPED=$((SKIPPED + 1))
    echo ""
fi

# ====================
# Summary
# ====================
echo "============================================"
echo "  Test Summary"
echo "============================================"
echo "  PASSED:  $PASSED"
echo "  FAILED:  $FAILED"
echo "  SKIPPED: $SKIPPED"
echo "  TOTAL:   $((PASSED + FAILED + SKIPPED))"
echo "============================================"

if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
