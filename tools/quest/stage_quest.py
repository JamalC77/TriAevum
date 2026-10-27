#!/usr/bin/env python3
"""Stage the title-free Quest APK libraries and the adb-runnable tools.

Never packages a title module or game data: the title is side-loaded (TitleInstaller) and data is
pushed by the user. Checks every library is AArch64, 16 KiB aligned and depends only on packaged or
platform libraries. With --openxr (the default build) the Khronos OpenXR loader is packaged for the
immersive activity and quest_xr_logic_tests joins the tools; --no-openxr is the panel-only build.
"""
import argparse
import platform
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

PLATFORM_LIBS = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libEGL.so", "libGLESv1_CM.so",
    "libGLESv2.so", "libGLESv3.so", "libvulkan.so", "libOpenSLES.so", "libaaudio.so", "libz.so",
}
APK_LIBS = {
    "libSDL2.so": "rt/_deps/sdl2-build/libSDL2.so",
    "libTriAevum.so": "rt/product/libTriAevum.so",
    "libtriaevum_title_bootstrap.so": "rt/product/libtriaevum_title_bootstrap.so",
}
# adb-runnable, title-free probes and tests (ports/android/native and the runtime tree).
TOOLS = [
    "native/triaevum_android_abi_probe",
    "native/abi/triaevum_probe_empty_title.so",
    "native/abi/oot3d_native_direct_aot_tests",
    "native/abi/triaevum_runtime_layout_tests",
    "native/native_presentation_policy_tests",
    "native/triaevum_android_nri_probe",
    "native/module/triaevum_input_service_tests",
    "native/module/triaevum_audio_service_tests",
    "native/module/triaevum_filesystem_service_tests",
    "native/module/triaevum_native_module_loader_tests",
    "native/module/triaevum_mock_module.so",
    "rt/product/oot3d_native_a32_vfp_fast_tests",
    "rt/product/oot3d_native_a32_runtime_tests",
]
# Immersive mode (QUEST_OPENXR=1): the loader from the AAR (ports/quest/runtime/CMakeLists.txt) and
# the chord/placement logic tests.
OPENXR_APK_LIBS = {"libopenxr_loader.so": "rt/openxr/libopenxr_loader.so"}
OPENXR_TOOLS = ["rt/product/quest_xr_logic_tests"]
# The loader's licence text from the same hash-pinned AAR (META-INF/LICENSE), shipped with it.
OPENXR_LICENSE = "rt/openxr/LICENSE.txt"
OPENXR_LICENSE_NAME = "openxr_loader.txt"
# The synthetic title: one generated function at 0x1000, built by the real Android title target.
SYNTHETIC_TITLE = "native/libtriaevum_title_aot.so"
TOOLS_README = """TriAevum Quest tools (arm64-v8a, title-free). Run on the headset as the shell user:

  adb push quest-tools-arm64 /data/local/tmp/
  adb shell 'cd /data/local/tmp/quest-tools-arm64 && sh run_all.sh'

run_all.sh reports PASS/FAIL per binary and exits non-zero on any failure.

synthetic/libtriaevum_title_aot.so is a synthetic ABI-v2 title module (one 6-instruction function,
no game code) built by the same Android title target as the real title. The ABI probe and the
direct-AOT loader test run against it. Pushed with title-abi.json into the app's incoming directory
it also exercises the APK's install and dlopen path (the game itself will then refuse to boot).
"""
RUN_ALL = """#!/system/bin/sh
cd "$(dirname "$0")" || exit 1
export LD_LIBRARY_PATH="$PWD:$LD_LIBRARY_PATH"
fail=0
run() {
  echo "== $*"
  "$@"; code=$?
  if [ $code -eq 0 ]; then echo "PASS $1"; elif [ $code -eq 77 ]; then echo "SKIP $1"; else echo "FAIL $1 ($code)"; fail=1; fi
}
chmod 755 ./*_tests ./*_probe 2>/dev/null
for t in native_presentation_policy_tests triaevum_runtime_layout_tests triaevum_input_service_tests          triaevum_audio_service_tests triaevum_filesystem_service_tests          oot3d_native_a32_vfp_fast_tests oot3d_native_a32_runtime_tests @EXTRA_TESTS@; do
  run ./$t
done
run ./triaevum_native_module_loader_tests ./triaevum_mock_module.so
run ./oot3d_native_direct_aot_tests ./triaevum_probe_empty_title.so --expect-empty
run ./oot3d_native_direct_aot_tests ./synthetic/libtriaevum_title_aot.so
run ./triaevum_android_abi_probe ./synthetic/libtriaevum_title_aot.so
run ./triaevum_android_nri_probe
[ $fail -eq 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
exit $fail
"""


def llvm_bin(ndk: Path, tool: str) -> Path:
    host = {"Windows": "windows-x86_64", "Darwin": "darwin-x86_64"}.get(platform.system(), "linux-x86_64")
    suffix = ".exe" if platform.system() == "Windows" else ""
    return ndk / "toolchains/llvm/prebuilt" / host / "bin" / f"{tool}{suffix}"


def libcxx(ndk: Path) -> Path:
    host = {"Windows": "windows-x86_64", "Darwin": "darwin-x86_64"}.get(platform.system(), "linux-x86_64")
    return ndk / "toolchains/llvm/prebuilt" / host / "sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"


