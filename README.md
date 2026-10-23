# AnyPSP

Game-agnostic static recompilation of PSP games to native Windows, Linux, macOS, and Android executables. No emulation loop: Allegrex/VFPU code is translated to C++ ahead of time and linked against HLE reimplementations of the PSP system modules, in the spirit of [AnyPS5](https://github.com/boykopovar/AnyPS5).

## Status

Early. Phase 01 (choosing a base) is done. Phase 02 (loader and analysis) has started; no AnyPSP-specific loader or analysis code exists yet.

- The base is imported: the [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) framework at `f6e7d41` under `recomp/`, with [psp-web-recomp](https://github.com/snuri00/psp-web-recomp)'s ten framework patches applied, and psp-web-recomp's generic HLE host under `runtime/`. Provenance is in [THIRD_PARTY.md](THIRD_PARTY.md); the decision is in [docs/dev/decision-base.md](docs/dev/decision-base.md).
- What works on Windows (CMake + MSVC): the framework, `psp_analyze`, `psp_recomp --auto` and the framework test.
- Not yet: the host runtime only builds for the web (Emscripten) or Linux EGL, so recompiled code cannot run on Windows yet. PRX files without section headers (or with packed `0x700000A1` relocations) are not loaded correctly yet.
- Prior art and evaluation: [docs/dev/prior-art.md](docs/dev/prior-art.md).

Build and test:

```
cmake -S . -B out/msvc -G "Visual Studio 17 2022" -A x64
cmake --build out/msvc --config Release
ctest --test-dir out/msvc -C Release
python tests/fetch-corpus.py      # pinned pspautotests binaries into tests/corpus/ (git-ignored)
```

Roadmap: [anyport plans](https://github.com/hoanglt1223/anyport/tree/main/plans/261023-1558-anypsp-static-recompiler).

## Legal

This project ships tools only. It does not include, distribute, or require firmware, keys, or game data. Users supply decrypted dumps of games they own.

## License

GPL-2.0-or-later (see [LICENSE](LICENSE)), so PPSSPP-derived code can be used. Imported MIT and LGPL code keeps its notices; see [THIRD_PARTY.md](THIRD_PARTY.md).
