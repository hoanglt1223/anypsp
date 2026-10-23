# Decision: AnyPSP base and license (2026-10-23)

Evidence is in [prior-art.md](prior-art.md).

## Decision

**Fork, not contribute upstream and not start fresh.** AnyPSP is a new repo, licensed **GPL-2.0-or-later**. It starts from two MIT sources:

1. The **[jessicanataliagta/PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) framework** at commit `f6e7d41` with the **10 [psp-web-recomp](https://github.com/snuri00/psp-web-recomp) patches** applied. It is vendored as a subtree and includes only `src/`, `include/`, `tools/`, `tests/` and `configs/`. Never import `profiles/`: it holds per-game code and committed game-derived output.
2. **psp-web-recomp's `profile/host`** as the seed of the generic HLE runtime. It gets restructured into the phase-04 layout. Its WebGL2 backend is replaced by Vulkan in phase 05, and its CPU-side GE decoding is kept.

Upstream relations:

- Generic fixes to files that came from MIT upstreams are offered back as PRs to psp-web-recomp, which is active, and to PSPRecomp if it becomes active again.
- [sp00nznet/psprecomp](https://github.com/sp00nznet/psprecomp) (MIT) is a reference and a source of MIT code. The candidates are its bring-up instruments, its SHA-1 NID verification, and VFPU and HLE pieces where they are better.
- [sal063/Ace-Combat-X-Recompilation](https://github.com/sal063/Ace-Combat-X-Recompilation) (GPL-2.0) is the reference for a Vulkan GE.
- PPSSPP (GPL-2.0+) is the behaviour oracle and may be a code source.

### Why this base

- It is the only codegen proven on several commercial titles from different studios: GTA VCS and LCS (Rockstar Leeds/London), and God of War CoO and GoS (Ready at Dawn), all playable. It also has the performance work needed for 60 fps (hot-register caching, unit chaining).
- psp-web-recomp already shows the game-agnostic shape AnyPSP wants. One script turns a user's ISO into a build, there is no game code in the repo, the HLE is generic (about 300 NIDs), PRX modules loaded at runtime are translated, and an interpreter catches missed code. That covers much of phases 02–04, which become adaptation work instead of new work.
- It builds with no dependencies on this PC (CMake + MSVC). `--auto` codegen works on open-source homebrew with no per-game config, which `anypsp build EBOOT.BIN` needs.
- It emits C++, which matches the plan's "Allegrex/VFPU to C++" design.

### Why not contribute upstream

- PSPRecomp's maintainer has been inactive since 2026-08-28: one merged PR, eight left open. Its architecture puts HLE and the renderer in per-game profiles, which is the opposite of AnyPSP's goal. Every downstream project has copied it rather than sent fixes back.
- sp00nznet/psprecomp is healthy and welcoming, with 18 merged PRs, and it is the right place for MIT-clean toolkit fixes. But it has not yet run a commercial game past a title screen. Its renderer is software-only, threading is experimental, the host stays per game, and its stated policy forbids PPSSPP-derived code. AnyPSP needs that code to reach five titles in a reasonable time.

### Why not other options

- **wizardengineer/psprecomp:** needs Ghidra and JDK 21 for discovery, runs on macOS only, has one title screen, and is AI-generated without review.
- **sal063 ACX:** tied to one game in about 230 places and dormant.
- **From scratch:** would duplicate months of codegen and HLE work that already runs games, which the plan names as a key risk.

## License: GPL-2.0-or-later

- Phases 03–05 rely on PPSSPP behaviour where adapting its code saves the most time: VFPU special-function precision, sceAtrac, sceMpeg/PSMF, the PGF font, GE pixel rules, and the vertex decoder. Adapting it requires GPL-2.0+.
- MIT code (PSPRecomp, psp-web-recomp, sp00nznet) may be combined into a GPL project. Keep its copyright and MIT notice in `THIRD_PARTY.md` and in the file headers.
- LGPL-2.1+ code (the `at3_standalone` ATRAC decoder) is compatible.
- **Do not take GPL-3.0 code** (gow-psp-recomp, the highcmd ports). It would force the combined work to GPL-3.0. Re-decide only if a GPL-3.0 source becomes essential.
- To keep upstreaming possible, put PPSSPP-derived code only in new files under GPL headers. Fixes to MIT-origin files must not contain PPSSPP-derived code, so they can still be sent upstream under MIT.
- Never commit or publish generated output. It is a translation of the game and belongs to its owner, which is how psp-web-recomp handles it. The VCS and LCS repos commit it, and AnyPSP must not.

## Gaps AnyPSP must still close on this base

| Gap | Phase |
|---|---|
| Unknown NIDs log and continue; AnyPS5 strictness needs a throw, with stubs only through per-game config | 04 |
| No NID-name database beyond a 43-row CSV; build one from public NID lists, with per-NID coverage output | 02 |
| VFPU sites left unlowered on pspautotests (7 in cubevfpu); run the pspautotests CPU/VFPU suites as differential tests | 03 |
| WebGL2 to Vulkan GE backend, with MoltenVK on macOS and Android Vulkan | 05 |
| Native desktop and Android hosts (the web host is Emscripten-only; the native runner is Linux EGL headless) | 06 |
| A Windows-runnable generic host is missing, so homebrew could not be executed in phase 01 | 02 (first task) |

## Exit-criteria status

The decision is written. Agreement with upstream is still open: no maintainer has been contacted, because that needs the user's go-ahead. The draft messages are below.

### Draft to snuri00 (psp-web-recomp), as an issue or discussion

> Hi, I'm starting AnyPSP, a game-agnostic native (desktop and Android, Vulkan) target for the PSPRecomp-based pipeline your web profile proved out. I'd like to base it on PSPRecomp f6e7d41 plus your 10 patches and use your `profile/host` as the starting point for a generic native HLE layer. AnyPSP will be GPL-2.0-or-later because I want to adapt some PPSSPP code. Your MIT files keep their notices, and any fix to them that doesn't involve PPSSPP code I'd send back to you as a PR under MIT. Two questions: are you open to PRs that make the host less web-specific (a backend seam between `ge.cpp` and `ge_gl.cpp`)? And do you plan to upstream your patches to PSPRecomp, or keep them as a patch series?

### Draft to jessicanataliagta (PSPRecomp), as an issue

> Hi, thank you for PSPRecomp. I'm building a game-agnostic native target on it (framework only, no profiles), building on psp-web-recomp's patches. Are you still accepting PRs? Several framework fixes are sitting in open PRs and in downstream patch series (scratchpad mapping, ADDI decode, PRX translation, interpreter fallback). If you'd like them consolidated, I can open one PR per fix.

## Re-open this decision if

- psp-web-recomp or PSPRecomp adopts a generic native host upstream (then contribute instead of fork).
- sp00nznet/psprecomp reaches gameplay in two commercial titles while AnyPSP's base has not moved.
- Phase 03 differential tests show that PSPRecomp's VFPU or CPU model needs a rewrite rather than fixes.
