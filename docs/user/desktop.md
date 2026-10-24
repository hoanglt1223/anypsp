# Playing on the desktop

`anypsp-play` runs a game in a window with sound, keyboard and gamepad input:

```sh
anypsp-play "Game.iso"                # or an extracted disc folder (the one holding PSP_GAME)
anypsp-play "Game.iso" --scale 3 --fullscreen
```

- It boots `PSP_GAME/SYSDIR/BOOT.BIN`, or `EBOOT.BIN` when `BOOT.BIN` is missing or not a plain ELF.
  Encrypted executables (`~PSP` header) are refused: AnyPSP has no keys and decrypts nothing, so supply
  a decrypted executable in an extracted folder.
- An ISO image is extracted once into the per-user data folder (`%APPDATA%\anyport\anypsp\discs\`,
  `~/.local/share/anyport/anypsp/discs/` on Linux). `--cache DIR` puts it elsewhere. Delete the folder to
  reclaim the space; it is extracted again on the next run.
- Saves go to the memory stick folder `memstick/PSP/SAVEDATA/` in the per-user data folder, so they
  persist between runs. `--ms DIR` uses another memory stick folder.
- The game runs at the PSP's 59.94 vblanks per second. The window title shows both rates reached, for
  example `60 Hz / game 30 fps`: `Hz` is the emulation speed (60 is full speed), `game fps` the frames the
  game itself finishes (many PSP games, UNH 1's menus included, draw at 30). Audio is 44.1 kHz stereo; when the host runs ahead of the sound card, new audio is dropped
  rather than queued past 100 ms, so sound never lags behind the picture. `--mute` disables sound.
- `--scale 1`-`4` sets the internal resolution (multiples of 480x272; default 2). The window is
  resizable and the picture keeps its aspect ratio. F11 toggles fullscreen.
- Every per-game translated runner (`anypsp-run-<name>-gpu`) and `anypsp-interp-gpu` accept the same
  `--window` flag and options. They stay headless by default, which is what tests use; `anypsp-play
  --headless` does the reverse.

Headless runs can still record what a window would play: `--wav out.wav` writes the audio output,
`--frames-dir`/`--dump` write frames, and `--input frame:buttons,...` scripts the pad. `--deterministic`
drives guest time from executed code instead of the host clock (and fixes the date), so two scripted
runs produce the same frames.

The GL renderer keeps framebuffers on the GPU. `--coherent lazy` (the default) copies GPU pixels back to
guest memory only when the game's CPU or a DMA copy touches them, and reloads framebuffer memory the
CPU wrote before the GPU uses it again. `--coherent 1` copies back after every display list, which is
much slower and only useful to rule coherence out while debugging; `--coherent 0` never copies back and
breaks games that read their framebuffers (UNH 1 draws its fighters as dark streaks).

## Menu

Escape, F1 or Back+Start on a gamepad pauses the game and opens the menu over a frosted copy of the
picture; the same keys, Circle on its first page, or a click outside it close it. It is drawn at the
window's resolution in its own font, in English or Vietnamese, with a dark or light theme.

- **Keyboard and gamepad** go through the PSP bindings: the d-pad chooses a row, Cross (or Start) acts on
  it, Left/Right change a value, Circle closes. L/R (or Tab and Shift+Tab) switch tabs; Left on a row with
  no value moves to the tab rail, where Up/Down switch tabs. Page Up/Page Down jump in long lists.
- **Mouse**: click a tab, a row or the arrows of a value; the wheel scrolls. **Touch** screens tap and
  drag the same way.

The tabs:

- **Game**: *Resume*, *Reset* (starts the program again with the same command line) and *Exit game*;
  both ask for a second Cross. Game saves are written when the game saves, so nothing is lost.
- **Saves**: the game's saves on the memory stick (`PSP/SAVEDATA/<disc ID>*`), newest first, with their
  icons, titles and dates. AnyPSP has no save states yet, so there are no state slots.
- **Video**: *Internal resolution* (1x-4x, from the next start), *Fullscreen* and *Scaling* (smooth or
  sharp).
- **Audio**: *Volume* (0-100%) and *Mute*; `--wav` recordings are not affected.
- **Controls**: a picture of the PSP with the selected button lit and every binding, keyboard and
  gamepad. Choose one and press the new key or gamepad button; one that belonged to another action
  swaps with it. Escape cancels; *Reset to defaults* restores the bindings.
- **Settings**: *Language* (English, Tiếng Việt), *Theme*, *Menu size* (75-150%), *Reduce motion* (no
  animations) and *Show frame rates* (a badge in the corner: vblanks per second and game frames per
  second).

Changes are saved at once to the input file in use (or `anypsp-input.ini` in the per-user data folder).

## Input mapping

A game looks for `anypsp-input.ini` next to the program, then in the per-user data folder. `--keymap FILE`
names a file explicitly. Without one the defaults below apply. `--write-input FILE` writes the current
bindings to start from.

```ini
[keyboard]
Up = Up
Down = Down
Left = Left
Right = Right
Cross = Z
Circle = X
Square = A
Triangle = S
L = Q
R = W
Start = Return
Select = Backspace
AnalogUp = I
AnalogDown = K
AnalogLeft = J
AnalogRight = L
Fullscreen = F11
Menu = Escape, F1

[gamepad]
Up = DpadUp
Down = DpadDown
Left = DpadLeft
Right = DpadRight
Cross = South
Circle = East
Square = West
Triangle = North
L = LeftShoulder
R = RightShoulder
Start = Start
Select = Back

[player]
Scale = 2
StickDeadzone = 8000
Volume = 100
Mute = off
ShowFps = off
Filter = smooth
Language = en
Theme = dark
UiScale = 100
ReduceMotion = off
```

- A value is a comma-separated list (`Cross = Z, Space`); an empty value unbinds. Names you leave out
  keep their defaults.
- Keyboard names are SDL key names (`Return`, `Space`, `Left Shift`, `Keypad 8`, `F1`...).
- Gamepad names are positional: `South`, `East`, `West`, `North`, `Back`, `Guide`, `Start`, `LeftStick`,
  `RightStick`, `LeftShoulder`, `RightShoulder`, `DpadUp`/`Down`/`Left`/`Right`, `LeftTrigger`,
  `RightTrigger`. Every connected gamepad uses the same mapping.
- The left stick of a gamepad is the PSP analog stick; `StickDeadzone` (0-32767) is the travel ignored
  around the centre. The `Analog*` bindings push the stick fully in one direction.
- `Menu` opens the in-game menu; Back+Start on a gamepad always does too.
- `[player]`: `Volume` 0-100 in steps of 10, `Mute`/`ShowFps`/`ReduceMotion` on or off, `Filter` smooth or
  sharp, `Language` en or vi, `Theme` dark or light, `UiScale` 75, 100, 125 or 150. The menu writes them.
- A misspelled section, name, key or button stops the program with the file name and line.
