#!/bin/bash
# install-deps-ubuntu.sh - Install QEMU build dependencies on Ubuntu 24.04 LTS (Noble)
#
# Usage: sudo ./install-deps-ubuntu.sh [--minimal]

set -e

MINIMAL=0
if [ "$1" = "--minimal" ]; then
    MINIMAL=1
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This script must be run as root (use sudo)"
    exit 1
fi

echo "=== QEMU++ Build Dependency Installer for Ubuntu 24.04 ==="
echo ""

# Detect Ubuntu version
if [ -f /etc/os-release ]; then
    . /etc/os-release
    echo "Detected: $PRETTY_NAME"
    if [ "$VERSION_ID" != "24.04" ]; then
        echo "Warning: This script targets Ubuntu 24.04. Your version ($VERSION_ID) may need adjustments."
    fi
else
    echo "Warning: Cannot detect OS version. Proceeding anyway."
fi

echo ""
apt-get update

# ---- Minimal dependencies (enough to build QEMU) ----
MINIMAL_PACKAGES=(
    # Build tools
    bash
    bc
    bison
    bzip2
    ccache
    flex
    gcc
    g++
    gettext
    git
    make
    ninja-build
    pkgconf
    python3
    python3-venv
    python3-pip
    python3-setuptools
    python3-wheel
    python3-tomli
    sed
    tar

    # Core libraries
    libglib2.0-dev
    libfdt-dev
    libffi-dev
    libpixman-1-dev
    zlib1g-dev
)

echo "=== Installing minimal build dependencies ==="
apt-get install -y "${MINIMAL_PACKAGES[@]}"

if [ "$MINIMAL" -eq 1 ]; then
    echo ""
    echo "=== Minimal installation complete ==="
    exit 0
fi

# ---- Full dependencies (all optional features) ----
FULL_PACKAGES=(
    # Audio
    libasound2-dev
    libpulse-dev
    libpipewire-0.3-dev
    libsdl2-dev
    libsdl2-image-dev

    # Graphics / Display
    libepoxy-dev
    libgbm-dev
    libgtk-3-dev
    libvirglrenderer-dev
    libvte-2.91-dev
    libdrm-dev

    # Compression
    libbz2-dev
    liblzo2-dev
    libsnappy-dev
    libzstd-dev
    zstd

    # Networking
    libcurl4-gnutls-dev
    libslirp-dev
    libssh-dev

    # Crypto / Security
    libgnutls28-dev
    libgcrypt20-dev
    nettle-dev
    libsasl2-dev
    libseccomp-dev
    libselinux1-dev
    libtasn1-6-dev

    # Storage / Block
    libaio-dev
    libiscsi-dev
    libnfs-dev
    liburing-dev

    # USB / Devices
    libusb-1.0-0-dev
    libusbredirhost-dev
    libcacard-dev
    libcap-ng-dev
    libudev-dev

    # Misc libraries
    libbpf-dev
    libbrlapi-dev
    libcapstone-dev
    libcbor-dev
    libattr1-dev
    libfuse3-dev
    libjemalloc-dev
    libjson-c-dev
    libnuma-dev
    libpam0g-dev
    libsystemd-dev
    systemtap-sdt-dev
    libncursesw5-dev

    # Image (for VNC)
    libjpeg-turbo8-dev
    libpng-dev

    # Python extras (for tests and docs)
    python3-numpy
    python3-pillow
    python3-sphinx
    python3-sphinx-rtd-theme
    python3-yaml

    # Testing tools
    socat
    swtpm
    mtools
    xorriso

    # Development tools
    sparse
    coreutils
    diffutils
    findutils
)

echo ""
echo "=== Installing full build dependencies ==="
apt-get install -y "${FULL_PACKAGES[@]}"

# Some packages may not exist on all Ubuntu versions; install them best-effort
OPTIONAL_PACKAGES=(
    libpmem-dev
    libdaxctl-dev
    librbd-dev
    libglusterfs-dev
    libibverbs-dev
    librdmacm-dev
    libspice-protocol-dev
    libspice-server-dev
    libgtk-vnc-2.0-dev
    libvdeplug-dev
    libxdp-dev
    liblttng-ust-dev
    tesseract-ocr
    tesseract-ocr-eng
    vulkan-tools
)

echo ""
echo "=== Installing optional packages (failures are non-fatal) ==="
for pkg in "${OPTIONAL_PACKAGES[@]}"; do
    apt-get install -y "$pkg" 2>/dev/null || echo "  Skipped (unavailable): $pkg"
done

echo ""
echo "=== All dependencies installed successfully ==="
echo ""
echo "To build QEMU:"
echo "  mkdir build && cd build"
echo "  ../configure --target-list=x86_64-softmmu,aarch64-softmmu,arm-softmmu,ppc64-softmmu,riscv64-softmmu"
echo "  make -j\$(nproc)"
