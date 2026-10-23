#!/usr/bin/env python3
"""Run the pspautotests GE tests that ship a reference screenshot and compare.

  python tests/ge-screenshots.py [--runner exe] [--renderer gl|soft] [--max-mse x] [--out dir]
                                 [--json file] [filter ...]

Each program runs headless in anypsp-interp-gpu (hidden GL context, no audio); the
picture at its emulator: screenshot request is compared with <test>.expected.bmp the
way PPSSPP's headless does (mean squared error over RGB of the 480x272 display, alpha
ignored). A test passes when the MSE is at most --max-mse (default 0, PPSSPP's default).
Text output is compared too, like tests/conformance.py. Exit code 1 when any selected
test fails, 77 when the corpus is missing.
"""

import argparse
import concurrent.futures
import json
import pathlib
import struct
import subprocess
import sys
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests" / "corpus" / "pspautotests" / "tests"
WIDTH, HEIGHT = 480, 272


def read_png(path):
    """Decodes an 8-bit RGB or RGBA, non-interlaced PNG into rows of RGB bytes."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, header = 8, bytearray(), None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"IDAT":
            idat += chunk
        elif kind == b"IEND":
            break
    width, height, depth, color, _, _, interlace = header
    if depth != 8 or color not in (2, 6) or interlace != 0:
        raise ValueError(f"{path}: unsupported PNG layout")
    bpp = 3 if color == 2 else 4
    raw = zlib.decompress(bytes(idat))
    stride = width * bpp
    rows, previous = [], bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = previous[x]
            c = previous[x - bpp] if x >= bpp else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            elif kind != 0:
                raise ValueError(f"{path}: bad filter {kind}")
        rows.append(bytes(line) if bpp == 3 else bytes(v for i, v in enumerate(line) if i % 4 != 3))
        previous = line
    return width, height, rows


def read_bmp(path):
    """Reads PPSSPP's 512x272 32-bit BGRA reference bitmaps into top-down RGB rows."""
    data = path.read_bytes()
    (offset,) = struct.unpack("<I", data[10:14])
    width, height = struct.unpack("<ii", data[18:26])
    (bits,) = struct.unpack("<H", data[28:30])
    if bits != 32:
        raise ValueError(f"{path}: expected a 32-bit bitmap")
    rows = []
    for y in range(abs(height)):
        source = y if height < 0 else abs(height) - 1 - y
        start = offset + source * width * 4
        line = data[start:start + width * 4]
        rows.append(bytes(line[i + c] for i in range(0, len(line), 4) for c in (2, 1, 0)))
    return width, abs(height), rows


def mse(actual_rows, expected_rows):
    total = 0
    for y in range(HEIGHT):
        a = actual_rows[y][:WIDTH * 3] if y < len(actual_rows) else bytes(WIDTH * 3)
        e = expected_rows[y][:WIDTH * 3]
        total += sum((p - q) * (p - q) for p, q in zip(a, e))
    return total / (WIDTH * HEIGHT * 3)


def lines(text):
    result = [line.rstrip("\r") for line in text.split("\n")]
    while result and result[-1] == "":
        result.pop()
    return result


def run_one(runner, renderer, program, frames, timeout, out_dir, max_mse):
    name = program.relative_to(TESTS).with_suffix("").as_posix()
    work = out_dir / name.replace("/", "_")
    work.mkdir(parents=True, exist_ok=True)
    shot, text = work / f"actual-{renderer}.png", work / f"output-{renderer}.txt"
    for stale in (shot, text):
        stale.unlink(missing_ok=True)
    try:
        result = subprocess.run([str(runner), str(program), "--renderer", renderer, "--frames", str(frames),
                                 "--output", str(text), "--screenshot", str(shot)],
                                capture_output=True, text=True, cwd=work, timeout=timeout)
        log = (result.stdout + result.stderr).strip().splitlines()
    except subprocess.TimeoutExpired:
        log = [f"timed out after {timeout}s"]
    outcome = next((l for l in reversed(log) if l.startswith("[anypsp] ran") or "fatal" in l), log[-1] if log else "")
    record = {"test": name, "outcome": outcome, "mse": None, "screenshot": shot.exists()}
    if shot.exists():
        _, _, expected = read_bmp(program.with_suffix(".expected.bmp"))
        width, height, actual = read_png(shot)
        if (width, height) != (WIDTH, HEIGHT):
            raise ValueError(f"{shot}: {width}x{height}, compare needs 480x272 (scale 1)")
        record["mse"] = round(mse(actual, expected), 4)
    expected_text = program.with_suffix(".expected")
    want = lines(expected_text.read_text(encoding="utf-8", errors="replace")) if expected_text.exists() else []
    got = lines(text.read_text(encoding="utf-8", errors="replace")) if text.exists() else []
    record["text_matches"] = want == got
    record["passed"] = record["mse"] is not None and record["mse"] <= max_mse and record["text_matches"]
    return record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("filters", nargs="*")
    parser.add_argument("--runner", type=pathlib.Path, default=ROOT / "out" / "msvc" / "Release" / "anypsp-interp-gpu.exe")
    parser.add_argument("--renderer", choices=("gl", "soft"), default="gl")
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--max-mse", type=float, default=0.0)
    parser.add_argument("--out", type=pathlib.Path, default=ROOT / "out" / "ge-screenshots")
    parser.add_argument("--json", type=pathlib.Path)
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.runner = args.runner.resolve()
    if not TESTS.exists():
        print("corpus not fetched; run python tests/fetch-corpus.py")
        return 77
    if not args.runner.exists():
        print(f"runner not built: {args.runner}")
        return 2
    programs = sorted(p for p in (TESTS / "gpu").rglob("*.prx") if p.with_suffix(".expected.bmp").exists())
    programs = [p for p in programs if not args.filters or any(f in p.as_posix() for f in args.filters)]
    if not programs:
        print("no GE screenshot tests selected")
        return 77
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(lambda p: run_one(args.runner, args.renderer, p, args.frames, args.timeout, args.out,
                                                  args.max_mse), programs))
    for r in results:
        mse_text = "no screenshot" if r["mse"] is None else f"mse {r['mse']:.4f}"
        print(f"{'PASS' if r['passed'] else 'FAIL'} {r['test']}: {mse_text}, text {'ok' if r['text_matches'] else 'differs'}"
              f" ({r['outcome']})")
    passed = sum(r["passed"] for r in results)
    print(f"{passed}/{len(results)} passed ({args.renderer}, max mse {args.max_mse}); pictures in {args.out}")
    if args.json:
        args.json.write_bytes((json.dumps(results, indent=1) + "\n").encode())
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
