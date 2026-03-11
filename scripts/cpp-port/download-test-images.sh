#!/bin/bash
# download-test-images.sh - Download small Alpine Linux images for QEMU testing
#
# Downloads pre-built Alpine Linux "virtual" ISO images for each target ISA.
# These are minimal (~50-200MB) and boot quickly under QEMU.
#
# Usage: ./download-test-images.sh [--dir /path/to/images]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMAGE_DIR="${REPO_ROOT}/test-images"

# Parse arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --dir) IMAGE_DIR="$2"; shift 2 ;;
        *) echo "Unknown argument: $1"; exit 1 ;;
    esac
done

mkdir -p "$IMAGE_DIR"

echo "=== QEMU++ Test Image Downloader ==="
echo "Image directory: $IMAGE_DIR"
echo ""

# Alpine Linux version to use
ALPINE_VERSION="3.21"
ALPINE_MINOR="3.21.3"
ALPINE_MIRROR="https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VERSION}/releases"

download_image() {
    local arch="$1"
    local url="$2"
    local filename="$3"
    local dest="${IMAGE_DIR}/${filename}"

    if [ -f "$dest" ]; then
        echo "  [SKIP] ${filename} already exists"
        return 0
    fi

    echo "  [DOWNLOAD] ${filename}..."
    if curl -L --fail --progress-bar -o "${dest}.tmp" "$url"; then
        mv "${dest}.tmp" "$dest"
        echo "  [OK] ${filename} ($(du -h "$dest" | cut -f1))"
    else
        rm -f "${dest}.tmp"
        echo "  [FAIL] Could not download ${filename}"
        return 1
    fi
}

# ---- x86_64 ----
echo "--- x86_64 ---"
download_image "x86_64" \
    "${ALPINE_MIRROR}/x86_64/alpine-virt-${ALPINE_MINOR}-x86_64.iso" \
    "alpine-x86_64.iso"

# ---- aarch64 ----
# ARM virt machines can't boot from CD-ROM without UEFI firmware.
# Use Debian netboot kernel+initrd for reliable direct-kernel boot.
echo "--- aarch64 ---"
AARCH64_BASE="https://deb.debian.org/debian/dists/bookworm/main/installer-arm64/current/images/netboot/debian-installer/arm64"
download_image "aarch64" \
    "${AARCH64_BASE}/linux" \
    "aarch64-linux"
download_image "aarch64" \
    "${AARCH64_BASE}/initrd.gz" \
    "aarch64-initrd.gz"

# ---- arm (32-bit) ----
# ARM virt machines can't boot from CD-ROM without UEFI firmware.
# Use Debian netboot kernel+initrd for reliable direct-kernel boot.
echo "--- arm (32-bit) ---"
ARMHF_BASE="https://deb.debian.org/debian/dists/bookworm/main/installer-armhf/current/images/netboot"
download_image "armhf" \
    "${ARMHF_BASE}/vmlinuz" \
    "arm-vmlinuz"
download_image "armhf" \
    "${ARMHF_BASE}/initrd.gz" \
    "arm-initrd.gz"

# ---- ppc64 ----
# Alpine doesn't ship ppc64 ISOs; we use a small Debian netboot kernel+initrd instead
echo "--- ppc64 ---"
PPC64_BASE="https://deb.debian.org/debian/dists/bookworm/main/installer-ppc64el/current/images/netboot/debian-installer/ppc64el"
download_image "ppc64" \
    "${PPC64_BASE}/vmlinux" \
    "ppc64-vmlinux"
download_image "ppc64" \
    "${PPC64_BASE}/initrd.gz" \
    "ppc64-initrd.gz"

# ---- riscv64 ----
# Alpine doesn't have riscv64 virt ISOs; use Debian netboot
echo "--- riscv64 ---"
RISCV64_BASE="https://deb.debian.org/debian/dists/sid/main/installer-riscv64/current/images/netboot/debian-installer/riscv64"
download_image "riscv64" \
    "${RISCV64_BASE}/linux" \
    "riscv64-linux"
download_image "riscv64" \
    "${RISCV64_BASE}/initrd.gz" \
    "riscv64-initrd.gz"

# Also download OpenSBI firmware for riscv64 if not present
OPENSBI_VERSION="1.6"
download_image "riscv64" \
    "https://github.com/riscv-software-src/opensbi/releases/download/v${OPENSBI_VERSION}/opensbi-${OPENSBI_VERSION}-rv-bin.tar.xz" \
    "opensbi-${OPENSBI_VERSION}-rv-bin.tar.xz"

echo ""
echo "=== Download Summary ==="
echo "Images stored in: $IMAGE_DIR"
ls -lh "$IMAGE_DIR"/ 2>/dev/null || true
echo ""
echo "To run smoke tests:  ./scripts/cpp-port/run-smoke-tests.sh"
