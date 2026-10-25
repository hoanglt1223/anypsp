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
- The game runs at the PSP's 59.94 vblanks per second. The window title shows the rates reached
  ([Display settings](#display-settings)): `vblank/s` is the emulation speed (60 is full speed), `game fps`
  the frames the game itself finishes (many PSP games, UNH 1's menus included, draw at 30). Audio is 44.1 kHz stereo; when the host runs ahead of the sound card, new audio is dropped
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

Escape, F1, Pause or Back+Start on a gamepad pauses the game and opens the menu over a frosted copy of the
picture; the same keys, Circle on its first page, or a click outside it close it. It is drawn at the
window's resolution in its own font, in English or Vietnamese, with a dark or light theme.

- **Keyboard and gamepad** go through the PSP bindings: the d-pad chooses a row, Cross (or Start) acts on
  it, Left/Right change a value, Circle closes. L/R (or Tab and Shift+Tab) switch tabs; Left on a row with
  no value moves to the tab rail, where Up/Down switch tabs. Page Up/Page Down jump in long lists.
- **Mouse**: click a tab, a row or the arrows of a value; the wheel scrolls. **Touch** screens tap and
  drag the same way.

The tabs:

- **Game**: *Resume*; a *Speed* section with *Speed* and *Fast-forward (hold)* (see [Speed](#speed));
  then *Reset* (starts the program again with the same command line) and *Exit game*; both ask for a
  second Cross. Game saves are written when the game saves, so nothing is lost.
- **Saves**: the game's saves on the memory stick (`PSP/SAVEDATA/<disc ID>*`), newest first, with their
  icons, titles and dates. AnyPSP has no save states yet, so there are no state slots.
- **Video**: *Graphics preset* (see [Graphics presets](#graphics-presets)), *Internal resolution* (1x-4x, from the next start), *Fullscreen*, *Scaling* (smooth or
  sharp), then the [display settings](#display-settings) *Smoothing*, *Frame interpolation* (turning it on applies from the next start), *Refresh*, *Variable refresh*,
  *Sync game to monitor*, *Menu FPS* and *FPS cap*, saved to `anypsp-display.ini` like the hotkeys, and
  *Game patches*, which turns the loaded [patch file](#game-patches) off and on for this run.
  Beside the rows a live preview shows the paused game and, under it, what the focused setting does
  (see [Video preview](#video-preview)).
- **Audio**: *Volume* (0-100%) and *Mute*; `--wav` recordings are not affected.
- **Controls**: a picture of the PSP with the selected button lit and every binding, keyboard and
  gamepad. Choose one and press the new key or gamepad button; one that belonged to another action
  swaps with it. Escape cancels; *Reset to defaults* restores the bindings.
- **Settings**: *Language* (English, Tiếng Việt), *Theme*, *Menu size* (75-150%), *Reduce motion* (no
  animations) and *Show frame rates* (a badge in the corner: vblanks per second and game frames per
  second).

Changes are saved at once to the input file in use (or `anypsp-input.ini` in the per-user data folder).

### Video preview

The Video tab's preview is the paused game picture split by a vertical divider: on the left (*Before*) the
picture enlarged nearest-neighbour, on the right (*After*) as *Scaling* shows it in the window. Both are
made at the size the game takes in the window and shrunk into the panel, so the loupe shows real pixels.
The picture is the one the game drew at the internal resolution in use; a different internal resolution,
smoothing, frame interpolation and the timing settings only show while the game runs (their descriptions
say so).

- **Divider**: drag it with the mouse (pressing anywhere on the picture moves it there) or a finger started
  on it; `[` and `]` or the gamepad's left and right triggers move it a tenth at a time.
- **Loupe**: a 4x circle that follows the mouse over the picture, or a finger held on the picture (it sits
  above the finger). Select (Backspace; gamepad Back) turns it on and off for the keyboard and gamepad;
  then the right stick or Shift with the d-pad moves it.

### Graphics presets

The first Video row, *Graphics preset*, sets the quality settings at once: Left/Right step through Very
low, Low, Medium, High, Very high and Ultra (wrapping around). The row shows the preset whose values are
all in effect, or *Custom* once one of them is changed by hand; changing it back shows the preset again.
Nothing extra is saved: the preset is worked out from `Scale` and `Filter` in the input file and
`Smoothing` and `Interpolate` in `anypsp-display.ini`.

| Preset | Internal resolution | Scaling | Smoothing | Frame interpolation |
|---|---|---|---|---|
| Very low | 1x | sharp | off | off |
| Low | 1x | smooth | off | off |
| Medium (the defaults) | 2x | smooth | off | off |
| High | 3x | smooth | on | off |
| Very high | 3x | smooth | on | on |
| Ultra | 4x | smooth | on | on |

- Internal resolution and turning frame interpolation on apply from the next start, as on their own rows.
- `Interpolate = auto` counts as what it decided for this game (on for the games it lists, else off). A
  preset leaves `auto` alone when that already matches, and otherwise writes an explicit `on` or `off`.
- Very high stays at 3x so that it differs from Ultra; smoothing stays on with interpolation, which wins
  when both are on (smoothing still applies while interpolation is not active in this run, for example
  until the next start).
- Not part of a preset: fullscreen, refresh, variable refresh, sync to monitor, Menu FPS, FPS cap and game
  patches.

## Speed

*Speed* (100, 200, 300 or 400%) sets how fast the game runs; while the FastForward key is held (Space
or the right trigger by default, rebindable in the Controls tab or `anypsp-input.ini`) it runs at
*Fast-forward (hold)* instead: 150-800% or unlimited. Faster speeds run several PSP vblanks for each
picture shown, spread evenly (1 and 2 in turn at 150%), so frame pacing and smoothing work as at 100%
(smoothing pauses while faster). The sound plays faster and higher to match up to 400%, and is muted
above that and at unlimited, which shows a picture about every 1/60 s and runs the game as fast as the
PC can in between. `--wav` recordings keep every sample. The window title and the frame-rate badge
show the speed whenever it is not 100%. Both are `[player]` settings (`Speed`, `FastForwardSpeed`).

## Display settings

Display settings live in `anypsp-display.ini`, next to the program or in the per-user data folder;
changes made with hotkeys while playing are saved to the per-user one. Command-line options override
the file for one run.

```ini
[display]
Smoothing = off
Interpolate = auto
Refresh = auto
VRR = off
SyncToMonitor = off
PauseFps = 30
FpsCap = off
```

| Setting | Option | Hotkey | |
|---|---|---|---|
| `Smoothing` | `--smoothing on\|off` | F9 | Off by default. See below. |
| `Interpolate` | `--interpolate auto\|on\|off` | F10 | `auto` (the default) turns it on in a window for games that draw at 30 fps and interpolate well (UNH 1, ULUS-10299) and off for others and headless runs. Read when the game starts; F10 switches it on and off at once when the game started with it on, otherwise from the next start. See [Frame interpolation](#frame-interpolation). |
| `Refresh` | `--refresh auto\|60\|75\|90\|120\|144\|165\|240` | F5 cycles | `auto` reads the refresh of the monitor the window is on (again every second, so moving the window is followed). |
| `VRR` | `--vrr on\|off` | F6 | Variable refresh monitor (G-Sync, FreeSync): no vsync, each vblank presented as soon as it is ready. |
| `SyncToMonitor` | `--sync-to-monitor on\|off` | F7 | Runs the PSP's vblanks at the monitor's rate when it (or a half, third, quarter of it) is within 1% of 59.94. |
| `PauseFps` | `--pause-fps 30\|60` | F4 | Pictures per second while the menu is open. |
| `FpsCap` | `--fps-cap off\|30\|60\|120` | F8 cycles | Most pictures shown per second. |

### Refresh and frame pacing

The game always runs at the PSP's 59.94 vblanks per second: a faster monitor makes motion smoother,
never the game faster. Each PSP vblank is shown from the first monitor refresh at or after its time,
with vsync, so vblanks stay up for evenly spread numbers of refreshes (on 60 Hz one each with a repeat
about every 17 s; on 144 Hz 2-3-2-2-3). When `Refresh` is set to a rate the monitor does not run at, a
timer keeps the same schedule without vsync. If a picture would be late its refresh is skipped rather
than slowing the game. Sound follows the display's clock through resampling of at most 0.5%.
`SyncToMonitor` removes the 60 Hz repeat by running vblanks at exactly 60 (0.1% faster, sound pitched
to match); it has no effect on 75, 144 or 165 Hz. With `VRR` the game is timed at 59.94 and the monitor
follows it. AnyPSP cannot ask the monitor whether it has variable refresh, so it is a setting. The
Pause key opens and closes the menu, which pauses the game; the menu is drawn at `PauseFps`.

The window title shows the monitor rate (`auto` or `set`) and pacing (`vsync`, `timer` or `VRR`), the
frames the game finished (`game`), the pictures shown (`shown`) and the PSP vblanks run per second
(59.94 is full speed), for example `144 Hz auto vsync / game 30 fps / shown 144 fps / 60 vblank/s`.

### Smoothing

The game always runs at the PSP's 59.94 vblanks per second. Many games draw only every second vblank
(30 fps), so each picture stays up for two vblanks. Smoothing shows a 50/50 blend of a new picture and
the one before on the vblank where the new one appears, then the new picture alone: motion steps at
60 per second instead of 30. It blends nothing else: games drawing at 60 fps, uneven frame times, and
scene cuts (two pictures that differ a lot, compared on small copies) are shown as they are. Blends
are mixes of two real pictures, not motion estimates, so fast movement looks doubled rather than
warped. Smoothing adds one vblank (about 17 ms) of input latency to the frames it blends. The window
title shows `smoothing` while it is on.

### Frame interpolation

`Interpolate = on` (or `--interpolate on`, and `auto` for UNH 1) gives games drawing at 30 fps a real 60 pictures per
second without touching the game: its code, timers, input and sound run exactly as before, and every
picture the game draws is shown unchanged. What is added is the picture on the vblank in between.

- Every primitive is drawn twice: once as the game sent it, and once into a second GL renderer with
  its vertices (after transform and skinning, before clipping) moved half way back to the same
  primitive in the game's previous frame. That second picture is shown on the vblank where the new
  frame appears, then the new frame itself, so motion, camera pans and animation step at 60 per second.
- Primitives are paired by vertex address (static models keep theirs) and otherwise by shape in drawing
  order. Unpaired primitives, clears, ones that jumped more than a quarter of the screen (or 96 pixels
  in 2D), and the whole picture when it differs from the new frame like a scene cut are drawn as in the
  new frame.
- Colours, texture coordinates and post-processing copies are not interpolated. It needs the GL
  renderer, draws everything twice on the GPU, and adds one vblank (about 17 ms) of latency, like
  smoothing. Interpolation wins over smoothing when both are on; the title shows `interpolated`.
- Headless runs with `--interpolate on` capture the in-between picture on those vblanks
  (`--hashes`, `--frames-dir`), and the run ends with how many primitives were paired.

Verified with Naruto: Ultimate Ninja Heroes (ULUS-10299, 30 fps everywhere) under `--deterministic`,
9000 vblanks through the intro, menus and a battle: every vblank showing a game frame is identical to a
run without interpolation (so logos, intro, title idle timer and the round timer keep their timing),
99.9% of primitives are paired, and the battle shows 2998 distinct pictures in 3000 vblanks.

## Game patches

`--patches auto` applies `patches/<disc ID>.ini` from the program's folder or the per-user data folder
(the disc ID is the first field of the disc's `UMD_DATA.BIN`, for example `ULUS-10299`); `--patches
file.ini` names a file. A patch file holds addresses and values only, one write per line:

```
# <when> <size> <address> = <value> [if <old>]
load   u32 0x08812345     = 0x00000001          # once, after the executable is loaded
vblank u8  [0x08C20C70]+1 = 1          if 2    # after every vblank, through a pointer
```

- `when` is `load` or `vblank`; `size` is `u8`, `u16` or `u32`; numbers are hex (`0x`) or decimal.
- `[0x08C20C70]+1` follows the 32-bit pointer stored at 0x08C20C70 and adds 1; the write is skipped
  while the pointer is not in user memory (0x08800000-0x09FFFFFF), so it waits until the game sets it up.
- `if <old>` writes only while the current value is `<old>`, leaving values the game sets for other
  reasons alone.
- Patches change data, not code: per-game runners run their translated code whatever the bytes in
  memory say, so `load` writes over instructions only affect `anypsp-play`.
- A mistake in the file stops the program with the file name and line. The run ends with the number
  of values the patches wrote.

No patch files ship. UNH 1 (ULUS-10299) waits for a frame interval (two vblanks in most scenes) kept
in a byte of its system object (`[0x08C20C70]+1`); setting it to 1 makes the game draw at 60 fps, but its
logic counts drawn frames, so intros, menus and the title's idle timer then run twice as fast. Its own
60 fps scenes set the interval to 1 and scale their timers by it, but most code (the logo and intro
timers among it) counts frames, so changing the interval instructions is not enough either. Use
[frame interpolation](#frame-interpolation) for 60 fps instead.

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
Interpolation = F10
Refresh = F5
VRR = F6
SyncToMonitor = F7
FpsCap = F8
PauseFps = F4
Pause = Pause
Menu = Escape, F1
FastForward = Space

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
FastForward = RightTrigger

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
Speed = 100
FastForwardSpeed = 300
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
