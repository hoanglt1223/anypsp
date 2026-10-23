# AnyPSP

Game-agnostic static recompilation of PSP games to native Windows, Linux, macOS, and Android executables. No emulation loop: Allegrex/VFPU code is translated to C++ ahead of time and linked against HLE reimplementations of the PSP system modules, in the spirit of [AnyPS5](https://github.com/boykopovar/AnyPS5).

## Status

Early. Phase 01 (choosing a base) is done; no recompiler code yet.

- Decision: fork [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) at `f6e7d41`, apply [psp-web-recomp](https://github.com/snuri00/psp-web-recomp)'s generic pipeline patches, and grow a game-agnostic HLE runtime with tracked coverage. See [docs/dev/decision-base.md](docs/dev/decision-base.md).
- Prior art and evaluation: [docs/dev/prior-art.md](docs/dev/prior-art.md).

Roadmap: [anyport plans](https://github.com/hoanglt1223/anyport/tree/main/plans/261023-1558-anypsp-static-recompiler).

## Legal

This project ships tools only. It does not include, distribute, or require firmware, keys, or game data. Users supply decrypted dumps of games they own.

## License

GPL-2.0-or-later (planned, so PPSSPP-derived code can be used). The license file is added together with the fork.
