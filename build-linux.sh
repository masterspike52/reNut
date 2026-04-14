#!/bin/bash
# build-linux.sh — install dependencies, clone, and compile reNut on Linux
#
# Supports Arch/Manjaro/SteamOS, Ubuntu/Debian, and Fedora/RHEL.
# Usage: bash build-linux.sh

set -eu

RENUT_REPO="https://github.com/etonedemid/reNut.git"
SDK_REPO="https://github.com/etonedemid/rexglue-sdk.git"
INSTALL_DIR="$HOME/reNut"
CMAKE_VERSION="3.27.9"
BUILD_PRESET="linux-amd64-relwithdebinfo"

# ── Colours ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[reNut]${NC} $*"; }
warn()  { echo -e "${YELLOW}[reNut]${NC} $*"; }
error() { echo -e "${RED}[reNut]${NC} $*" >&2; exit 1; }

# ── Detect distro ─────────────────────────────────────────────────────────────
detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        case "$ID" in
            arch|manjaro|endeavouros) echo "arch" ;;
            steamos)                  echo "steamos" ;;
            ubuntu|debian|linuxmint|pop) echo "debian" ;;
            fedora|rhel|centos|nobara) echo "fedora" ;;
            opensuse*|sles)           echo "suse" ;;
            *)
                # Check ID_LIKE as fallback
                case "${ID_LIKE:-}" in
                    *arch*)   echo "arch" ;;
                    *debian*) echo "debian" ;;
                    *fedora*|*rhel*) echo "fedora" ;;
                    *suse*)   echo "suse" ;;
                    *)        echo "unknown" ;;
                esac
                ;;
        esac
    else
        echo "unknown"
    fi
}

# ── Sanity checks ─────────────────────────────────────────────────────────────
[ "$(uname -m)" = "x86_64" ] || error "This script is for x86_64 only."

DISTRO=$(detect_distro)
info "Detected distro family: $DISTRO"

