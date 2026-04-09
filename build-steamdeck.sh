#!/bin/bash
# build-steamdeck.sh — fetch and compile reNut on Steam Deck
#
# Run in Desktop Mode. Will disable the read-only filesystem temporarily
# to install build tools, then restore it.
#
# Usage:
#   curl -sSL https://raw.githubusercontent.com/masterspike52/reNut/Linux/build-steamdeck.sh | bash
#   -- or --
#   bash build-steamdeck.sh

set -eu

RENUT_REPO="https://github.com/masterspike52/reNut.git"
SDK_REPO="https://github.com/etonedemid/rexglue-sdk.git"
INSTALL_DIR="$HOME/reNut"
CMAKE_VERSION="3.27.9"

# ── Colours ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[reNut]${NC} $*"; }
warn()  { echo -e "${YELLOW}[reNut]${NC} $*"; }
error() { echo -e "${RED}[reNut]${NC} $*" >&2; exit 1; }

# ── Sanity checks ─────────────────────────────────────────────────────────────
[ "$(uname -m)" = "x86_64" ] || error "This script is for x86_64 (Steam Deck) only."
if ! grep -qi "steamos\|arch" /etc/os-release 2>/dev/null; then
    warn "Not detected as SteamOS/Arch — proceeding anyway, but package names may differ."
fi

# ── Step 1: install build deps via pacman ─────────────────────────────────────
info "Installing build dependencies…"

READONLY_DISABLED=0
if command -v steamos-readonly &>/dev/null && steamos-readonly status 2>/dev/null | grep -q "enabled"; then
    warn "SteamOS read-only filesystem detected. Disabling temporarily (will re-enable after)."
    sudo steamos-readonly disable
    READONLY_DISABLED=1
fi

# Initialise pacman keyring and trust SteamOS signing keys.
# SteamOS packages are signed by Valve's CI key (holo keyring), not the
# standard Arch keyring — both must be populated or pacman rejects packages.
info "Refreshing pacman keyring (Arch + SteamOS)…"
sudo pacman-key --init
sudo pacman-key --populate archlinux holo

sudo pacman -Sy --noconfirm --needed \
    base-devel \
    clang \
    lld \
    ninja \
    cmake \
    pkg-config \
    python3 \
    git \
    gtk3 \
    libx11 \
    libxcb \
    libpipewire \
    vulkan-headers \
    vulkan-icd-loader \
    linux-api-headers

if [ "$READONLY_DISABLED" -eq 1 ]; then
    info "Re-enabling SteamOS read-only filesystem."
    sudo steamos-readonly enable
fi

# ── Step 2: check cmake version (need ≥ 3.25) ────────────────────────────────
CMAKE_OK=0
if command -v cmake &>/dev/null; then
    CMAKE_VER=$(cmake --version | awk 'NR==1{print $3}')
    MAJOR=$(echo "$CMAKE_VER" | cut -d. -f1)
    MINOR=$(echo "$CMAKE_VER" | cut -d. -f2)
    if [ "$MAJOR" -gt 3 ] || { [ "$MAJOR" -eq 3 ] && [ "$MINOR" -ge 25 ]; }; then
        CMAKE_OK=1
    fi
fi

if [ "$CMAKE_OK" -eq 0 ]; then
    info "System CMake is too old. Downloading CMake ${CMAKE_VERSION} to ~/cmake…"
    mkdir -p "$HOME/cmake"
    curl -sSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.sh" \
        -o /tmp/cmake-install.sh
    chmod +x /tmp/cmake-install.sh
    /tmp/cmake-install.sh --skip-license --prefix="$HOME/cmake"
    rm /tmp/cmake-install.sh
    export PATH="$HOME/cmake/bin:$PATH"
    info "CMake installed to ~/cmake/bin — add the following to your ~/.bashrc to make it permanent:"
    echo '  export PATH="$HOME/cmake/bin:$PATH"'
fi

# ── Step 3: clone repos ───────────────────────────────────────────────────────
if [ -d "$INSTALL_DIR/.git" ]; then
    info "reNut already cloned — pulling latest changes…"
    git -C "$INSTALL_DIR" pull --ff-only
else
    info "Cloning reNut (Linux branch)…"
    git clone --branch Linux --depth 1 "$RENUT_REPO" "$INSTALL_DIR"
fi

SDK_DIR="$(dirname "$INSTALL_DIR")/rexglue-sdk"
if [ -d "$SDK_DIR/.git" ]; then
    info "rexglue-sdk already cloned — pulling latest changes…"
    git -C "$SDK_DIR" pull --ff-only
else
    info "Cloning rexglue-sdk…"
    git clone --depth 1 "$SDK_REPO" "$SDK_DIR"
fi

# ── Step 4: configure ─────────────────────────────────────────────────────────
info "Configuring build…"
# SteamOS ships a patched Clang (holo repo) that may not auto-detect the
# freshly-installed GCC sysroot. Passing --gcc-toolchain=/usr makes clang
# find /usr/lib/gcc/x86_64-pc-linux-gnu/... and thus /usr/include/ so that
# cmake feature-detection tests (check_include_file, check_symbol_exists)
# succeed for standard libc headers.
cmake -S "$INSTALL_DIR" \
      -B "$INSTALL_DIR/out/build/linux-amd64-relwithdebinfo" \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      "-DCMAKE_C_FLAGS=--gcc-toolchain=/usr" \
      "-DCMAKE_CXX_FLAGS=--gcc-toolchain=/usr" \
      -DREXSDK_DIR="$SDK_DIR"

# ── Step 5: build ─────────────────────────────────────────────────────────────
JOBS=$(nproc)
info "Building with $JOBS cores — this will take several minutes…"
cmake --build "$INSTALL_DIR/out/build/linux-amd64-relwithdebinfo" \
      --target renut \
      -j "$JOBS"

# ── Done ──────────────────────────────────────────────────────────────────────
BINARY="$INSTALL_DIR/out/build/linux-amd64-relwithdebinfo/renut"
info "Build complete!"
echo ""
echo -e "  Binary:    ${GREEN}${BINARY}${NC}"
echo -e "  Launcher:  ${GREEN}${INSTALL_DIR}/out/build/linux-amd64-relwithdebinfo/launch.sh${NC}"
echo ""
info "Run launch.sh and point it at your Banjo-Kazooie: Nuts & Bolts (US) ISO."
