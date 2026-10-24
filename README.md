# AnyPSP

Game-agnostic static recompilation of PSP games to native Windows, Linux, macOS, and Android executables. No emulation loop: Allegrex/VFPU code is translated to C++ ahead of time and linked against HLE reimplementations of the PSP system modules, in the spirit of [AnyPS5](https://github.com/boykopovar/AnyPS5).

## Status

Early. Phases 01 (base) and 02 (loader and analysis) are done; 03 (code generation) and 04 (HLE) are in progress.

- **CPU.** A reference Allegrex interpreter adapted from PPSSPP (`runtime/cpu/`) covers every integer, FPU and VFPU instruction, VFPU prefixes and PPSSPP's bit-exact special functions. Generated code translates the common integer, memory and FPU instructions inline and calls the interpreter for everything else; a random-state differential test checks the inline translations against it. With the interpreter, 37 of 55 pspautotests CPU tests match their expected output, the same set PPSSPP passes apart from three.
- **First games.** A user's own unencrypted Naruto: Ultimate Ninja Heroes executable runs headless from boot through the menus into a battle, both in the interpreter-only runner and as a translated `psp_recomp` runner (all 32,681 discovered functions translated, no missing code in 1,500 frames, about 1.4x faster than the interpreter on the same 8,000-frame input). Ultimate Ninja Heroes 2 reaches its story mode in both runners (translated: 10,000 frames in 80 s, 1.3x the interpreter). Every import of both games has an HLE implementation. Game files never enter this repository.
- **HLE.** Runners list the imports that have no implementation at start, per library. Calling one stops the run and names it, unless a per-game `--hle-stubs` file lists it. Network modules behave as an offline PSP (WLAN switch off, no ad hoc peers). The kernel has variable and fixed pools, thread suspension and the Refer*Status calls; `anypsp_hle_tests` covers HLE functions directly. pspautotests kernel programs (`threads/`, `io/`, `sysmem/`, `rtc/`) pass 14 of 186 so far (2,011 of 8,611 expected lines).

- **Base.** The [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) framework at `f6e7d41` is under `recomp/`, with [psp-web-recomp](https://github.com/snuri00/psp-web-recomp)'s ten framework patches applied. psp-web-recomp's generic HLE host is under `runtime/`. Provenance is in [THIRD_PARTY.md](THIRD_PARTY.md); the decision is in [docs/dev/decision-base.md](docs/dev/decision-base.md).
- **Loader and analysis** ([docs/dev/loader-and-analysis.md](docs/dev/loader-and-analysis.md)):
  - EBOOT/PRX loading with all three PSP relocation formats;
  - several modules at a fixed layout, with imports linked between them;
  - a 3,953-entry NID database;
  - iterative function discovery: exports, thread entries and other code-pointer APIs, jump tables, tail calls, unreferenced code;
  - delay-slot aware CFGs;
  - trace feedback from the runtime.

  On the pspautotests demos that keep a symbol table, every function symbol is found statically.
- **Headless host.** It builds with MSVC: the HLE kernel and the software GE, with no window and no audio. A runner per translated program runs homebrew and writes feedback. Many pspautotests programs still stop at Allegrex instructions the code generator does not lower yet (phase 03).
- **Not yet:**
  - Vulkan GE (phase 05);
  - desktop and Android hosts with a window (phase 06);
  - message pipes, thread-local pools, virtual timers and callback notification (phase 04);
  - a check of the web/EGL build after the renderer seam.
- Prior art and evaluation: [docs/dev/prior-art.md](docs/dev/prior-art.md).

Build and test:

```
cmake -S . -B out/msvc -G "Visual Studio 17 2022" -A x64
cmake --build out/msvc --config Release
python tests/fetch-corpus.py      # pinned pspautotests binaries into tests/corpus/ (git-ignored)
ctest --test-dir out/msvc -C Release
python tests/feedback-pass.py tests/corpus/pspautotests/demos/threadstatus.elf   # analyze, translate, run headless, feed back
```

Roadmap: [anyport plans](https://github.com/hoanglt1223/anyport/tree/main/plans/261023-1558-anypsp-static-recompiler).

## Legal

This project ships tools only. It does not include, distribute, or require firmware, keys, or game data. Users supply decrypted dumps of games they own.

## License

GPL-2.0-or-later (see [LICENSE](LICENSE)), so PPSSPP-derived code can be used. Imported MIT and LGPL code keeps its notices; see [THIRD_PARTY.md](THIRD_PARTY.md).
