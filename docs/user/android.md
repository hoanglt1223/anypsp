# Android

`anypsp build --target android` writes a signed APK of one game:

```sh
anypsp build "Game.iso" --target android -o apk                  # arm64-v8a phones and handhelds
anypsp build EBOOT.PBP --target android --abi arm64-v8a,x86_64   # also for x86_64 emulators
```

It cross-compiles the translated code, the runtime and the Android host (`platform/android/`) with the NDK
into `libmain.so`, and assembles the APK straight from the SDK build tools (aapt2, javac, d8, zipalign,
apksigner) without Gradle, as AnyGBA does. Requirements: JDK 17, the Android SDK with `platforms;android-35`,
`build-tools;35.0.0` and an NDK (27 is tested). AnyPSP finds the SDK through `--android-sdk`, `ANDROID_HOME`,
`ANDROID_SDK_ROOT`, `~/Android/Sdk` or `~/android-sdk`, and the NDK through `--android-ndk`,
`ANDROID_NDK_HOME` or the newest `<sdk>/ndk/*`. Without them, install the command-line tools and run
`sdkmanager "platforms;android-35" "build-tools;35.0.0" "ndk;27.0.12077973"`.

## Game data

- **Discs are not put in the APK** unless you ask (`--embed-game`). On first start the app asks for your copy:
  an **ISO image** or the **extracted disc folder** (the one holding `PSP_GAME`), chosen through Android's file
  picker (storage access framework). It is copied into the app's private storage once; an ISO is extracted
  there on the next start and then deleted. The app checks that the disc's executable is the one the APK was
  built from and asks again if not.
- **Homebrew** (a lone ELF, PRX or EBOOT.PBP) is always embedded.
- Saves live in the app's private storage (`files/memstick/PSP/SAVEDATA`); system fonts can be copied to
  `files/flash0/font` (else the bundled fallback is used).

## Playing

- Landscape, full screen. **Touch controls** are drawn over the game: an 8-way d-pad on the left, Triangle,
  Circle, Cross and Square on the right, L and R in the top corners, SELECT and START at the bottom, with
  haptic feedback. Several fingers work at once.
- **Gamepads** (USB or Bluetooth) map as on the PSP: d-pad, the south button is Cross, east Circle, west
  Square, north Triangle, shoulders or triggers L/R, Start and Back/Select; the left stick is the analog
  stick. **Keyboards**: arrows, Z (Cross), X (Circle), A (Square), S (Triangle), Q/W (L/R), Enter (START),
  Right Shift (SELECT). The touch controls hide while a gamepad or keyboard is used and come back on the
  next touch.
- The game runs at the PSP's 59.94 vblanks per second whatever the display's refresh rate, with 44.1 kHz
  stereo sound (dropped rather than delayed when the device falls behind).
- Renderer: the GE's OpenGL ES 3 backend at 2x the PSP resolution, letterboxed. The Vulkan backend is the
  long-term target for Android; it is developed separately and not wired into the app yet.
- Not on Android yet: the desktop's in-game menu, cheats, patches and the system dialogs' own screens
  (dialogs are answered automatically, as in headless desktop runs).

## Signing

The debug key (`~/.android/debug.keystore`, created when missing) signs by default, and such APKs are
debuggable. `--keystore FILE --keystore-pass-env VAR [--key-alias NAME]` signs with your own key. The
package id is `io.anyport.anypsp.g<first 12 hex digits of the executable's SHA-256>` (`--package-id` to
change it); `--version-code` defaults to minutes since 2024 so a rebuild installs over the last one.

## Tests

`sh platform/android/emulator-smoke.sh app.apk [frames]` installs the APK on the running emulator or device,
runs the game for that many vblanks through the app (`--esa args --frames,N,--dump,smoke.png`), checks the
log and copies the last frame to `smoke.png`. CI does this on an API 35 x86_64 emulator.
