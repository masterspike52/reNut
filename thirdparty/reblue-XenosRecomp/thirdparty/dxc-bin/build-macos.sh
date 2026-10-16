#!/bin/bash
set -e

# Set up cleanup function
cleanup() {
  echo "Cleaning up resources..."
  if [ -d "DirectXShaderCompiler" ]; then
    echo "Removing cloned repository"
    rm -rf "DirectXShaderCompiler"
  fi
  if [ ! -z "$BUILD_DIR_UNIVERSAL" ] && [ -d "$BUILD_DIR_UNIVERSAL" ]; then
    echo "Removing build directory"
    rm -rf "$BUILD_DIR_UNIVERSAL"
  fi
}

# Ensure cleanup happens on exit (normal or error)
trap cleanup EXIT

# Default version if not specified
VERSION=${1:-"v1.7.2403"}
echo "Building DirectXShaderCompiler ${VERSION}..."

# Create temporary build directory
BUILD_DIR_UNIVERSAL=$(mktemp -d)

# Create output directory
mkdir -p outputs

# Clone repository with specific version
git clone -b ${VERSION} https://github.com/microsoft/DirectXShaderCompiler.git
cd DirectXShaderCompiler
git submodule update --init

# Build for Universal Binary (arm64 and x86_64)
echo "Building Universal Binary..."
cmake -B $BUILD_DIR_UNIVERSAL \
  -GNinja \
  -C./cmake/caches/PredefinedParams.cmake \
  -DSPIRV_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
ninja -C $BUILD_DIR_UNIVERSAL

# Copy binaries to output directory
echo "Copying binaries to outputs folder..."
cp "$BUILD_DIR_UNIVERSAL/bin/dxc" "../outputs/dxc-macos-universal"
cp "$BUILD_DIR_UNIVERSAL/lib/libdxcompiler.dylib" "../outputs/libdxcompiler.dylib"
chmod +x "../outputs/dxc-macos-universal"

# Fix RPATH in binaries to make them portable
echo "Fixing RPATH in binaries..."
install_name_tool -change "@rpath/zlib.net/v1.3.1/lib/libz.1.3.1.dylib" "/usr/lib/libz.1.dylib" "../outputs/dxc-macos-universal" || true

# Test compiled binaries
echo "Testing compiled binaries..."
cd ..
./outputs/dxc-macos-universal --help

echo "Build completed successfully!"
echo "Universal binary is available at: outputs/dxc-macos-universal"
echo "You can now manually copy the binary to your desired location."
