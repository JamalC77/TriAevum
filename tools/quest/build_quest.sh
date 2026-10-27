#!/usr/bin/env bash
# Title-free Quest build: runtime libraries, adb tools, APK. Used by .github/workflows/quest.yml.
# Never builds, packages or uploads the real title module or game data.
#
#   ANDROID_NDK_HOME  Android NDK 29.0.14206865 (same revision as the private title build)
#   ANDROID_HOME      Android SDK (platforms;android-35, build-tools;35.0.0)
#   QUEST_OUT         build root (default: out/quest)
#   QUEST_JOBS        parallel jobs (default: nproc)
#   TRIAEVUM_ANDROID_DEPENDENCY_SOURCE_CACHE  optional FetchContent source cache (*-src trees)
#   TRIAEVUM_QUEST_KEYSTORE_FILE + _KEYSTORE_PASSWORD/_KEY_ALIAS/_KEY_PASSWORD  release signing
#   QUEST_SKIP_APK=1  stop after staging (no JDK/Gradle needed)
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
ndk="${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK 29.0.14206865}"
out="${QUEST_OUT:-$root/out/quest}"
jobs="${QUEST_JOBS:-$(nproc 2>/dev/null || echo 4)}"
python="${PYTHON:-python3}"
dist="$out/dist"
mkdir -p "$out" "$dist"

grep -q 'Pkg.Revision = 29.0.14206865' "$ndk/source.properties" || {
  echo "error: the Quest runtime and the title must share NDK 29.0.14206865" >&2; exit 1; }

echo "=== Title ABI closure guard ==="
"$python" "$root/tools/quest/title_abi.py" check --root "$root"
"$python" "$root/tools/quest/title_abi.py" write --root "$root" --ndk "$ndk" --output "$out/title-abi.json"

common=(
  -G Ninja
  "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake"
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-32 -DANDROID_STL=c++_shared
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
)
[[ -n "${CMAKE_MAKE_PROGRAM:-}" ]] && common+=("-DCMAKE_MAKE_PROGRAM=$CMAKE_MAKE_PROGRAM")
[[ -n "${CMAKE_C_COMPILER_LAUNCHER:-}" ]] && common+=("-DCMAKE_C_COMPILER_LAUNCHER=$CMAKE_C_COMPILER_LAUNCHER")
[[ -n "${CMAKE_CXX_COMPILER_LAUNCHER:-}" ]] && common+=("-DCMAKE_CXX_COMPILER_LAUNCHER=$CMAKE_CXX_COMPILER_LAUNCHER")

echo "=== Runtime (libTriAevum.so, SDL2, title bootstrap, runtime tests) ==="
rt_args=("${common[@]}")
[[ -n "${TRIAEVUM_ANDROID_DEPENDENCY_SOURCE_CACHE:-}" ]] &&
  rt_args+=("-DTRIAEVUM_ANDROID_DEPENDENCY_SOURCE_CACHE=$TRIAEVUM_ANDROID_DEPENDENCY_SOURCE_CACHE")
cmake -S "$root/ports/quest/runtime" -B "$out/rt" "${rt_args[@]}"
cmake --build "$out/rt" --parallel "$jobs" --target \
  oot3d_native_game triaevum_title_aot_stub \
  oot3d_native_a32_vfp_fast_tests oot3d_native_a32_runtime_tests

echo "=== Title-free probes (ports/android/native, synthetic title) ==="
"$python" "$root/tools/quest/synthetic_title.py" --output "$out/synthetic"
cmake -S "$root/ports/android/native" -B "$out/native" "${common[@]}" \
  "-DTRIAEVUM_TRANSLATED_TITLE_DIR=$out/synthetic/translated"
cmake --build "$out/native" --parallel "$jobs" --target \
  triaevum_module native_presentation_policy_tests triaevum_android_abi_probe triaevum_android_nri_probe \
  oot3d_native_direct_aot_tests triaevum_probe_empty_title triaevum_runtime_layout_tests \
  triaevum_input_service_tests triaevum_audio_service_tests triaevum_filesystem_service_tests \
  triaevum_native_module_loader_tests triaevum_mock_module triaevum_android_title

echo "=== Staging ==="
"$python" "$root/tools/quest/stage_quest.py" --build "$out" --ndk "$ndk" \
  --title-abi "$out/title-abi.json" --jni "$out/jni" --assets "$out/assets" \
  --tools-zip "$dist/quest-tools-arm64.zip"
cp "$out/title-abi.json" "$dist/title-abi.json"

if [[ "${QUEST_SKIP_APK:-0}" == 1 ]]; then
  echo "QUEST_SKIP_APK=1: native staging done in $out"
  exit 0
fi

echo "=== APK ==="
# The SDL Java sources must be exactly those of the SDL build (a cached source tree when one is used).
sdl_source="$(sed -n 's/^SDL2_SOURCE_DIR:STATIC=//p' "$out/rt/CMakeCache.txt")"
test -f "$sdl_source/android-project/app/src/main/java/org/libsdl/app/SDLActivity.java"
gradle_args=(--no-daemon -p "$root/ports/quest"
  "-PtriaevumSdlSource=$sdl_source"
  "-PtriaevumNativeStage=$out/jni"
  "-PtriaevumQuestAssets=$out/assets")
"${GRADLE:-gradle}" "${gradle_args[@]}" :app:testReleaseUnitTest :app:assembleRelease
apk="$root/ports/quest/app/build/outputs/apk/release/app-release.apk"
test -f "$apk"
cp "$apk" "$dist/TriAevum-Quest-arm64.apk"
apksigner="$(ls -d "${ANDROID_HOME:?}"/build-tools/*/apksigner 2>/dev/null | sort -V | tail -1)"
if [[ -n "$apksigner" ]]; then
  "$apksigner" verify "$dist/TriAevum-Quest-arm64.apk"
  echo "APK signature verifies"
fi
ls -l "$dist"
