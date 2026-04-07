#!/bin/bash
# Build renut inside a container for broad glibc compatibility.
# Produces a stripped binary at out/build/linux-amd64-relwithdebinfo/renut
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RENUT_DIR="$SCRIPT_DIR"
SDK_DIR="$(realpath "$RENUT_DIR/../rexglue-sdk")"
IMAGE_NAME="renut-builder"

echo "==> Building container image (cached)..."
docker build -t "$IMAGE_NAME" -f "$RENUT_DIR/Dockerfile.build" "$RENUT_DIR"

echo "==> Running containerized build..."
docker run --rm \
  -v "$RENUT_DIR:/src/reNut:rw" \
  -v "$SDK_DIR:/src/rexglue-sdk:rw" \
  -w /src/reNut \
  "$IMAGE_NAME" \
  bash -c '
    cmake --preset linux-amd64-relwithdebinfo 2>&1 && \
    cmake --build out/build/linux-amd64-relwithdebinfo --target renut -j$(nproc) 2>&1
  '

BINARY="$RENUT_DIR/out/build/linux-amd64-relwithdebinfo/renut"
if [ -f "$BINARY" ]; then
  echo "==> Build succeeded!"
  echo "    Binary: $BINARY"
  echo "    glibc requirement: $(objdump -T "$BINARY" 2>/dev/null | grep -oP 'GLIBC_\d+\.\d+' | sort -V | tail -1)"
else
  echo "==> Build failed!"
  exit 1
fi
