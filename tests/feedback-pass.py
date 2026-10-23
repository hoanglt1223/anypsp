#!/usr/bin/env python3
"""Run the analysis -> translate -> run -> feedback loop on one homebrew module.

  python tests/feedback-pass.py <module.prx|.elf> [--frames 600] [--passes 2] [--base-first] [--tools out/msvc]

Pass N: anypsp-analyze (with the feedback of earlier passes), psp_recomp --auto
--seeds, a headless runner build (out/runners/<name>), then a run that appends
the code it reached without translation to out/feedback/<name>/feedback.txt.
Stops early once a run reports no new address. Prints a JSON summary; exit code
1 when the last run still reached untranslated code.
"""

import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CMAKE = shutil.which("cmake") or r"C:\Program Files\CMake\bin\cmake.exe"


def run(command, **kwargs):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        sys.stderr.write(result.stdout[-4000:] + result.stderr[-4000:])
        raise SystemExit(f"command failed ({result.returncode}): {' '.join(map(str, command))}")
    return result.stdout


def exe(directory, name):
    for candidate in (directory / "Release" / f"{name}.exe", directory / f"{name}.exe", directory / name):
        if candidate.exists():
            return candidate
    raise SystemExit(f"{name} not found under {directory}; build the project first")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("module", type=pathlib.Path)
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--passes", type=int, default=2)
    parser.add_argument("--opt", default="d", help="generated-code optimization: d, 1 or 2")
    parser.add_argument("--base-first", action="store_true",
                        help="translate pass 1 from the framework's own seeds only, to exercise the feedback loop")
    parser.add_argument("--tools", type=pathlib.Path, default=ROOT / "out" / "msvc")
    args = parser.parse_args()

    module = args.module.resolve()
    if not module.exists():
        raise SystemExit(f"{module} does not exist (run tests/fetch-corpus.py?)")
    name = re.sub(r"[^A-Za-z0-9_]", "_", module.name)
    analyze = exe(args.tools, "anypsp-analyze")
    recomp = exe(args.tools / "recomp", "psp_recomp")
    work = ROOT / "out" / "feedback" / name
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    feedback = work / "feedback.txt"
    feedback.touch()
    runner_build = ROOT / "out" / "runners" / name
    generated = ROOT / "out" / "gen" / name

    passes = []
    for number in range(1, args.passes + 1):
        report_dir = work / f"pass{number}"
        run([analyze, "--out", report_dir, "--feedback", feedback, module])
        report = json.loads((report_dir / "report.json").read_text(encoding="utf-8"))["modules"][0]
        seeds = report_dir / report["seeds_csv"]
        if generated.exists():
            shutil.rmtree(generated)
        base_only = args.base_first and number == 1
        run([recomp, module, "--auto", generated] + ([] if base_only else ["--seeds", seeds]))
        run([CMAKE, "-S", ROOT, "-B", runner_build, f"-DANYPSP_RUNNER_NAME={name}", f"-DANYPSP_RUNNER_GENERATED={generated}", f"-DANYPSP_RUNNER_OPT={args.opt}"])
        run([CMAKE, "--build", runner_build, "--config", "Release", "--target", f"anypsp-run-{name}"])
        runner = exe(runner_build, f"anypsp-run-{name}")
        before = len([line for line in feedback.read_text().splitlines() if line.strip()])
        result = subprocess.run([str(runner), str(module), "--frames", str(args.frames), "--feedback", str(feedback)],
                                capture_output=True, text=True, cwd=work)
        (work / f"pass{number}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        addresses = [line.split()[0] for line in feedback.read_text().splitlines() if line.strip()]
        new = len(addresses) - before
        stopped = re.findall(r"\[anypsp\] ran (\d+) frames; (.*)", result.stdout)
        passes.append({
            "pass": number,
            "functions": report["functions"]["count"],
            "translated_from": "framework seeds" if base_only else "anypsp-analyze seeds",
            "feedback_used": report["feedback"]["used"],
            "runner_exit": result.returncode,
            "frames": int(stopped[-1][0]) if stopped else None,
            "outcome": stopped[-1][1] if stopped else "runner did not finish",
            "new_missing_code": new,
        })
        if new == 0:
            break

    summary = {"module": str(module), "passes": passes, "missing_code_after_last_pass": passes[-1]["new_missing_code"]}
    print(json.dumps(summary, indent=2))
    return 0 if passes[-1]["new_missing_code"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
