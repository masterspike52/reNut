#!/bin/sh
# Configures the Android (arm64-v8a) build of reNut in ../out/build/android:
# librenut.so, which build.sh packs into the APK.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
renut=$(dirname "$here")
: "${ANDROID_SDK:=$HOME/Android/Sdk}"
: "${ANDROID_NDK:=$ANDROID_SDK/ndk/29.0.14206865}"
# the rexglue-sdk source tree, as is (its development branch)
: "${REXSDK_DIR:=$(dirname "$renut")/rexglue-sdk}"
# codegen runs on the host: the x86 build's rexglue CLI (build it first)
: "${HOST_REXGLUE:=$REXSDK_DIR/out/linux-amd64/rexglue}"
cmake -S "$renut" -B "$renut/out/build/android" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-30 \
    -DANDROID_STL=c++_shared \
    -DCMAKE_BUILD_TYPE=Release \
    -DREXSDK_DIR="$REXSDK_DIR" \
    -DREXGLUE_USE_VULKAN=ON \
    -DREXGLUE_ENABLE_TRACY=OFF \
    -DREXGLUE_ENABLE_PERF_COUNTERS=OFF \
    -DPython3_EXECUTABLE=/usr/bin/python3 \
    -DREXGLUE_CODEGEN_EXECUTABLE="$HOST_REXGLUE" \
    "$@"
