#!/usr/bin/env python3
"""Integration test: run each corpus .exe through the Fortochka runtime and
assert its outcome. Complements zhelezo/tests/run.py (which unit-tests the CPU
on raw .s binaries); this exercises the full peload→zhelezo→HLE→reverse-thunk
pipeline on real PEs.

    python3 runtime/tests/run_corpus.py [--build build]
"""

import argparse
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parents[2]  # fortochka/


# (binary, runner, argv-prefix, expected exit code, stdout substring or None)
CASES = [
    ("hello.exe", "zhrun", [], 0, "hello from fortochka corpus"),
    ("window.exe", "window_test", [], 0, "reverse thunk round trip"),
    ("recurse.exe", "window_test", ["--recurse-guard"], 0, "depth guard tripped"),
    ("seh.exe", "zhrun", [], 42, None),          # fs:[0] handler saw 0x1234
    ("seh_chain.exe", "zhrun", [], 42, None),    # chain walk: inner+outer ran
    ("seh_noncont.exe", "zhrun", [], 1, "noncontinuable"),  # rejected, not resumed
    ("fpu.exe", "zhrun", [], 42, None),          # real x87 float math end-to-end
    ("paint.exe", "window_test", ["--paint"], 0, "guest GDI reached"),  # WM_PAINT→fb
    ("d3dclear.exe", "window_test", ["--d3d", "--corner", "336699"], 0, "reached the backbuffer"),
    ("tri.exe", "window_test", ["--d3d", "--tri"], 0, "reached the backbuffer"),  # rasterized triangle
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    args = ap.parse_args()
    build = HERE / args.build
    corpus = HERE / "corpus" / "bin"

    runners = {
        "zhrun": build / "runtime" / "zhrun",
        "window_test": build / "u32web" / "window_test",
    }

    failed = 0
    for binary, runner, prefix, want_code, want_out in CASES:
        exe = corpus / binary
        if not exe.exists():
            print(f"FAIL {binary}: not built (run `make -C corpus`)")
            failed += 1
            continue
        cmd = [str(runners[runner]), *prefix, str(exe)]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        combined = proc.stdout + proc.stderr
        errs = []
        if proc.returncode != want_code:
            errs.append(f"exit {proc.returncode}, want {want_code}")
        if want_out and want_out not in combined:
            errs.append(f"missing stdout {want_out!r}")
        if errs:
            failed += 1
            print(f"FAIL {binary}: {'; '.join(errs)}")
        else:
            print(f"PASS {binary}")

    print(f"\n{len(CASES) - failed}/{len(CASES)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