def check_elf(ndk: Path, path: Path, allowed: set) -> list:
    readelf = llvm_bin(ndk, "llvm-readelf")
    header = subprocess.check_output([readelf, "-h", path], text=True)
    if "AArch64" not in header:
        raise ValueError(f"Not an ARM64 ELF: {path}")
    segments = subprocess.check_output([readelf, "-lW", path], text=True)
    for line in segments.splitlines():
        if line.strip().startswith("LOAD"):
            align = int(line.split()[-1], 16)
            if align < 0x4000:
                raise ValueError(f"{path.name}: LOAD alignment {align:#x} is below 16 KiB")
    dynamic = subprocess.check_output([readelf, "-d", path], text=True)
    needed = sorted(set(re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic)))
    missing = set(needed) - allowed
    if missing:
        raise ValueError(f"{path.name} needs unpackaged libraries: {sorted(missing)}")
    return needed


def copy_stripped(ndk: Path, source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    subprocess.run([llvm_bin(ndk, "llvm-strip"), "--strip-debug", target], check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True, help="build root holding rt/ and native/")
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--title-abi", type=Path, required=True)
    parser.add_argument("--jni", type=Path, required=True, help="output jniLibs directory")
    parser.add_argument("--assets", type=Path, required=True, help="output APK assets directory")
    parser.add_argument("--tools-zip", type=Path, required=True)
    parser.add_argument("--openxr", dest="openxr", action="store_true", default=True,
                        help="package the OpenXR loader and the XR logic tests (default)")
    parser.add_argument("--no-openxr", dest="openxr", action="store_false",
                        help="panel-only build (QUEST_OPENXR=0)")
    args = parser.parse_args()
    apk_libs = dict(APK_LIBS)
    tools = list(TOOLS)
    if args.openxr:
        apk_libs.update(OPENXR_APK_LIBS)
        tools += OPENXR_TOOLS
    run_all = RUN_ALL.replace("@EXTRA_TESTS@", "quest_xr_logic_tests" if args.openxr else "").replace(" ; do", "; do")

    jni = args.jni / "arm64-v8a"
    shutil.rmtree(args.jni, ignore_errors=True)
    jni.mkdir(parents=True)
    allowed = PLATFORM_LIBS | set(apk_libs) | {"libc++_shared.so"}
    sources = {name: args.build / rel for name, rel in apk_libs.items()}
    sources["libc++_shared.so"] = libcxx(args.ndk)
    for name, source in sources.items():
        if not source.is_file():
            raise FileNotFoundError(f"{name}: {source}")
        copy_stripped(args.ndk, source, jni / name)
        needed = check_elf(args.ndk, jni / name, allowed)
        print(f"apk  {name}: {(jni / name).stat().st_size} bytes; needs {needed}")
    if (jni / "libtriaevum_title_aot.so").exists():
        raise ValueError("A title module must never be packaged")

    shutil.rmtree(args.assets, ignore_errors=True)
    args.assets.mkdir(parents=True)
    shutil.copyfile(args.title_abi, args.assets / "title-abi.json")
    license_source = args.build / OPENXR_LICENSE
    if args.openxr:
        if not license_source.is_file():
            raise FileNotFoundError(f"OpenXR loader licence: {license_source}")
        (args.assets / "licenses").mkdir()
        shutil.copyfile(license_source, args.assets / "licenses" / OPENXR_LICENSE_NAME)
        print(f"apk  assets/licenses/{OPENXR_LICENSE_NAME}")

    tools_allowed = allowed | {"libtriaevum_title_whole_aot_support.so"}
    staging = args.tools_zip.parent / "quest-tools-arm64"
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    for rel in tools:
        source = args.build / rel
        if not source.is_file():
            raise FileNotFoundError(source)
        copy_stripped(args.ndk, source, staging / source.name)
        needed = check_elf(args.ndk, staging / source.name, tools_allowed)
        print(f"tool {source.name}: {(staging / source.name).stat().st_size} bytes; needs {needed}")
    synthetic = staging / "synthetic"
    copy_stripped(args.ndk, args.build / SYNTHETIC_TITLE, synthetic / "libtriaevum_title_aot.so")
    needed = check_elf(args.ndk, synthetic / "libtriaevum_title_aot.so", tools_allowed)
    print(f"tool synthetic/libtriaevum_title_aot.so: needs {needed}")
    shutil.copyfile(args.title_abi, synthetic / "title-abi.json")
    shutil.copyfile(libcxx(args.ndk), staging / "libc++_shared.so")
    if args.openxr:
        # Tests linking the runtime library NEED it (run_all.sh puts the directory on the path).
        shutil.copyfile(jni / "libopenxr_loader.so", staging / "libopenxr_loader.so")
        (staging / "LICENSES").mkdir()
        shutil.copyfile(license_source, staging / "LICENSES" / OPENXR_LICENSE_NAME)
    shutil.copyfile(args.title_abi, staging / "title-abi.json")
    (staging / "README.txt").write_text(TOOLS_README)
    (staging / "run_all.sh").write_text(run_all, newline="\n")
    args.tools_zip.unlink(missing_ok=True)
    with zipfile.ZipFile(args.tools_zip, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(p for p in staging.rglob("*") if p.is_file()):
            info = zipfile.ZipInfo(f"quest-tools-arm64/{path.relative_to(staging).as_posix()}")
            info.external_attr = (0o755 if path.suffix not in (".txt", ".json") else 0o644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, path.read_bytes())
    print(f"tools zip {args.tools_zip}: {args.tools_zip.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
