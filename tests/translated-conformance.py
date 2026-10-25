#!/usr/bin/env python3
"""Translate pspautotests programs ahead of time and check their output against the .expected files.

  python tests/translated-conformance.py [filter ...] [--tools out/msvc] [--json out.json]

Every selected test goes through anypsp-analyze and psp_recomp --auto; all of them are built
as runners in one build tree (out/tconf/runners, sharing one build of the host), then run like
tests/conformance.py does. Code the analysis missed still runs, in the reference interpreter;
the "untranslated" column counts the addresses where that happened.
"""

import argparse
import concurrent.futures
import json
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
import conformance  # noqa: E402

CMAKE = shutil.which("cmake") or r"C:\Program Files\CMake\bin\cmake.exe"


def exe(directory, name):
    for candidate in (directory / "Release" / f"{name}.exe", directory / f"{name}.exe", directory / name):
        if candidate.exists():
            return candidate
    raise SystemExit(f"{name} not found under {directory}; build the project first")


def run(command):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"{' '.join(map(str, command))} failed:\n{result.stdout[-3000:]}{result.stderr[-3000:]}")
    return result.stdout


def translate(program, work, analyze, recomp):
    name = "t_" + re.sub(r"[^A-Za-z0-9_]", "_", program.relative_to(conformance.TESTS).with_suffix("").as_posix())
    report_dir = work / "analysis" / name
    generated = work / "gen" / name
    for directory in (report_dir, generated):
        if directory.exists():
            shutil.rmtree(directory)
    run([analyze, "--out", report_dir, program])
    report = json.loads((report_dir / "report.json").read_text(encoding="utf-8"))["modules"][0]
    run([recomp, program, "--auto", generated, "--seeds", report_dir / report["seeds_csv"]])
    return name, generated


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("filters", nargs="*")
    parser.add_argument("--tools", type=pathlib.Path, default=ROOT / "out" / "msvc")
    parser.add_argument("--work", type=pathlib.Path, default=ROOT / "out" / "tconf")
    parser.add_argument("--opt", default="d")
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--build-jobs", type=int, default=6, help="runners built at once, or build jobs with a single-config generator (each needs ~2.5 GB)")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    if not conformance.TESTS.exists():
        print("corpus not fetched; run python tests/fetch-corpus.py")
        return 77
    analyze, recomp = exe(args.tools, "anypsp-analyze"), exe(args.tools / "recomp", "psp_recomp")
    programs = sorted(p for p in conformance.TESTS.rglob("*.prx") if p.with_suffix(".expected").exists())
    if args.filters:
        programs = [p for p in programs if any(f in p.relative_to(conformance.TESTS).as_posix() for f in args.filters)]
    work = args.work.resolve()
    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        translated = list(pool.map(lambda p: translate(p, work, analyze, recomp), programs))

    build = work / "runners"
    runners = ";".join(f"{name}={generated.as_posix()}" for name, generated in translated)
    run([CMAKE, "-S", ROOT, "-B", build, f"-DANYPSP_RUNNERS={runners}", f"-DANYPSP_RUNNER_OPT={args.opt}", "-DANYPSP_RUNNER_JOBS=2",
         # Single-config generators (Ninja, Makefiles; CMAKE_GENERATOR, CC and CXX pick them) need these.
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF"])
    targets = [f"anypsp-run-{name}" for name, _ in translated]
    # The Visual Studio generator builds a --target list one project at a time, and each test's generated
    # code is one huge function that takes minutes to compile. Build the shared host with the first runner,
    # then the rest side by side without re-entering their (already built, shared) project references.
    run([CMAKE, "--build", build, "--config", "Release", "--parallel", str(args.build_jobs), "--target", targets[0]])
    if (build / "AnyPSP.sln").exists():
        with concurrent.futures.ThreadPoolExecutor(args.build_jobs) as pool:
            list(pool.map(lambda t: run([CMAKE, "--build", build, "--config", "Release", "--target",
                                         t.replace("anypsp-run-", "anypsp_generated_"), t,
                                         "--", "/p:BuildProjectReferences=false"]), targets[1:]))
    elif targets[1:]:
        run([CMAKE, "--build", build, "--config", "Release", "--parallel", str(args.build_jobs), "--target"] + targets[1:])

    def check(item):
        program, (name, _) = item
        feedback = work / "feedback" / f"{name}.txt"
        feedback.parent.mkdir(parents=True, exist_ok=True)
        feedback.unlink(missing_ok=True)
        result = conformance.run_one(exe(build, f"anypsp-run-{name}"), program, args.frames, args.timeout,
                                     ["--feedback", str(feedback)])
        result["untranslated"] = len(feedback.read_text().splitlines()) if feedback.exists() else 0
        return result

    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        results = list(pool.map(check, zip(programs, translated)))
    passed = sum(r["passed"] for r in results)
    for r in results:
        print(f"{'PASS' if r['passed'] else 'FAIL'} {r['test']}  ({r['lines_matching']}/{r['lines_expected']} lines, "
              f"{r['untranslated']} untranslated)")
        if not r["passed"] and args.verbose:
            d = r["first_difference"]
            print(f"     line {d['line']}: expected {d['expected']!r}\n                 actual   {d['actual']!r}\n     {r['outcome']}")
    print(f"{passed}/{len(results)} passed")
    if args.json:
        args.json.write_bytes(json.dumps({"passed": passed, "total": len(results), "results": results}, indent=2).encode())
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
