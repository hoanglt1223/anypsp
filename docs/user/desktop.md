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

## Display settings

Display settings live in `anypsp-display.ini`, next to the program or in the per-user data folder;
changes made with hotkeys while playing are saved to the per-user one. Command-line options override
the file for one run.

```ini
[display]
Smoothing = off
```

| Setting | Option | Hotkey | |
|---|---|---|---|
| `Smoothing` | `--smoothing on\|off` | F9 | Off by default. See below. |

### Smoothing

The game always runs at the PSP's 59.94 vblanks per second. Many games draw only every second vblank
(30 fps), so each picture stays up for two vblanks. Smoothing shows a 50/50 blend of a new picture and
the one before on the vblank where the new one appears, then the new picture alone: motion steps at
60 per second instead of 30. It blends nothing else: games drawing at 60 fps, uneven frame times, and
scene cuts (two pictures that differ a lot, compared on small copies) are shown as they are. Blends
are mixes of two real pictures, not motion estimates, so fast movement looks doubled rather than
warped. Smoothing adds one vblank (about 17 ms) of input latency to the frames it blends. The window
title shows `smoothing` while it is on.

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
Smoothing = F9

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
```

- A value is a comma-separated list (`Cross = Z, Space`); an empty value unbinds. Names you leave out
  keep their defaults.
- Keyboard names are SDL key names (`Return`, `Space`, `Left Shift`, `Keypad 8`, `F1`...).
- Gamepad names are positional: `South`, `East`, `West`, `North`, `Back`, `Guide`, `Start`, `LeftStick`,
  `RightStick`, `LeftShoulder`, `RightShoulder`, `DpadUp`/`Down`/`Left`/`Right`, `LeftTrigger`,
  `RightTrigger`. Every connected gamepad uses the same mapping.
- The left stick of a gamepad is the PSP analog stick; `StickDeadzone` (0-32767) is the travel ignored
  around the centre. The `Analog*` bindings push the stick fully in one direction.
- A misspelled section, name, key or button stops the program with the file name and line.
