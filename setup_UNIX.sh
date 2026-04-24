#!/usr/bin/env bash
# =============================================================================
#  setup.sh  –  Bootstrap script for PQC_Master_Thesis_2026
#  Supports: macOS (Homebrew) · Debian/Ubuntu · Fedora/RHEL · Arch Linux
#
#  Usage:
#    chmod +x setup.sh && ./setup.sh
#
#  Optional environment variables:
#    BUILD_TYPE=Debug    (default: Release)
#    JOBS=4              (default: all logical cores)
# =============================================================================
set -euo pipefail

# ── Colour helpers ─────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'

info()    { echo -e "${CYAN}[INFO]${RESET}  $*"; }
success() { echo -e "${GREEN}[ OK ]${RESET}  $*"; }
warn()    { echo -e "${YELLOW}[WARN]${RESET}  $*"; }
error()   { echo -e "${RED}[ERR ]${RESET}  $*" >&2; exit 1; }
header()  {
    echo -e "\n${BOLD}${CYAN}══════════════════════════════════════════════════${RESET}"
    echo -e   "${BOLD}${CYAN}  $*${RESET}"
    echo -e   "${BOLD}${CYAN}══════════════════════════════════════════════════${RESET}"
}

# ── Detect OS ──────────────────────────────────────────────────────────────────
OS=""
if [[ "$OSTYPE" == "darwin"* ]]; then
    OS="macos"
elif [[ -f /etc/debian_version ]]; then
    OS="debian"
elif [[ -f /etc/fedora-release ]] || [[ -f /etc/redhat-release ]]; then
    OS="fedora"
elif [[ -f /etc/arch-release ]]; then
    OS="arch"
else
    OS="unknown"
    warn "Unrecognised Linux distro – skipping package installation."
    warn "Ensure CMake 4.1+, C++23 compiler, Python 3, OpenSSL, and the Vulkan SDK are present."
fi

# ── Resolve paths ──────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)}"

header "PQC Master Thesis 2026 – Setup"
info "Platform  : $OS"
info "Build type: $BUILD_TYPE"
info "Parallel  : $JOBS jobs"
info "Source    : $SCRIPT_DIR"
info "Build dir : $BUILD_DIR"

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 1 – SYSTEM DEPENDENCIES
# ═══════════════════════════════════════════════════════════════════════════════
header "Step 1 – System dependencies"

_check_cmake_version() {
    if command -v cmake &>/dev/null; then
        CMAKE_VER=$(cmake --version | head -1 | awk '{print $3}')
        CMAKE_MAJOR=$(echo "$CMAKE_VER" | cut -d. -f1)
        if [[ "$CMAKE_MAJOR" -lt 4 ]]; then
            warn "CMake $CMAKE_VER found, but project requires ≥ 4.1. Upgrading via pip…"
            pip3 install --upgrade cmake --break-system-packages 2>/dev/null \
                || pip3 install --upgrade cmake \
                || error "Failed to upgrade CMake. Please install CMake 4.1+ manually from https://cmake.org/download/"
            # Prepend pip's bin so the new cmake is found first
            PIP_BIN="$(python3 -m site --user-base 2>/dev/null)/bin"
            export PATH="${PIP_BIN}:$PATH"
            success "CMake upgraded → $(cmake --version | head -1)"
        else
            success "CMake $CMAKE_VER ✓"
        fi
    else
        error "CMake not found. Please install CMake 4.1+ from https://cmake.org/download/"
    fi
}

install_macos() {
    if ! command -v brew &>/dev/null; then
        warn "Homebrew not found – installing…"
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
        # shellcheck disable=SC2016
        eval "$(/opt/homebrew/bin/brew shellenv 2>/dev/null || /usr/local/bin/brew shellenv)"
    fi

    info "Updating Homebrew…"
    brew update

    for pkg in cmake python3 openssl ninja; do
        brew list "$pkg" &>/dev/null && success "$pkg already installed" || { info "Installing $pkg…"; brew install "$pkg"; }
    done

    # Vulkan on macOS: install loader + headers via Homebrew formulae.
    # The LunarG 'vulkan-sdk' cask needs interactive sudo and frequently fails
    # in CI; the formulae below give find_package(Vulkan) everything it needs
    # (libvulkan.dylib, vulkan/vulkan.h) plus MoltenVK as the Metal ICD.
    for pkg in vulkan-headers vulkan-loader molten-vk glslang; do
        brew list "$pkg" &>/dev/null \
            && success "$pkg already installed" \
            || { info "Installing $pkg…"; brew install "$pkg"; }
    done

    _check_cmake_version
}

install_debian() {
    info "Running apt-get update…"
    sudo apt-get update -y

    sudo apt-get install -y \
        build-essential git cmake python3 python3-pip ninja-build pkg-config \
        libssl-dev \
        libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
        libwayland-dev libxkbcommon-dev

    # Vulkan from the stock Ubuntu repositories — simpler and more reliable
    # than mixing the LunarG repo, which frequently breaks apt-get on newer
    # Ubuntu codenames (e.g. noble) when only a jammy list is available.
    if ! dpkg -s libvulkan-dev &>/dev/null; then
        info "Installing Vulkan from the Ubuntu repositories…"
        sudo apt-get install -y \
            libvulkan-dev vulkan-tools glslang-tools spirv-tools
    else
        success "libvulkan-dev already installed"
    fi

    _check_cmake_version
}

install_fedora() {
    sudo dnf install -y \
        gcc gcc-c++ git cmake python3 ninja-build pkg-config \
        openssl-devel \
        libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel \
        wayland-devel libxkbcommon-devel \
        vulkan-devel vulkan-tools glslang spirv-tools

    _check_cmake_version
}

