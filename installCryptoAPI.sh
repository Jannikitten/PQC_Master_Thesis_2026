#!/bin/bash
set -e # Exit immediately if a command exits with a non-zero status.

# Define the root directory for all crypto libraries
CRYPTO_LIBS_ROOT="$HOME/crypto_libs"

# Helper function for colored output
info() {
    echo -e "\n\033[1;34m--- $1 ---\033[0m" # Blue color
}

error() {
    echo -e "\n\033[1;31mERROR: $1\033[0m" # Red color
    exit 1
}

check_command() {
    command -v "$1" >/dev/null 2>&1 || { error "Command '$1' is required but not installed. Aborting."; }
}

# --- 0. Prerequisites ---
info "Checking prerequisites..."
check_command "git"
check_command "cmake"
check_command "make"
check_command "autoreconf" # For wolfSSL's autogen.sh
check_command "nproc"      # For parallel make, if available

# Create the root directory for all crypto libraries
info "Creating root crypto libraries directory: $CRYPTO_LIBS_ROOT"
mkdir -p "$CRYPTO_LIBS_ROOT"

# Navigate to the root directory for cloning and building
cd "$CRYPTO_LIBS_ROOT" || error "Failed to change directory to $CRYPTO_LIBS_ROOT"

# --- wolfSSL ---
info "Installing wolfSSL (wolfCrypt)..."
WOLFSSL_REPO="https://github.com/wolfSSL/wolfssl.git"
if [ ! -d "wolfssl" ]; then
    git clone --depth=1 "$WOLFSSL_REPO"
else
    info "wolfssl directory already exists. Skipping clone, updating."
    cd wolfssl && git pull && cd ..
fi
cd wolfssl || error "Failed to change directory to wolfssl"
if [ -e "install" ]; then # Check if 'install' exists as a file or directory
    info "Removing existing 'install' directory/file in wolfssl..."
    rm -rf install
fi
mkdir -p install
./autogen.sh # Required to generate the configure script
# Configure with the correct install prefix and enable post-quantum, linking to liboqs
# The --with-liboqs path now correctly points to the liboqs installation within CRYPTO_LIBS_ROOT
./configure --prefix="$PWD/install" \
            --enable-kyber \
            --enable-dilithium \
            --enable-dtls \
            --enable-dtls13 \
            --enable-dtls-mtu \
            --enable-dtls-frag-ch
make -j$(nproc) # Use all available cores for make
make install
cd .. # Go back to CRYPTO_LIBS_ROOT

# --- Botan 3 ---
info "Installing Botan 3..."
BOTAN_REPO="https://github.com/randombit/botan.git"
if [ ! -d "botan" ]; then
    git clone --depth=1 "$BOTAN_REPO" botan # Clone into 'botan' directory
else
    info "botan directory already exists. Skipping clone, updating."
    cd botan && git pull && cd ..
fi
cd botan || error "Failed to change directory to botan"
mkdir -p build install # Create build and install directories inside botan
# Configure Botan 3 using its configure script
# Use --prefix to set the installation directory
# --with-build-dir is used by Botan to specify the build output directory
./configure.py --prefix="$PWD/install"
make -j$(nproc) # Use all available cores for make
make install
cd .. # Go back to CRYPTO_LIBS_ROOT

info "All specified cryptographic libraries have been cloned and installed in: $CRYPTO_LIBS_ROOT"