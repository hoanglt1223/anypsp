#!/usr/bin/env python3
"""Translate pspautotests demos and check their frames against the interpreter's.

  python tests/translated-demos.py [filter ...] [--tools out/linux] [--frames 60] [--build-jobs 4]

Each selected demo (default: cubevfpu.prx) goes through anypsp-analyze and psp_recomp --auto and is
built as anypsp-run-<name>-gpu (one build tree per demo under out/tdemos). Both that runner and
anypsp-interp-gpu run headless with the software renderer and --deterministic, and the per-frame
hashes must be identical. Exit code 1 on any difference, 77 when the corpus is missing.
On Linux set CMAKE_GENERATOR=Ninja, CC=clang and CXX=clang++.
"""

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEMOS = ROOT / "tests" / "corpus" / "pspautotests" / "demos"
CMAKE = shutil.which("cmake") or r"C:\Program Files\CMake\bin\cmake.exe"


def exe(directory, name):
    for candidate in (directory / "Release" / f"{name}.exe", directory / f"{name}.exe", directory / name):
        if candidate.exists():
            return candidate
    raise SystemExit(f"{name} not found under {directory}; build the project first")


def run(command, **kwargs):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        raise RuntimeError(f"{' '.join(map(str, command))} failed:\n{result.stdout[-3000:]}{result.stderr[-3000:]}")
    return result.stdout


def hashes(runner, program, frames):
    env = dict(os.environ, SDL_VIDEO_DRIVER="offscreen")
    with tempfile.TemporaryDirectory() as work:
        path = pathlib.Path(work) / "hashes.txt"
        subprocess.run([str(runner), str(program), "--headless", "--deterministic", "--renderer", "soft",
                        "--frames", str(frames), "--hashes", str(path)],
                       capture_output=True, text=True, cwd=work, env=env, timeout=300)
        return path.read_text().splitlines() if path.exists() else []


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("filters", nargs="*", default=["cubevfpu.prx"])
    parser.add_argument("--tools", type=pathlib.Path, default=ROOT / "out" / "msvc")
    parser.add_argument("--work", type=pathlib.Path, default=ROOT / "out" / "tdemos")
    parser.add_argument("--opt", default="1")
    parser.add_argument("--frames", type=int, default=60)
    parser.add_argument("--build-jobs", type=int, default=4)
    args = parser.parse_args()
    if not DEMOS.exists():
        print("corpus not fetched; run python tests/fetch-corpus.py")
        return 77
    tools = args.tools.resolve()
    analyze, recomp = exe(tools, "anypsp-analyze"), exe(tools / "recomp", "psp_recomp")
    interp = exe(tools, "anypsp-interp-gpu")
    programs = sorted(p for p in DEMOS.iterdir() if any(f in p.name for f in args.filters))
    failures = 0
    for program in programs:
        name = "d_" + re.sub(r"[^A-Za-z0-9_]", "_", program.stem)
        work = args.work.resolve() / name
        report_dir, generated, tree = work / "analysis", work / "gen", work / "runners"
        for directory in (report_dir, generated):
            if directory.exists():
                shutil.rmtree(directory)
        run([analyze, "--out", report_dir, program])
        report = json.loads((report_dir / "report.json").read_text(encoding="utf-8"))["modules"][0]
        run([recomp, program, "--auto", generated, "--seeds", report_dir / report["seeds_csv"]])
        run([CMAKE, "-S", ROOT, "-B", tree, f"-DANYPSP_RUNNER_NAME={name}", f"-DANYPSP_RUNNER_GENERATED={generated}",
             f"-DANYPSP_RUNNER_OPT={args.opt}", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF"])
        run([CMAKE, "--build", tree, "--config", "Release", "--parallel", str(args.build_jobs),
             "--target", f"anypsp-run-{name}-gpu"])
        translated = hashes(exe(tree, f"anypsp-run-{name}-gpu"), program, args.frames)
        reference = hashes(interp, program, args.frames)
        same = translated == reference and len(reference) == args.frames
        failures += not same
        detail = ""
        if not same:
            first = next((i for i, (a, b) in enumerate(zip(translated, reference)) if a != b), min(len(translated), len(reference)))
            detail = f" (first difference at frame {first + 1}; {len(translated)} vs {len(reference)} frames)"
        print(f"{'PASS' if same else 'FAIL'} {program.name}{detail}")
    print(f"{len(programs) - failures}/{len(programs)} demos match the interpreter")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