# ── Step 1: install build dependencies ────────────────────────────────────────
install_deps_arch() {
    # Handle SteamOS read-only filesystem
    READONLY_DISABLED=0
    if command -v steamos-readonly &>/dev/null && steamos-readonly status 2>/dev/null | grep -q "enabled"; then
        warn "SteamOS read-only filesystem detected. Disabling temporarily."
        sudo steamos-readonly disable
        READONLY_DISABLED=1
    fi

    if [ "$DISTRO" = "steamos" ]; then
        info "Refreshing pacman keyring…"
        sudo pacman-key --init
        sudo pacman-key --populate archlinux holo
        sudo sed -i 's/^NoExtract/#NoExtract/g' /etc/pacman.conf
    fi

    # Full list of desired packages
    local ALL_PKGS=(
        base-devel glibc linux-api-headers clang lld ninja cmake pkgconf
        python3 git gtk3 pango harfbuzz fontconfig freetype2 cairo
        gdk-pixbuf2 glib2 at-spi2-core libx11 libxcb libpipewire
        vulkan-headers vulkan-icd-loader alsa-lib libpulse libusb
        libunwind dbus ibus systemd systemd-libs
    )

    # Filter to only packages that are not already installed
    local MISSING=()
    for pkg in "${ALL_PKGS[@]}"; do
        if ! pacman -Qi "$pkg" &>/dev/null; then
            MISSING+=("$pkg")
        fi
    done

    if [ ${#MISSING[@]} -eq 0 ]; then
        info "All dependencies already installed — nothing to do."
    else
        info "Installing ${#MISSING[@]} missing package(s): ${MISSING[*]}"
        sudo pacman -S --needed --noconfirm "${MISSING[@]}"
    fi

    # Ensure pkg-config can find .pc files
    export PKG_CONFIG_PATH="/usr/lib/pkgconfig:/usr/share/pkgconfig:${PKG_CONFIG_PATH:-}"

    # Generate shims for libsystemd/libudev if missing (common on SteamOS)
    _create_pc_shim "libsystemd" "-lsystemd"
    _create_pc_shim "libudev"    "-ludev"

    if [ "$READONLY_DISABLED" -eq 1 ]; then
        info "Re-enabling SteamOS read-only filesystem."
        sudo steamos-readonly enable
    fi
}

install_deps_debian() {
    local ALL_PKGS=(
        build-essential clang lld ninja-build cmake pkg-config python3 git
        libgtk-3-dev libpango1.0-dev libharfbuzz-dev libfontconfig-dev
        libfreetype-dev libcairo2-dev libgdk-pixbuf-2.0-dev libglib2.0-dev
        libatspi2.0-dev libx11-dev libxcb1-dev libpipewire-0.3-dev
        vulkan-headers libvulkan-dev libasound2-dev libpulse-dev
        libusb-1.0-0-dev libunwind-dev libdbus-1-dev libibus-1.0-dev
        libsystemd-dev libudev-dev
    )

    local MISSING=()
    for pkg in "${ALL_PKGS[@]}"; do
        if ! dpkg -s "$pkg" &>/dev/null; then
            MISSING+=("$pkg")
        fi
    done

    if [ ${#MISSING[@]} -eq 0 ]; then
        info "All dependencies already installed — nothing to do."
    else
        info "Installing ${#MISSING[@]} missing package(s)…"
        sudo apt-get update
        sudo apt-get install -y "${MISSING[@]}"
    fi
}

install_deps_fedora() {
    local ALL_PKGS=(
        @development-tools clang lld ninja-build cmake pkgconf python3 git
        gtk3-devel pango-devel harfbuzz-devel fontconfig-devel freetype-devel
        cairo-devel gdk-pixbuf2-devel glib2-devel at-spi2-core-devel
        libX11-devel libxcb-devel pipewire-devel vulkan-headers
        vulkan-loader-devel alsa-lib-devel pulseaudio-libs-devel
        libusb1-devel libunwind-devel dbus-devel ibus-devel systemd-devel
    )

    local MISSING=()
    for pkg in "${ALL_PKGS[@]}"; do
        [[ "$pkg" == @* ]] && { MISSING+=("$pkg"); continue; }
        if ! rpm -q "$pkg" &>/dev/null; then
            MISSING+=("$pkg")
        fi
    done

    if [ ${#MISSING[@]} -eq 0 ]; then
        info "All dependencies already installed — nothing to do."
    else
        info "Installing ${#MISSING[@]} missing package(s)…"
        sudo dnf install -y "${MISSING[@]}"
    fi
}

install_deps_suse() {
    local ALL_PKGS=(
        clang lld ninja cmake pkgconf python3 git
        gtk3-devel pango-devel harfbuzz-devel fontconfig-devel freetype2-devel
        cairo-devel gdk-pixbuf-devel glib2-devel at-spi2-core-devel
        libX11-devel libxcb-devel pipewire-devel vulkan-headers
        libvulkan1 vulkan-devel alsa-devel libpulse-devel
        libusb-1_0-devel libunwind-devel dbus-1-devel ibus-devel
        systemd-devel libudev-devel
    )

    local MISSING=()
    for pkg in "${ALL_PKGS[@]}"; do
        if ! rpm -q "$pkg" &>/dev/null; then
            MISSING+=("$pkg")
        fi
    done

    # Always ensure devel_basis pattern
    if [ ${#MISSING[@]} -eq 0 ]; then
        info "All dependencies already installed — nothing to do."
    else
        info "Installing ${#MISSING[@]} missing package(s)…"
        sudo zypper install -y -t pattern devel_basis
        sudo zypper install -y "${MISSING[@]}"
    fi
}

_create_pc_shim() {
    local name="$1" libs="$2"
    if ! pkg-config --exists "$name" 2>/dev/null; then
        warn "${name}.pc not found — generating a shim…"
        local shim_dir="$HOME/.local/lib/pkgconfig"
        mkdir -p "$shim_dir"
        local ver
        ver=$(pacman -Q systemd 2>/dev/null | awk '{print $2}' | cut -d- -f1 || echo "255")
        cat > "$shim_dir/${name}.pc" <<EOF
prefix=/usr
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: ${name}
Description: ${name} (shim generated by build-linux.sh)
Version: ${ver}
Libs: -L\${libdir} ${libs}
Cflags: -I\${includedir}
EOF
        export PKG_CONFIG_PATH="$shim_dir:${PKG_CONFIG_PATH:-}"
        info "${name}.pc shim written to $shim_dir"
    fi
}

info "Installing build dependencies…"
case "$DISTRO" in
    arch|steamos) install_deps_arch ;;
    debian)       install_deps_debian ;;
    fedora)       install_deps_fedora ;;
    suse)         install_deps_suse ;;
    *)
        warn "Unsupported distro — skipping automatic dependency install."
        warn "Please install: clang lld ninja cmake git and the required dev libraries manually."
        warn "See BUILD-LINUX.md for the full list."
        ;;
esac

# ── Step 2: check cmake version (need >= 3.25) ───────────────────────────────
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
    info "System CMake is too old or missing. Downloading CMake ${CMAKE_VERSION}…"
    mkdir -p "$HOME/cmake"
    curl -sSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.sh" \
        -o /tmp/cmake-install.sh
    chmod +x /tmp/cmake-install.sh
    /tmp/cmake-install.sh --skip-license --prefix="$HOME/cmake"
    rm /tmp/cmake-install.sh
    export PATH="$HOME/cmake/bin:$PATH"
    info "CMake ${CMAKE_VERSION} installed to ~/cmake/bin"
    warn "Add to your shell profile to make permanent:  export PATH=\"\$HOME/cmake/bin:\$PATH\""
fi

# ── Step 3: clone repos ──────────────────────────────────────────────────────
# Helper: update (or clone) a shallow clone of a specific branch.
# Usage: git_update <dir> <repo> <branch>
git_update() {
    local dir="$1" repo="$2" branch="$3"
    if [ -d "$dir/.git" ]; then
        info "$(basename "$dir") already cloned — updating…"
        git -C "$dir" fetch --depth 1 origin "$branch"
        git -C "$dir" checkout "$branch" 2>/dev/null \
            || git -C "$dir" checkout -b "$branch" "origin/$branch"
        git -C "$dir" reset --hard "origin/$branch"
    else
        info "Cloning $(basename "$dir")…"
        git clone --depth 1 --branch "$branch" "$repo" "$dir"
    fi
}

RENUT_BRANCH="Linux"
git_update "$INSTALL_DIR" "$RENUT_REPO" "$RENUT_BRANCH"

SDK_DIR="$(dirname "$INSTALL_DIR")/rexglue-sdk"
SDK_BRANCH="main"
git_update "$SDK_DIR" "$SDK_REPO" "$SDK_BRANCH"

# ── Step 4: configure ────────────────────────────────────────────────────────
BUILD_DIR="$INSTALL_DIR/out/build/$BUILD_PRESET"

if [ -d "$BUILD_DIR" ]; then
    info "Clearing previous build…"
    rm -rf "$BUILD_DIR"
fi

info "Configuring ($BUILD_PRESET)…"
# Ensure pkg-config can find system .pc files (required for gtk3/pango/harfbuzz on SteamOS)
export PKG_CONFIG_PATH="/usr/lib/pkgconfig:/usr/share/pkgconfig:${PKG_CONFIG_PATH:-}"
cmake -S "$INSTALL_DIR" \
      -B "$BUILD_DIR" \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DREXSDK_DIR="$SDK_DIR"

# ── Step 5: build ────────────────────────────────────────────────────────────
JOBS=$(nproc)
info "Building with $JOBS cores…"
cmake --build "$BUILD_DIR" --target renut -j "$JOBS"

# ── Done ─────────────────────────────────────────────────────────────────────
info "Build complete!"
echo ""
echo -e "  Binary:   ${GREEN}${BUILD_DIR}/renut${NC}"
echo -e "  Launcher: ${GREEN}${BUILD_DIR}/launch.sh${NC}"
echo ""
info "Place your Banjo-Kazooie: Nuts & Bolts (US) ISO and run launch.sh."
