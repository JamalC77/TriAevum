#!/usr/bin/env python3
"""Generate a synthetic, redistributable translated-title source tree (one 6-instruction function).

It goes through the real whole-AOT C++ generator and the real Android title build
(ports/android/native, target triaevum_android_title), so CI can produce an ABI-v2 title module
without any ROM-derived input: the ABI probe and the direct-AOT loader test run against it, and it
can be side-loaded to check the APK's install and dlopen path. The program is the one
tools/triaevum_release/validate_whole_aot_toolchain.py uses (entry 0x1000).
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
A32_ROOT = ROOT / "tools/oot3d/native_a32_runtime"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    inputs = output / "inputs"
    sources = output / "translated"
    inputs.mkdir(parents=True, exist_ok=True)
    sources.mkdir(parents=True, exist_ok=True)
    # ldr r3,[r1,#4]; mov r0,#1; add r0,r0,#2; str r0,[r1]; ldr r2,[r1]; bx lr.
    code = b"".join(word.to_bytes(4, "little") for word in
                    (0xE5913004, 0xE3A00001, 0xE2800002, 0xE5810000, 0xE5912000, 0xE12FFF1E))
    code_path = inputs / "synthetic.bin"
    code_path.write_bytes(code)
    program = inputs / "program.json"
    program.write_text(json.dumps({
        "format": "oot3d_whole_aot_program_v1", "base": 0x1000,
        "code_sha256": sha256(code_path),
        "blocks": [{"id": 0, "pc": 0x1000, "end_pc": 0x1018,
                    "successors": [{"site": 0x1014, "kind": "return"}]}],
        "functions": [{"entry": 0x1000, "name": "SyntheticFunction", "blocks": [0],
                       "closed_static_cfg": True, "direct_calls": [], "tail_calls": [], "indirect_sites": []}],
    }))
    selection = inputs / "selection.json"
    selection.write_text(json.dumps({"format": "oot3d_whole_aot_function_selection_v1",
                                     "functions": [{"entry": 0x1000}]}))
    sys.path.insert(0, str(A32_ROOT))
    import whole_aot_cpp  # noqa: E402

    whole_aot_cpp.generate(program, selection, code_path, sources, shard_count=1, shard_strategy="affinity")
    files = {path.name: sha256(path) for path in sorted(sources.iterdir())
             if path.suffix in (".cpp", ".h") and path.is_file()}
    (sources / "TITLE_SOURCE_MANIFEST.json").write_text(json.dumps({
        "format": "triaevum_translated_title_source_v1",
        "synthetic": True,
        "files": files,
    }, indent=2) + "\n")
    print(f"synthetic title sources: {sources} ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
