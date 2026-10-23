# Prior art: PSP static recompilation (surveyed 2026-10-23)

Phase 01 survey for AnyPSP. Everything below was checked against the repositories on 2026-10-23 (GitHub API for metadata, shallow clones for code). The PSP recomp scene grew from nothing to more than twenty repositories between June and October 2026, mostly per-game ports and mostly AI-assisted, so the numbers will go stale quickly.

## The "PSPRecomp" name covers three unrelated frameworks

The plan's overview describes PSPRecomp as "a Rust analyser emitting C++17, SDL2 + OpenGL 3.3" behind the GTA LCS, Ace Combat X and PSP2i ports. That conflates three separate codebases:

| Framework | Language / output | Used by |
|---|---|---|
| [jessicanataliagta/PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) | C++20 analyser and codegen, emits C++ | GTA VCS (in-repo profile), GTA LCS, the VCS/LCS Android, Xbox and macOS ports, PSP Web Recomp (God of War CoO and GoS) |
| [sp00nznet/psprecomp](https://github.com/sp00nznet/psprecomp) (`refs/psprecomp` is the [zmbkilla fork](https://github.com/zmbkilla/psprecomp)) | C11 analyser, emits C | WTF: Work Time Fun, Phantasy Star Portable 2 Infinity, Killzone: Liberation |
| [wizardengineer/psprecomp](https://github.com/wizardengineer/psprecomp) | Rust + Ghidra analyser, emits C++17; SDL2 + OpenGL 3.3 runtime | Patapon (title screen), .hack//Link (loading screen) |

Ace Combat X uses a fourth codebase of its own (sal063's "pspreco").

## Frameworks

| Repo | License | Created / last push | Commits | Stars / forks | Maintainers | Games reached |
|---|---|---|---|---|---|---|
| jessicanataliagta/PSPRecomp | MIT | 2026-08-12 / 2026-08-28 | 13 | 236 / 33 | jessicanataliagta alone; 8 open PRs from 4 outside authors, 1 merged | GTA VCS playable (Windows, DX12) |
| sp00nznet/psprecomp | MIT | 2026-07-19 / 2026-10-23 | 186 | 21 / 9 | sp00nznet; 18 merged PRs from 4 other contributors | WTF title screen (v0.5.0 alpha, 2026-10-23) |
| zmbkilla/psprecomp (fork of the above) | MIT | 2026-10-04 / 2026-10-20 | 176 (52 ahead, 62 behind upstream) | 0 / 1 | zmbkilla | PSP2i (via PSP2I_Recomp) |
| wizardengineer/psprecomp | GPL-2.0 | 2026-06-12 / 2026-10-02 | 92 | 16 / 1 | wizardengineer (+1 contributor) | Patapon title screen; .hack//Link loading screen |
| [sal063/PSP-recompilation-project](https://github.com/sal063/PSP-recompilation-project) | GPL-2.0-or-later | 2026-06-13 / 2026-06-14 | 5 | 49 / 5 | sal063 | none stated; dormant |

## Per-game ports and derived projects

| Repo | Base | License | Last push | Commits | Stars | Targets / notes |
|---|---|---|---|---|---|---|
| [snuri00/psp-web-recomp](https://github.com/snuri00/psp-web-recomp) | PSPRecomp f6e7d41 + 10 patches | MIT (ATRAC decoder LGPL-2.1+) | 2026-10-15 | 23 | 382 | Browser (Emscripten, WebGL2); native headless runner on Linux. God of War CoO and GoS playable at ~60 fps. Generic `port.sh <game> <iso>` flow, no game code in the repo |
| [elmasas/lcs-recomp](https://github.com/elmasas/lcs-recomp) | PSPRecomp copy | MIT | 2026-10-18 | 40 | 254 | GTA LCS (ULUS-10041 v1.05), Windows, VS 2022 + clang-cl. Commits ~96 MB of generated game code in `lcs/generated/` |
| [OverkillLabs3/vice-city-stories-recomp](https://github.com/OverkillLabs3/vice-city-stories-recomp) | PSPRecomp | MIT | 2026-10-05 | 3 | 29 | GTA VCS, Windows + macOS, Vulkan, playable release |
| [codepdbh/PSPRecomp-LCS-Android](https://github.com/codepdbh/PSPRecomp-LCS-Android), [-VCS-Android](https://github.com/codepdbh/PSPRecomp-VCS-Android) | PSPRecomp | MIT | 2026-10-22 | 33 / 25 | 10 / 37 | Android arm64 |
| [zmbkilla/PSP2I_Recomp](https://github.com/zmbkilla/PSP2I_Recomp) | zmbkilla/psprecomp (submodule) | none | 2026-10-20 | 87 | 8 | PSP2i: Windows, Android, macOS, D3D11. No license file, so it cannot be reused |
| [sal063/Ace-Combat-X-Recompilation](https://github.com/sal063/Ace-Combat-X-Recompilation) | own (Python analyser, C runtime) | GPL-2.0 | 2026-08-28 | 10 | 84 | ACX (ULUS10176), Windows, SDL3 + Vulkan. GE rules derived from PPSSPP softgpu; PGF font ported from PPSSPP |
| [sp00nznet/wtf-psp-recomp](https://github.com/sp00nznet/wtf-psp-recomp) | sp00nznet/psprecomp | MIT | 2026-10-02 | — | 1 | Reference game for the toolkit |
| [DrummingBird1/gow-psp-recomp](https://github.com/DrummingBird1/gow-psp-recomp) | own, PPSSPP-referenced | GPL-3.0-or-later | 2026-10-23 | 89 | 32 | GoW CoO and GoS, native PC |
| highcmd (Chinatown Wars, Half-Minute Hero), Tzeentchnet (Killzone), azazel6 (Tekken 6), CarsPSPRecomp | various | GPL-3.0 / MIT / other | Oct 2026 | — | 0–4 | Single-game, early |

Press coverage of the LCS and VCS ports: [ixbt.games](https://ixbt.games/en/news/2026/09/25/spustia-21-god-na-pk-perenesli-gta-liberty-city-stories-poiavilsia-fanatskii-port-igry.html), [gtaboom](https://www.gtaboom.com/vice-city-stories-runs-natively-on-pc-now-no-emulator-needed-0148).

## Assessment from the code

### jessicanataliagta/PSPRecomp (plus psp-web-recomp)

- **Generic vs per-game.** The framework (`src/`, `tools/`, `include/`) is about 9,500 lines and contains no game addresses. Everything else is per-game by design: `docs/ARCHITECTURE.md` says a profile supplies "generated guest code, HLE functions, title bootstrap, compatibility behavior, renderer/audio/input integration, native guest-leaf replacements". The VCS profile has 33,000 hand-written lines (`vcs_profile.cpp` alone is 10,484, with 92 hard-coded `0x08xxxxxx` guest addresses) and commits 2.3 million lines of generated game code.
- **psp-web-recomp proves the framework can be game-agnostic.** Its `profile/host` is 14,500 lines of MIT code with no game-specific logic (two mentions of a disc ID, in PGD decryption). It covers a cooperative kernel (threads, semaphores, event flags, callbacks, partitions), I/O with streaming, controller, audio, SAS, ATRAC, savedata dialogs, fonts, and the MPEG bookkeeping, about 300 NIDs. Its 10 patches add runtime translation and unloading of the game's own PRX modules (0006), relocation-driven function seeding (0008), and an interpreter fallback for untranslated code (0010). The 10 patches apply cleanly in sequence to PSPRecomp HEAD f6e7d41, which is also the commit `setup.sh` pins.
- **VFPU.** Implemented inside `allegrex_context.hpp` (1,455 lines): source/destination prefixes, `vhdp`, matrix operations. Patch 0007 adds random numbers, integer conversion and colour packing. On pspautotests' `cubevfpu.prx` the codegen leaves 7 VFPU/COP2 sites unlowered.
- **HLE structure.** The framework only has `register_hle(library, nid, fn)` and a 43-entry `nids.csv`. Modules live in the host. psp-web-recomp logs `[hle] unimplemented <name>` and continues, which is looser than AnyPS5's rule.
- **Renderer.** Per profile. VCS uses DX12 with SMAA and HDR. psp-web-recomp decodes GE lists on the CPU (`ge.cpp`, 1,726 lines: vertex formats, skinning, lighting, clipping) and draws with WebGL2 (`ge_gl.cpp`): VRAM-keyed render targets, format reinterpretation, stencil-in-alpha, 1–4x scaling, and the GE on its own worker thread.
- **Hosts.** Windows (VCS, LCS), macOS and Android arm64 (downstream ports), browser and Linux headless (psp-web-recomp).
- **Build.** CMake 3.20, C++20, no external dependencies in the framework.
- **Quality.** Compact and readable, with one 1,782-line test binary. The upstream maintainer has been inactive since 2026-08-28 and has merged one PR in total. Downstream projects copy the code rather than send fixes back.

### sp00nznet/psprecomp (zmbkilla fork in `refs/`)

- **Generic vs per-game.** The toolkit has no game addresses. Each game lives in its own repo and supplies the host: generated code calls `psp_syscall` and `psp_unimplemented`, which only the game repo defines.
- **Scope.** It is the only toolkit that peels the whole container stack (ISO9660, PBP, `~PSP`/`~SCE`) and does KIRK decryption itself, with keys supplied by the user.
- **VFPU.** `src/vfpu.c` (911 lines) covers arithmetic, matrix operations and prefixes. On pspautotests binaries the emitter leaves `vdet`, `vhdp` and 15–19 unnamed VFPU encodings as traps, plus a few branch-likely sites in delay slots.
- **HLE.** One file per module, `hle_register` keyed by NID, with each NID checked against SHA-1 of its name. The upstream README lists 112 registrations. The zmbkilla fork has 424, including net, adhoc and NP for PSP2i. Unknown NIDs log and return 0.
- **Renderer.** A backend interface (`render.h`) with the list interpretation kept separate, a software rasterizer, and DXT textures in the fork. Threading is an experimental opt-in.
- **Build.** CMake, C11, zero dependencies, `/bigobj` for generated code.
- **Quality.** The best documentation of the set (`BRINGUP.md` records retractions as well as findings) and the best bring-up instruments. Upstream is active and merges PRs. Its stated policy is a hard MIT boundary with no PPSSPP code.

### wizardengineer/psprecomp

- GPL-2.0. Function discovery needs Ghidra 12.0.2 with a version-pinned ghidra-allegrex extension and JDK 21, run once per binary.
- The runtime core is enforced game-agnostic by a purity gate, and 237 NIDs are bound per game. It runs on macOS only and is described as "vibe coded" with no human review.
- The renderer has a known degenerate-matrix fallback that hard-codes Patapon's viewport. There is no audio, and GE lighting, skinning and transfers are missing.

### sal063 (ACX and the standalone toolkit)

- GPL-2.0(+). C runtime, Python analyser, SDL3 + Vulkan GE with PPSSPP-derived pixel rules, an interpreter for cold paths, a JSON NID database, and a VFPU table.
- The runtime is tied to ACX in about 230 places (`acx_api.c`, hooks, QoL input). It has one maintainer and both repos have been dormant since August.
- Useful as GPL-compatible reference code for a Vulkan GE.

## Build test on this PC (no game data)

Inputs: open-source pspsdk-built test binaries from [hrydgard/pspautotests](https://github.com/hrydgard/pspautotests) (`cpu_alu.prx`, `fpu.prx`, `vfpu/matrix.prx`, `demos/cubevfpu.prx`). All builds ran in copies under `%TEMP%`. MSVC 14.44 was used through the Visual Studio 2022 CMake generator.

| Framework | Build | Tests | Homebrew pipeline |
|---|---|---|---|
| zmbkilla/psprecomp (`refs/` HEAD 7e31846) | OK, 1 warning | 14/14 ctest pass | `info`, `cover`, `funcs` and `emit` work on all four PRX files (matrix.prx: 3,239 functions, 96% of `.text` reached, 55 imports). The generated C compiles and links against the runtime once two host hooks are stubbed. Not run: there is no generic host. |
| jessicanataliagta/PSPRecomp f6e7d41, framework only | OK | 1/1 pass | `psp_analyze` and `psp_recomp --auto` work on cubevfpu (350 seeds, 1,301 blocks, 32 imports) |
| Same, with the 10 psp-web-recomp patches | OK | 1/1 pass | `--auto` works on cubevfpu and matrix (886 seeds, 81 import wrappers) |
| wizardengineer/psprecomp (Rust crates) | `cargo build --release` OK (Rust 1.97) | 304 pass, 1 fails only because no `c++` is on PATH | `analyze` not run: needs Ghidra 12.0.2 + JDK 21 |

Executing the recompiled homebrew was not tested. None of the frameworks ships a generic native host for Windows. psp-web-recomp's runner needs Linux EGL/GLES, and Docker/WSL are not installed on this PC.

## Trade-off matrix

Scores run from 1 (poor) to 5 (strong), judged for AnyPSP's goals.

| Dimension | PSPRecomp + web host | sp00nznet psprecomp | wizardengineer | sal063 ACX | From scratch |
|---|---|---|---|---|---|
| Proven on commercial games | 5 (VCS, LCS, GoW x2 playable) | 2 (title screen; PSP2i on fork) | 1 | 4 (ACX) | 0 |
| Game-agnostic today | 4 (web host) | 3 (toolkit yes, host per game) | 3 | 2 | — |
| Codegen performance work | 5 (hot registers, unit chaining, 60 fps) | 2 | 2 | 3 | — |
| VFPU coverage | 3 | 3 | 3 | 3 | 0 |
| HLE breadth | 4 (~300 NIDs, generic) | 4 (424 on fork, network-heavy) | 3 | 4 | 0 |
| Renderer fit (Vulkan, scaling) | 3 (GL; CPU-side decode reusable) | 2 (software) | 2 | 4 (Vulkan) | 0 |
| Upstream health | 2 (inactive) / 3 (web) | 5 | 3 | 1 | — |
| License fit with PPSSPP adaptation | 5 (MIT folds into GPL) | 2 (policy forbids it upstream) | 5 | 5 | 5 |
| Toolchain friction on Windows | 5 | 5 | 2 (Ghidra/JDK, macOS) | 3 (MSYS2) | — |

## Sources

- Repositories and metadata: links in the tables above, via `gh api` on 2026-10-23.
- [uofw/uofw](https://github.com/uofw/uofw) (MIT firmware reverse engineering), [pspautotests](https://github.com/hrydgard/pspautotests), [ghidra-allegrex](https://github.com/kotcrab/ghidra-allegrex).
- PPSSPP license: `refs/ppsspp/Core/HLE/*.cpp` headers, "GPL version 2.0 or later".
