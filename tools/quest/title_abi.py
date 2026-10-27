#!/usr/bin/env python3
"""Host/title ABI pairing manifest for the Quest port.

The side-loaded title library (built privately on the PC) runs its own inline copy of
NativeA32Memory and the VFP support code against the host's objects, so host and title
must be compiled from byte-identical title-side sources with the same NDK. This tool
computes that closure (seed files plus their transitive repository includes), writes
title-abi.json, and checks the closure against the pinned v0.8.0-beta.1.2 hashes.

  title_abi.py write --root <repo> --ndk <ndk dir> --output title-abi.json
  title_abi.py check --root <repo>          # fails if the closure drifted from the pin
  title_abi.py pin --root <v0.8.0 checkout> --output tools/quest/title-abi-closure.json
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

NGR = "tools/oot3d/native_game_runtime"
A32 = "tools/oot3d/native_a32_runtime"
PINNED = f"{A32}/upstream"

SEEDS = [
    f"{NGR}/oot3d_native_a32_memory.h",
    f"{NGR}/oot3d_native_a32_memory.cpp",
    f"{NGR}/oot3d_native_a32_memory_bus_defaults.cpp",
    f"{NGR}/oot3d_native_whole_aot_runtime.h",
    f"{NGR}/triaevum_title_whole_aot_abi.h",
    f"{NGR}/triaevum_title_whole_aot_plugin.cpp",
    f"{PINNED}/recomp/a32_vfp_binary64.cpp",
    f"{A32}/oot3d_native_a32_vfp_ops.cpp",
    f"{PINNED}/recomp/a32_vfp_transport.cpp",
    "tools/triaevum_release/cmake/TitleSupport.cmake",
    "ports/android/native/CMakeLists.txt",
]
INCLUDE_DIRS = [NGR, PINNED, f"{PINNED}/recomp", A32, f"{A32}/mass_cpp"]
PIN_FILE = "tools/quest/title-abi-closure.json"
INCLUDE = re.compile(rb'^\s*#\s*include\s*"([^"]+)"', re.M)


def closure(root: Path) -> dict:
    found = {}
    pending = list(SEEDS)
    while pending:
        rel = pending.pop()
        if rel in found:
            continue
        path = root / rel
        if not path.is_file():
            raise FileNotFoundError(rel)
        data = path.read_bytes()
        # Line endings never change the compiled code; hash LF text so a CRLF checkout pairs.
        found[rel] = hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()
        if not rel.endswith((".h", ".hpp", ".cpp", ".inc")):
            continue
        for name in INCLUDE.findall(data):
            name = name.decode()
            for base in [str(Path(rel).parent).replace("\\", "/")] + INCLUDE_DIRS:
                candidate = (root / base / name).resolve()
                if candidate.is_file():
                    pending.append(candidate.relative_to(root.resolve()).as_posix())
                    break
            # Unresolved quoted includes are generated title headers (supplied by the
            # private title tree) or external packages; they are not host sources.
    return dict(sorted(found.items()))


def ndk_revision(ndk: Path) -> str:
    props = (ndk / "source.properties").read_text()
    match = re.search(r"Pkg\.Revision\s*=\s*(\S+)", props)
    if not match:
        raise ValueError(f"No Pkg.Revision in {ndk}/source.properties")
    return match.group(1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=["write", "check", "pin"])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--ndk", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    files = closure(args.root)
    pin_path = args.root / PIN_FILE
    if args.mode == "pin":
        (args.output or pin_path).write_text(json.dumps({"base": "v0.8.0-beta.1.2", "files": files}, indent=2) + "\n")
        print(f"pinned {len(files)} files")
        return 0
    pinned = json.loads(pin_path.read_text())["files"]
    drift = sorted(set(files) ^ set(pinned)) + sorted(k for k in files if k in pinned and files[k] != pinned[k])
    if drift:
        print("title ABI closure drifted from v0.8.0-beta.1.2:", *drift, sep="\n  ", file=sys.stderr)
        return 1
    print(f"title ABI closure matches the pin ({len(files)} files)")
    if args.mode == "write":
        if not args.ndk or not args.output:
            parser.error("write needs --ndk and --output")
        manifest = {
            "schema": 1,
            "ndk_revision": ndk_revision(args.ndk),
            "abi": "arm64-v8a",
            "stl": "c++_shared",
            "closure_sha256": hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest(),
            "files": files,
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        print(f"wrote {args.output} (ndk {manifest['ndk_revision']}, closure {manifest['closure_sha256'][:12]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
