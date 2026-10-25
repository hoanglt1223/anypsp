# Compatibility data

One TOML file per title in `compat/`. `tools/compat-site/generate.py` validates them and renders them,
with the coverage numbers, into `progress.html`, `compat.json` and SVG badges; the `pages` workflow
publishes the result. The format is AnyGBA's (`repos/anygba/compat/schema.md`); only the
platform fields (`disc_id`, `module`, `runner`, `hosts`) differ.

```toml
title = "Naruto: Ultimate Ninja Heroes"
kind = "commercial"                  # commercial | homebrew | test-suite
source = "the user's own UMD dump"   # a URL for homebrew and test suites
disc_id = "ULUS-10299"               # commercial titles: the disc ID, never a file name or hash
module = "pspautotests/demos/cubevfpu.prx"   # homebrew and test suites: path in tests/corpus
genre = "fighting"                   # optional
stage = "in-game"                    # see below
runner = "translated"                # translated (psp_recomp code, `anypsp build`) | interpreter
hosts = ["linux-x64"]                # where the evidence was taken: windows-x64, linux-x64, linux-arm64,
                                     # macos-arm64, android-arm64, android-x86_64
evidence = "what was run, how long, what was checked"
blockers = []                        # what keeps it from the next stage, one line each
regression = "ctest -L unh1"         # optional: the test that guards the evidence
updated = 2026-10-25
```

Commercial titles never name a file path or carry a hash of game data: only the disc ID (`ULUS-10299`,
with the dash) identifies them, and whoever owns the game checks them. Their regression tests run
only where a local copy of the game exists and skip elsewhere.

## Stages

| Stage | Meaning |
|---|---|
| `fails` | Stops with an error before the first frame, or never shows a picture |
| `boots` | Shows pictures, but stops with an error or shows wrong output before its main content |
| `menus` | Title screen and menus work; the main content does not |
| `in-game` | Main content runs (for a game: a level, battle or match) without errors; output not checked in full |
| `playable` | Can be played start to finish on the recorded hosts without known blocking bugs |
| `perfect` | No known difference from hardware: all of the title's own checks pass (test suites), or pictures and sound match the reference throughout |

A title only moves up a stage with the evidence recorded in `evidence`. Stages `in-game` and above must
have evidence.

## Coverage

The badges come from three measurements, rendered by `generate.py --hle-coverage <build>/hle-coverage.json
--coverage <(anypsp-coverage)`:

- **HLE functions**: `anypsp-hle-coverage` runs after every host build and writes `hle-coverage.json`: the
  functions of `data/nids.toml` the kernel implements, in total and per library (the page lists the libraries
  with at least one). The total includes kernel-mode `*ForKernel` libraries, which games never import.
- **VFPU instructions**: every VFPU instruction of the reference interpreter, by mnemonic, sampled over its
  encodings; native when the code generator translates all of them itself instead of calling the
  interpreter from generated code (`anypsp::codegen::translated_inline`).
- **GE commands**: an approximate static scan of `runtime/host/ge.cpp` against the PSP's GE command list:
  a command counts when the GE front end reads or dispatches it.
