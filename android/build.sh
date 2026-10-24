#!/bin/sh
# Builds librenut.so (configure.sh first) and packs it into
# app/build/outputs/apk/release/app-release.apk. "install" also installs it.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
renut=$(dirname "$here")
: "${ANDROID_SDK:=$HOME/Android/Sdk}"
: "${ANDROID_NDK:=$ANDROID_SDK/ndk/29.0.14206865}"
: "${RENUT_ANDROID_OUT:=$renut/out/build/android}"
out=$RENUT_ANDROID_OUT
ninja -C "$out" renut
tools="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64"
libs="$here/app/libs/arm64-v8a"
mkdir -p "$libs"
"$tools/bin/llvm-strip" --strip-unneeded -o "$libs/librenut.so" "$out/librenut.so"
cp "$tools/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$libs/"
# the SDK's runtime, a library of its own, built into the SDK's out/ folder
sdk=$(sed -n 's/^REXSDK_DIR:PATH=//p' "$out/CMakeCache.txt")
"$tools/bin/llvm-strip" --strip-unneeded -o "$libs/librexruntime.so" "$sdk/out/linux-arm64/librexruntime.so"
# adrenotools' driver hooks: dlopen'd from the app's native library folder
for hook in "$out"/thirdparty/libadrenotools/src/hook/*.so; do
    [ -e "$hook" ] && cp "$hook" "$libs/"
done
cd "$here"
export ANDROID_HOME="$ANDROID_SDK"
./gradlew --no-daemon -q assembleRelease
apk="$here/app/build/outputs/apk/release/app-release.apk"
echo "$apk"
if [ "${1:-}" = install ]; then adb install -r "$apk"; fi
