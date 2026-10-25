# Contributing to AnyPSP

## Legal policy

AnyPSP is a tool. It ships no firmware, keys or game data, and contributions must keep it that way:

- **No game data in the repository, issues, discussions or pull requests**: no ISO images, executables
  (encrypted or decrypted), PRX modules, extracted assets, save files, or hashes and dumps derived from
  them. Commercial games are named by title and disc ID only (`compat/schema.md`).
- **No firmware and no keys**: nothing from `flash0:` (fonts included), no KIRK/PGD keys, no decryption
  code. Users bring decrypted dumps of games they own; AnyPSP refuses encrypted executables.
- **Test content is open or self-made**: the corpus is pspautotests, fetched by `tests/fetch-corpus.py`
  (pinned by commit and SHA-256, never committed). Tests that need a commercial game run only when a
  local copy is present and skip otherwise.
- **Licences**: AnyPSP is GPL-2.0-or-later. Code adapted from elsewhere must have a compatible licence,
  keep its notice, and be listed in `THIRD_PARTY.md`. PPSSPP is the behaviour reference; say in the file
  header when code follows it.

## Building and testing

Commands per host are in `README.md`; the Linux and Windows CI jobs are in `.github/workflows/ci.yml`.

```sh
python tests/fetch-corpus.py
cmake -S . -B out/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/linux
ctest --test-dir out/linux -LE slow          # slow: long game runs, skipped without the game
```

Add a unit test for new logic (`tests/unit/`, registered in `CMakeLists.txt` or `tools/anypsp/CMakeLists.txt`).
HLE functions are tested directly in `anypsp_hle_tests`; CPU changes must keep `anypsp_cpu_difftest` and
`tests/conformance.py` green.

## Bringing up a game

1. Build it: `anypsp build game.iso -o out/games` (`docs/user/build.md`). The runner lists, at start,
   every import without an HLE implementation.
2. Run it headless with a scripted input (`--headless --deterministic --input frame:mask,...
   --frames-dir dir`) and look at the frames. An unimplemented HLE function stops the run and names
   itself; implement it in `runtime/host/` or, only to get further, stub it with `--hle-stubs`.
3. Feed missed code back: run with `--feedback feedback.txt` and rebuild with `anypsp build ... --feedback feedback.txt`.
4. Open a discussion with the **Game bring-up** template (`.github/DISCUSSION_TEMPLATE/game-bring-up.yml`)
   and add or update `compat/<game>.toml` with the stage and the evidence. Regenerate the page with
   `python tools/compat-site/generate.py --coverage <(out/linux/anypsp-coverage)` to check it.

## Commits

Conventional commits (`feat(hle): ...`, `fix(gpu): ...`, `docs: ...`), one logical change each, describing
behaviour rather than internal task names.
