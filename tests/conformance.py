#!/usr/bin/env python3
"""Run pspautotests programs headless and compare their output with the .expected files.

  python tests/conformance.py [--runner exe] [--frames n] [--json out.json] [filter ...]

The default runner is the interpreter-only anypsp-interp; pass a per-game runner
(anypsp-run-<name>) to check translated code. Filters are substrings of the test path
under tests/corpus/pspautotests/tests (e.g. "cpu/" or "cpu/vfpu/prefixes"). Lines are
compared the way PPSSPP's headless --compare does: line endings and trailing blank lines
are ignored. Exit code 1 when any selected test fails, 77 when the corpus is missing.
"""

import argparse
import concurrent.futures
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests" / "corpus" / "pspautotests" / "tests"


def lines(text):
    result = [line.rstrip("\r") for line in text.split("\n")]
    while result and result[-1] == "":
        result.pop()
    return result


def first_difference(expected, actual):
    for index in range(max(len(expected), len(actual))):
        want = expected[index] if index < len(expected) else "<end>"
        got = actual[index] if index < len(actual) else "<end>"
        if want != got:
            return index + 1, want, got
    return None


def run_one(runner, program, frames, timeout, extra=()):
    expected_path = program.with_suffix(".expected")
    with tempfile.TemporaryDirectory() as work:
        output = pathlib.Path(work) / "out.txt"
        try:
            result = subprocess.run([str(runner), str(program), "--frames", str(frames), "--output", str(output)] + list(extra),
                                    capture_output=True, text=True, cwd=work, timeout=timeout)
            log = result.stdout + result.stderr
        except subprocess.TimeoutExpired:
            log = f"timed out after {timeout}s"
        actual = output.read_text(encoding="utf-8", errors="replace") if output.exists() else ""
    expected = expected_path.read_text(encoding="utf-8", errors="replace")
    want, got = lines(expected), lines(actual)
    difference = first_difference(want, got)
    matching = sum(1 for a, b in zip(want, got) if a == b)
    outcome = [line for line in log.splitlines() if line.startswith("[anypsp] ran") or "fatal" in line]
    return {
        "test": program.relative_to(TESTS).with_suffix("").as_posix(),
        "passed": difference is None,
        "lines_matching": matching,
        "lines_expected": len(want),
        "first_difference": None if difference is None else {"line": difference[0], "expected": difference[1], "actual": difference[2]},
        "outcome": outcome[-1] if outcome else log.strip().splitlines()[-1:] or "",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("filters", nargs="*")
    parser.add_argument("--runner", type=pathlib.Path, default=ROOT / "out" / "msvc" / "Release" / "anypsp-interp.exe")
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    if not TESTS.exists():
        print("corpus not fetched; run python tests/fetch-corpus.py")
        return 77
    if not args.runner.exists():
        sys.exit(f"{args.runner} not found; build the project first")
    programs = sorted(p for p in TESTS.rglob("*.prx") if p.with_suffix(".expected").exists())
    if args.filters:
        programs = [p for p in programs if any(f in p.relative_to(TESTS).as_posix() for f in args.filters)]
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(lambda p: run_one(args.runner, p, args.frames, args.timeout), programs))
    passed = sum(r["passed"] for r in results)
    for r in results:
        status = "PASS" if r["passed"] else "FAIL"
        print(f"{status} {r['test']}  ({r['lines_matching']}/{r['lines_expected']} lines)")
        if not r["passed"] and args.verbose:
            d = r["first_difference"]
            print(f"     line {d['line']}: expected {d['expected']!r}\n                 actual   {d['actual']!r}\n     {r['outcome']}")
    print(f"{passed}/{len(results)} passed")
    if args.json:
        args.json.write_bytes(json.dumps({"runner": str(args.runner), "passed": passed, "total": len(results), "results": results}, indent=2).encode())
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
