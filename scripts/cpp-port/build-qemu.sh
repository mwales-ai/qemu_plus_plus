#!/bin/bash
# build-qemu.sh - Build QEMU for the 5 target ISAs
#
# Usage: ./scripts/cpp-port/build-qemu.sh [--clean] [--jobs N]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
CLEAN=0
JOBS=$(nproc)

TARGET_LIST="x86_64-softmmu,aarch64-softmmu,arm-softmmu,ppc64-softmmu,riscv64-softmmu"

while [ $# -gt 0 ]; do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        --jobs)  JOBS="$2"; shift 2 ;;
        *) echo "Unknown argument: $1"; exit 1 ;;
    esac
done

echo "=== QEMU++ Builder ==="
echo "Targets: $TARGET_LIST"
echo "Jobs:    $JOBS"
echo ""

if [ "$CLEAN" -eq 1 ] && [ -d "$BUILD_DIR" ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ ! -f "build.ninja" ]; then
    echo "=== Configuring ==="
    "${REPO_ROOT}/configure" \
        --target-list="$TARGET_LIST" \
        --enable-slirp \
        --enable-tools
fi

echo ""
echo "=== Building (${JOBS} jobs) ==="
make -j"$JOBS"

echo ""
echo "=== Build complete ==="
echo "Binaries:"
for arch in x86_64 aarch64 arm ppc64 riscv64; do
    binary="${BUILD_DIR}/qemu-system-${arch}"
    if [ -x "$binary" ]; then
        echo "  $binary"
    else
        echo "  $binary (NOT FOUND)"
    fi
done