install_arch() {
    sudo pacman -Syu --noconfirm
    sudo pacman -S --noconfirm --needed \
        base-devel git cmake python3 ninja pkg-config \
        openssl \
        libx11 libxrandr libxinerama libxcursor libxi \
        vulkan-devel vulkan-tools glslang spirv-tools

    _check_cmake_version
}

case "$OS" in
    macos)   install_macos   ;;
    debian)  install_debian  ;;
    fedora)  install_fedora  ;;
    arch)    install_arch    ;;
    unknown) _check_cmake_version || true ;;
esac

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 2 – GIT SUBMODULES
# ═══════════════════════════════════════════════════════════════════════════════
header "Step 2 – Git submodules"

cd "$SCRIPT_DIR"

# .git is a directory in a normal clone, a file in a git worktree.
if [[ ! -e .git ]]; then
    error "This directory doesn't look like a git repository root. Clone the repo first."
fi

info "Initialising and updating all submodules…"
# --no-recommend-shallow disables the shallow=true hint from .gitmodules, which
# can otherwise fail when the recorded commit is not at the tip of the branch
# (a common cause of empty submodule directories).
if ! git submodule update --init --recursive --progress --no-recommend-shallow; then
    warn "Submodule update failed. Retrying once with deinit + init…"
    git submodule deinit -f --all || true
    git submodule update --init --recursive --progress --no-recommend-shallow \
        || error "Failed to clone submodules. Check your internet connection and run: git submodule update --init --recursive --no-recommend-shallow"
fi

# Verify the submodules whose files are required by the build actually landed.
# glm / spdlog / yaml-cpp are pulled via CMake FetchContent, not submodules.
REQUIRED_FILES=(
    "imgui/imgui.h"
    "botan/configure.py"
    "wolfssl/CMakeLists.txt"
)
for f in "${REQUIRED_FILES[@]}"; do
    [[ -f "$f" ]] || error "Submodule file missing: $f (submodule init did not populate its parent). Try: git submodule update --init --recursive --force"
done
success "Submodules ready"

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 3 – BUILD BOTAN 3
# ═══════════════════════════════════════════════════════════════════════════════
header "Step 3 – Building Botan 3 (PQC algorithms)"

BOTAN_DIR="${SCRIPT_DIR}/botan"
BOTAN_INSTALL="${BOTAN_DIR}/install"

[[ -d "$BOTAN_DIR" ]] || error "botan/ submodule not found. Make sure it's listed in .gitmodules and run: git submodule update --init --recursive"

if [[ -f "${BOTAN_INSTALL}/lib/libbotan-3.a" ]] || \
   [[ -f "${BOTAN_INSTALL}/lib/libbotan-3.so" ]] || \
   [[ -f "${BOTAN_INSTALL}/lib/libbotan-3.dylib" ]]; then
    success "Botan already built – skipping (delete botan/install/ to force rebuild)"
else
    info "Configuring Botan with ML-KEM and ML-DSA…"
    cd "$BOTAN_DIR"

    BOTAN_CC_FLAG=""
    [[ "$OS" == "macos" ]] && BOTAN_CC_FLAG="--cc=clang"

    # NOTE: Botan 3 has no `--with-tls` flag — TLS is built in by default.
    # `--enable-modules=…` explicitly pulls in ML-KEM and ML-DSA on top of defaults.
    # shellcheck disable=SC2086
    python3 configure.py \
        --prefix="${BOTAN_INSTALL}" \
        --enable-modules=ml_kem,ml_dsa \
        ${BOTAN_CC_FLAG}

    info "Building Botan with $JOBS parallel jobs (this takes a few minutes)…"
    make -j"$JOBS"

    info "Installing Botan to ${BOTAN_INSTALL}…"
    make install

    success "Botan built and installed → ${BOTAN_INSTALL}"
    cd "$SCRIPT_DIR"
fi

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 4 – CMAKE CONFIGURE
# ═══════════════════════════════════════════════════════════════════════════════
header "Step 4 – CMake configure"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

EXTRA_CMAKE_ARGS=()

# On macOS, Homebrew's OpenSSL isn't in the default search path
if [[ "$OS" == "macos" ]]; then
    OPENSSL_PREFIX="$(brew --prefix openssl 2>/dev/null || true)"
    [[ -n "$OPENSSL_PREFIX" ]] && EXTRA_CMAKE_ARGS+=("-DOPENSSL_ROOT_DIR=${OPENSSL_PREFIX}")
fi

cmake "$SCRIPT_DIR" \
    -G "Ninja" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBotan_ROOT="${BOTAN_INSTALL}" \
    "${EXTRA_CMAKE_ARGS[@]}"

success "CMake configuration complete"

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 5 – BUILD
# ═══════════════════════════════════════════════════════════════════════════════
header "Step 5 – Building"

cmake --build . --parallel "$JOBS"

# ── Done ──────────────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}${GREEN}╔══════════════════════════════════════════════╗${RESET}"
echo -e "${BOLD}${GREEN}║          Build successful! 🎉                ║${RESET}"
echo -e "${BOLD}${GREEN}╚══════════════════════════════════════════════╝${RESET}"
echo ""
echo -e "  Client : ${BOLD}${BUILD_DIR}/SafiraClient${RESET}"
echo -e "  Server : ${BOLD}${BUILD_DIR}/SafiraServer${RESET}"
echo ""
echo -e "${YELLOW}Tips:${RESET}"
echo -e "  • Rebuild after changes : ${BOLD}cmake --build ${BUILD_DIR}${RESET}"
echo -e "  • Debug build           : ${BOLD}BUILD_TYPE=Debug ./setup.sh${RESET}"
echo -e "  • Rebuild Botan         : ${BOLD}rm -rf botan/install && ./setup.sh${RESET}"