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

## PPSSPP (GPL-2.0-or-later): behaviour reference and data source

- [hrydgard/ppsspp](https://github.com/hrydgard/ppsspp). Files that adapt PPSSPP code or data say so in their header, and are listed here.

## Test corpus (not in the repository)

- [hrydgard/pspautotests](https://github.com/hrydgard/pspautotests) binaries are fetched by `tests/fetch-corpus.py`, pinned by commit and SHA-256 in `tests/corpus-manifest.txt`, into the git-ignored `tests/corpus/`.
