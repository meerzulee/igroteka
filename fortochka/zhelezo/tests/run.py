#!/usr/bin/env python3
"""zhelezo test driver: assemble each tests/*.s with MinGW binutils to a flat
binary, run it under zhtest, and assert the `# EXPECT:` key=value pairs
against the machine-state dump. `exit=hlt` is the implicit default.

    python3 tests/run.py [--zhtest build/zhtest] [--keep] [name ...]
"""

import argparse
import pathlib
import re
import subprocess
import sys

AS = "i686-w64-mingw32-as"
OBJCOPY = "i686-w64-mingw32-objcopy"

HERE = pathlib.Path(__file__).parent
STR_KEYS = {"exit", "fault"}


def parse_kv(text: str) -> dict:
    out = {}
    for k, v in re.findall(r"(\w+)=(\S+)", text):
        if k in STR_KEYS:
            out[k] = v
        else:
            try:
                out[k] = int(v, 0)  # EXPECT side: 0x…, decimal
            except ValueError:
                out[k] = int(v, 16)  # zhtest side: bare %08x
    return out


def run_one(src: pathlib.Path, zhtest: str, workdir: pathlib.Path) -> list[str]:
    expects = {"exit": "hlt"}
    for line in src.read_text().splitlines():
        m = re.match(r"#\s*EXPECT:\s*(.*)", line)
        if m:
            expects.update(parse_kv(m.group(1)))

    obj = workdir / (src.stem + ".o")
    binf = workdir / (src.stem + ".bin")
    subprocess.run([AS, str(src), "-o", str(obj)], check=True)
    subprocess.run(
        [OBJCOPY, "-O", "binary", "-j", ".text", str(obj), str(binf)], check=True
    )

    proc = subprocess.run(
        [zhtest, str(binf)], capture_output=True, text=True, timeout=60
    )
    actual = parse_kv(proc.stdout)

    errors = []
    for key, want in expects.items():
        got = actual.get(key)
        if got != want:
            wtxt = want if isinstance(want, str) else f"{want:#x}"
            gtxt = got if isinstance(got, (str, type(None))) else f"{got:#x}"
            errors.append(f"{key}: want {wtxt}, got {gtxt}")
    if errors and proc.stdout:
        errors.append("state: " + " ".join(proc.stdout.split()))
    return errors


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--zhtest", default=str(HERE.parent / "build" / "zhtest"))
    ap.add_argument("--keep", action="store_true", help="keep .o/.bin artifacts")
    ap.add_argument("names", nargs="*", help="run only these tests")
    args = ap.parse_args()

    workdir = HERE.parent / "build" / "testbin"
    workdir.mkdir(parents=True, exist_ok=True)

    sources = sorted(HERE.glob("*.s"))
    if args.names:
        sources = [s for s in sources if s.stem in args.names]

    failed = 0
    for src in sources:
        try:
            errors = run_one(src, args.zhtest, workdir)
        except subprocess.CalledProcessError as e:
            errors = [f"toolchain failed: {e}"]
        if errors:
            failed += 1
            print(f"FAIL {src.stem}")
            for e in errors:
                print(f"     {e}")
        else:
            print(f"PASS {src.stem}")
    if not args.keep:
        for f in workdir.iterdir():
            f.unlink()

    print(f"\n{len(sources) - failed}/{len(sources)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
