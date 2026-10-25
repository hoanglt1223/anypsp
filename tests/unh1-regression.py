#!/usr/bin/env python3
"""Long scripted run of Naruto: Ultimate Ninja Heroes (ULUS-10299), the user's own disc.

  python tests/unh1-regression.py --play <anypsp-play> [--program <program from anypsp build>]
                                  [--iso unh1.iso] [--work dir] [--baseline-dir dir] [--frames N]

The input script goes from boot through the title and the menus to team select, battle order and a
battle with a few jutsu inputs, then pauses it, returns to mode select (Game Over, saving to the run's
own memory stick) and ends there. Under --deterministic the frames repeat exactly, so:

- the reference interpreter (anypsp-play) must reach the frame limit with no unimplemented HLE call;
- a translated program built by `anypsp build` (--program, or ANYPSP_UNH1_PROGRAM) must draw the same
  frames as the interpreter, frame for frame;
- both are compared with a baseline kept outside the repository (no game-derived data is committed):
  --baseline-dir, ANYPSP_REGRESSION_DIR, else the per-user AnyPSP data folder. The first run records it
  (look at the checkpoint PNGs it writes before trusting it); later runs must match it.

Exit codes: 0 pass, 1 fail, 77 skipped (no disc: --iso, ANYPSP_UNH1_ISO, ~/games/psp/unh1.iso).
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys

SKIP = 77
DISC_ID = "ULUS-10299"

# sceCtrl masks: Select 0x1, Start 0x8, Up 0x10, Right 0x20, Down 0x40, Left 0x80, Triangle 0x1000,
# Circle 0x2000, Cross 0x4000, Square 0x8000. (frame, buttons, vblanks held)
STEPS = [
    (1500, 0x10, 6),                                     # title: Up to New Game
    *[(f, 0x4000, 6) for f in range(1620, 2950, 120) if f != 2220],  # New Game, VS CPU, the menus, team select
    (2220, 0x40, 6),                                     #   (one Down on the way)
    *[(f, 0x8, 6) for f in range(3300, 3900, 100)],      # Start: skip the stage intro dialogue
    *[(f, 0x4000, 6) for f in range(4000, 4700, 60)],    # battle order, confirm, begin
    (4900, 0x10, 4), (4908, 0x2010, 4),                  # in battle: jutsu inputs and attacks
    (5050, 0x40, 4), (5058, 0x2040, 4),
    (5200, 0x1000, 4), (5208, 0x2000, 4),
    (5350, 0x2000, 4),
    (6000, 0x8, 6),                                      # pause
    *[(f, 0x40, 6) for f in range(6100, 6350, 50)],      # down to Return to Mode Select
    (6400, 0x4000, 6), (6500, 0x4000, 6),                # choose it, Yes
    (6800, 0x4000, 6),                                   # Game Over: Next
    (7100, 0x4000, 6),                                   # Save data? Yes (into the run's own memory stick)
]
# Title, team select, battle order, battle, pause menu, game over, save prompt, mode select.
CHECKPOINTS = [1400, 2500, 4300, 5500, 6100, 6700, 7000, 7400]


def input_script(steps: list[tuple[int, int, int]]) -> str:
    items = []
    for frame, mask, hold in sorted(steps):
        items += [f"{frame}:{mask:x}", f"{frame + hold}:0"]
    return ",".join(items)


def default_baseline_dir() -> pathlib.Path:
    if os.name == "nt":
        return pathlib.Path(os.environ.get("APPDATA", pathlib.Path.home())) / "anyport" / "anypsp" / "regression"
    return pathlib.Path(os.environ.get("XDG_DATA_HOME", pathlib.Path.home() / ".local" / "share")) / "anyport" / "anypsp" / "regression"


def run(name: str, command: list[str], work: pathlib.Path, frames: int, script: str) -> dict[int, str]:
    out = work / name
    shutil.rmtree(out, ignore_errors=True)
    (out / "ms").mkdir(parents=True)
    hashes = out / "hashes.txt"
    full = command + ["--headless", "--deterministic", "--mute", "--frames", str(frames), "--input", script,
                      "--ms", str(out / "ms"), "--cache", str(work / "cache"), "--hashes", str(hashes),
                      "--frames-dir", str(out / "frames"), "--png-every", "100"]
    env = dict(os.environ, SDL_VIDEO_DRIVER=os.environ.get("SDL_VIDEO_DRIVER", "offscreen"))
    result = subprocess.run(full, capture_output=True, text=True, env=env)
    log = result.stdout + result.stderr
    (out / "run.log").write_text(log, encoding="utf-8")
    summary = [line for line in log.splitlines() if "] ran " in line]
    print(f"{name}: {summary[-1] if summary else 'no summary line'}")
    if result.returncode != 0 or not summary or "frame limit reached" not in summary[-1]:
        raise SystemExit(f"FAIL: {name} did not reach frame {frames} (exit {result.returncode}); see {out / 'run.log'}")
    if "unimplemented HLE function" in log:
        raise SystemExit(f"FAIL: {name} called an unimplemented HLE function; see {out / 'run.log'}")
    table = {}
    for line in hashes.read_text().splitlines():
        frame, value = line.split()
        table[int(frame)] = value
    # Keep only the checkpoint pictures for review.
    for png in (out / "frames").glob("frame_*.png"):
        if int(png.stem.split("_")[1]) not in CHECKPOINTS:
            png.unlink()
    return table


def first_difference(a: dict[int, str], b: dict[int, str]) -> int | None:
    for frame in sorted(set(a) | set(b)):
        if a.get(frame) != b.get(frame):
            return frame
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--play", required=True, help="anypsp-play (the reference interpreter)")
    parser.add_argument("--program", default=os.environ.get("ANYPSP_UNH1_PROGRAM"), help="translated program from anypsp build")
    parser.add_argument("--iso", default=os.environ.get("ANYPSP_UNH1_ISO", str(pathlib.Path.home() / "games" / "psp" / "unh1.iso")))
    parser.add_argument("--work", type=pathlib.Path, default=pathlib.Path("unh1-regression"))
    parser.add_argument("--baseline-dir", type=pathlib.Path,
                        default=pathlib.Path(os.environ["ANYPSP_REGRESSION_DIR"]) if os.environ.get("ANYPSP_REGRESSION_DIR") else default_baseline_dir())
    parser.add_argument("--frames", type=int, default=CHECKPOINTS[-1])
    args = parser.parse_args()

    iso = pathlib.Path(args.iso)
    if not iso.is_file():
        print(f"skipped: no {DISC_ID} disc at {iso} (set ANYPSP_UNH1_ISO)")
        return SKIP
    args.work.mkdir(parents=True, exist_ok=True)
    script = input_script(STEPS)
    reference = run("interpreter", [args.play, str(iso)], args.work, args.frames, script)
    if args.program:
        translated = run("translated", [args.program, str(iso)], args.work, args.frames, script)
        diff = first_difference(reference, translated)
        if diff is not None:
            print(f"FAIL: the translated program draws frame {diff} differently from the interpreter")
            return 1
        print(f"translated: all {len(translated)} frames match the interpreter")

    baseline = args.baseline_dir / f"{DISC_ID}-{args.frames}.txt"
    current = "\n".join(f"{f} {reference[f]}" for f in sorted(reference)) + "\n"
    if baseline.is_file():
        recorded = {int(f): h for f, h in (line.split() for line in baseline.read_text().splitlines())}
        diff = first_difference(recorded, reference)
        if diff is not None:
            print(f"FAIL: frame {diff} differs from the baseline {baseline}; if the change is intended, delete it and rerun")
            return 1
        print(f"baseline: all {len(recorded)} frames match {baseline}")
    else:
        baseline.parent.mkdir(parents=True, exist_ok=True)
        baseline.write_text(current)
        print(f"baseline: recorded {baseline}; check the checkpoint frames in {args.work / 'interpreter' / 'frames'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
