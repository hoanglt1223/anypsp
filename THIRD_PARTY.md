# Third-party code in AnyPSP

AnyPSP as a whole is licensed under the GNU General Public License, version 2 or (at your option) any later version (see `LICENSE`). It combines code from the projects below. Their copyright and license notices are kept, and files that came from them keep the license they were published under. Fixes to MIT-origin files that contain no GPL-derived code may be offered back upstream under MIT.

## PSPRecomp framework (MIT): `recomp/`

- Source: [jessicanataliagta/PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) at `f6e7d415c7f447b934cc3865a31eb725f353d659`.
- Imported with history filtered to `src/`, `include/`, `tools/`, `tests/`, `configs/`, `CMakeLists.txt`, `LICENSE`, `docs/ARCHITECTURE.md` and `docs/SOURCE_PROVENANCE.md`, moved under `recomp/`. Each imported commit carries an `Upstream-Commit:` trailer with its original hash. `profiles/` (per-game code and generated game output) was not imported.
- Copyright (c) 2026 PSPRecomp contributors. License text: `recomp/LICENSE`.

## psp-web-recomp framework patches (MIT): `recomp/`

- Source: [snuri00/psp-web-recomp](https://github.com/snuri00/psp-web-recomp) at `37826ab881d9a593efb842fe47adfae2e9118b5d`, `patches/0001` to `patches/0010`, applied in order to the PSPRecomp import.
- Copyright (c) 2026 Samir Nuri. License text: `runtime/LICENSE`.

## psp-web-recomp host runtime (MIT): `runtime/`

- Source: [snuri00/psp-web-recomp](https://github.com/snuri00/psp-web-recomp) at `37826ab881d9a593efb842fe47adfae2e9118b5d`: `profile/host/`, `profile/third_party/`, `profile/CMakeLists.txt` and `LICENSE`, moved under `runtime/` with history filtered to those paths (`Upstream-Commit:` trailers as above).
- `profile/host/pgd.cpp` and `pgd.hpp` were removed from the imported history because `pgd.cpp` embeds KIRK key-vault keys, and AnyPSP ships no keys.
- Copyright (c) 2026 Samir Nuri. License text: `runtime/LICENSE`.

## ATRAC3/ATRAC3+ decoder (LGPL-2.1-or-later): `runtime/third_party/at3_standalone/`

- FFmpeg code by way of PPSSPP's `at3_standalone`, as bundled by psp-web-recomp. Origin commit: `runtime/third_party/at3_standalone/SOURCE_COMMIT`. License text: `runtime/third_party/at3_standalone/COPYING.LGPLv2.1`.

## SDL3 (zlib): fetched at build time, not in the repository

- [libsdl-org/SDL](https://github.com/libsdl-org/SDL) release 3.4.18, pinned by SHA-256 in `runtime/gpu/gpu.cmake` and linked statically into the GPU runners (`anypsp-interp-gpu`, `anypsp-run-<name>-gpu`). It provides the hidden-window OpenGL context, the GL function lookup and PNG writing (SDL's bundled miniz, MIT). License text: `LICENSE.txt` in the fetched source.

## PPSSPP (GPL-2.0-or-later): behaviour reference and data source

- [hrydgard/ppsspp](https://github.com/hrydgard/ppsspp). Files that adapt PPSSPP code or data say so in their header, and are listed here:
  - `recomp/loader/relocs.cpp`: HI16 pairing and the packed relocation format (program header type `0x700000A1`) follow `Core/ELF/ElfReader.cpp` (`LoadRelocations`, `LoadRelocations2`).
  - `data/nids.toml`: library, NID and function names extracted from PPSSPP's HLE function tables (`Core/HLE/*.cpp`) by `tools/gen-nids.py`; the PPSSPP commit is recorded in the file header.
  - `runtime/cpu/` (the reference Allegrex interpreter), adapted from PPSSPP at `902e5ccd8c1400b4387b73942491cded655e5f9c`: `interp_alu.cpp` from `Core/MIPS/Interpreter.cpp`, `interp_vfpu.cpp` from `Core/MIPS/InterpreterVFPU.cpp`, `vfpu_utils.cpp`/`.h` from `Core/MIPS/MIPSVFPUUtils.cpp`/`.h`, `interp_tables.cpp` from `Core/MIPS/MIPSTables.cpp`; `interp.cpp`, `interp_ops.h`, `mips_state.h` and `ppsspp_compat.h` follow `Core/MIPS/MIPS.cpp`/`MIPS.h`, `MIPSTables.h` and `Common/Math/math_util.h`. The copied files keep PPSSPP's copyright header.
  - `runtime/host/kernel.cpp`: `sceDmacMemcpy`, `sceDisplayGetAccumulatedHcount` and `sceUmdReplace*` follow `Core/HLE/sceDmac.cpp`, `sceDisplay.cpp` and `sceUmd.cpp`; the initial thread register state follows `PSPThreadContext::reset` in `Core/HLE/sceKernelThread.cpp`.
  - `tests/conformance.py` compares output the way `headless/Compare.cpp` does.
  - `runtime/host/ge.cpp`: Bezier and spline evaluation (`Ge::draw_curve` and its helpers) adapted from `GPU/Common/SplineCommon.cpp` at `902e5ccd8c1400b4387b73942491cded655e5f9c`; immediate-mode vertices follow `GPU/GPUCommon.cpp` (`Execute_ImmVertexAlphaPrim`, `FlushImm`).

## PSPSDK (BSD): data source

- [pspdev/pspsdk](https://github.com/pspdev/pspsdk) import stubs (`IMPORT_FUNC` lines) supply the NIDs in `data/nids.toml` that PPSSPP does not list; the PSPSDK commit is recorded in the file header.

## Test corpus (not in the repository)

- [hrydgard/pspautotests](https://github.com/hrydgard/pspautotests) binaries and their `.expected` output are fetched by `tests/fetch-corpus.py`, pinned by commit and SHA-256 in `tests/corpus-manifest.txt`, into the git-ignored `tests/corpus/`.
