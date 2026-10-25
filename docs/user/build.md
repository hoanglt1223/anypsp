# Building a game with `anypsp build`

`anypsp build` turns your own decrypted PSP game into a native program: it analyses the executable
(`anypsp-analyze`), translates it to C++ (`psp_recomp --auto`), compiles that with the AnyPSP runtime and
packages the result.

```sh
anypsp build "Game.iso" -o games                     # or an extracted disc folder, EBOOT.BIN, EBOOT.PBP, module.prx
anypsp build "Game.iso" -o games --target android    # an APK (docs/user/android.md)
games/<Title>/<Title> --headless --frames 600         # what CI does; without options it opens a window
```

- **Input.** An ISO image or an extracted disc folder boots `PSP_GAME/SYSDIR/BOOT.BIN`, or `EBOOT.BIN` when
  `BOOT.BIN` is not a plain ELF, as `anypsp-play` does. Encrypted executables are refused: AnyPSP has no keys
  and decrypts nothing. A lone ELF, PRX or EBOOT.PBP (homebrew) works too.
- **Output.** `<output>/<Name>/` holds `<Name>` (`<Name>.exe` on Windows), `nids.toml`, `LICENSE.txt`,
  `README.txt` and a copy of the game files: `game.iso`, the disc folder as `game/`, or `EBOOT.BIN`/`EBOOT.PBP`.
  `--no-game` leaves the game files out; the program then takes the game as its first argument. On macOS the
  result is `<Name>.app` with the game under `Contents/Resources`, signed ad hoc (`--codesign ID` for a
  Developer ID). The name is the disc title (`--name` to change it).
- **The program** is the desktop player of `docs/user/desktop.md` with the game's translated code: a window
  with sound, keyboard and gamepad by default, the same menu, settings files and options (`--headless`,
  `--frames`, `--input`, `--scale`, ...; `--help` lists them). It checks at start that the game's executable
  is the one it was translated from (SHA-256), since translated code only fits that exact binary.
- **Work folder.** `--work DIR` (default `<output>/<input name>-build`) keeps the extracted disc, the analysis,
  the generated code (`gen/`) and the CMake build tree (`out-<target>/`). Rebuilding with the same work folder
  only recompiles what changed.
- **Speed and size.** The generated code of a commercial game is tens of thousands of functions in a few dozen
  large C++ files: allow several GB of memory per compile job (`--jobs N`) and from minutes to an hour.
  `--opt d` compiles fastest; `1` (default) and `2` run faster.
- **Code the analysis missed** runs in the reference interpreter, so the game still works. To translate it
  too, play with `--feedback feedback.txt` and rebuild with `anypsp build ... --feedback feedback.txt`.

## Requirements

- CMake 3.20+, a C++20 compiler (MSVC 2022, clang 16+ or GCC 13+), Ninja (optional, used when on the PATH).
- Linux: the SDL3 build dependencies (X11/Wayland, ALSA/PulseAudio, Mesa headers; the list is in
  `.github/workflows/ci.yml`).
- The AnyPSP sources: a release package has them in `share/anypsp/source` with SDL3 in
  `third_party/SDL3-src.tar.gz`, so no download is needed; a source build uses its own tree (`--source DIR`
  to choose another).

## Hosts

| Target | Build it on | Notes |
|---|---|---|
| `linux` | Linux x64 | verified end to end (UNH 1, `compat/naruto-ultimate-ninja-heroes.toml`) |
| `linux` arm64 | Linux, `--toolchain <source>/cmake/toolchains/aarch64-linux-gnu.cmake` | cross build with clang and the GNU sysroot (`apt install g++-aarch64-linux-gnu`); a handheld's own sysroot via `ANYPSP_AARCH64_SYSROOT` gives SDL3 its display and audio backends |
| `windows` | Windows x64 with Visual Studio 2022 | run from a Developer prompt or with CMake and MSVC on the PATH; the program is a window application without a console |
| `macos` | macOS arm64 with Xcode command-line tools | produces a signed `.app` |
| `android` | any of the above with the Android SDK and NDK | `docs/user/android.md` |

Desktop programs are built on the system they run on (except cross builds through `--toolchain`); Android
apps build from any desktop host.

## Windows

```bat
cmake -S . -B out\msvc -G "Visual Studio 17 2022" -A x64
cmake --build out\msvc --config Release
out\msvc\Release\anypsp.exe build D:\Games\Game.iso -o D:\Games\anypsp
```

The game build uses the Visual Studio generator unless Ninja is on the PATH; generated units compile with
`/bigobj`. Keep the work folder short (for example `--work D:\w\game`): MSBuild paths are limited to 260
characters.
