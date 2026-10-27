#!/usr/bin/env bash
# Builds TriAevum XR Test: native libraries with CMake + Ninja (NDK r29),
# staged as jniLibs, then the APK with Gradle. Outputs in $TRIAEVUM_HELLO_DIST
# (default dist/ under the repository root):
#   TriAevum-XR-Test.apk   signed when TRIAEVUM_QUEST_KEYSTORE_FILE is set
#   libprobe.so            the side-load probe, pushed separately (never in the APK)
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd -- "$here/../../.." && pwd)"
sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:?Set ANDROID_HOME}}"
ndk="${ANDROID_NDK_HOME:-$sdk/ndk/29.0.14206865}"
build="${TRIAEVUM_HELLO_BUILD:-$root/build/quest-hello}"
dist="${TRIAEVUM_HELLO_DIST:-$root/dist}"
gradle="${GRADLE:-gradle}"
test -f "$ndk/build/cmake/android.toolchain.cmake"

cmake -S "$here/native" -B "$build/native" -G Ninja \
  "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-32 \
  -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release
cmake --build "$build/native" --parallel --target triaevum_hello triaevum_probe

# jniLibs: everything the app loads. libprobe.so stays out on purpose.
stage="$build/jniLibs"
rm -rf "$stage"
mkdir -p "$stage/arm64-v8a"
for lib in libtriaevum_hello.so libSDL2.so libopenxr_loader.so; do
  cp "$build/native/lib/$lib" "$stage/arm64-v8a/"
done
cxx="$(find "$ndk/toolchains/llvm/prebuilt" -path '*/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so' | head -n 1)"
cp "$cxx" "$stage/arm64-v8a/"
ls -l "$stage/arm64-v8a"

sdl_source="$(cat "$build/native/sdl-source-dir.txt")"
"$gradle" -p "$here" --no-daemon :app:assembleRelease \
  "-PhelloSdlSource=$sdl_source" "-PhelloNativeStage=$stage"

mkdir -p "$dist"
cp "$here/app/build/outputs/apk/release/"*.apk "$dist/TriAevum-XR-Test.apk"
cp "$build/native/lib/libprobe.so" "$dist/libprobe.so"
ls -l "$dist"
