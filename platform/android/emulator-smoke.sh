#!/bin/sh
# Installs an APK made by `anypsp build --target android` on the running emulator or device, runs
# the game headless-style for N vblanks (GameActivity EXTRA_ARGS: --frames N --dump smoke.png),
# and checks that it reached the frame limit. Writes smoke.png (the last frame) and smoke.log here.
#
#   sh platform/android/emulator-smoke.sh app.apk [frames] [timeout seconds]
#
# Needs adb and aapt2 (ANDROID_HOME/build-tools) and a debug-signed APK (run-as reads the frame).
set -eu
apk=$1
frames=${2:-300}
timeout=${3:-300}
sdk=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}
aapt2=$(ls -d "$sdk"/build-tools/*/aapt2 | sort -V | tail -n 1)
pkg=$("$aapt2" dump packagename "$apk")
echo "smoke: $pkg, $frames frames"

adb wait-for-device
adb install -r "$apk" > /dev/null
adb shell pm clear "$pkg" > /dev/null
adb logcat -c
adb shell am start -W -n "$pkg/io.anyport.anypsp.SetupActivity" --esa args "--frames,$frames,--dump,smoke.png" > /dev/null

waited=0
while :; do
    adb logcat -d -s anypsp:V > smoke.log || true
    if grep -q "\] ran [0-9]* frames" smoke.log; then break; fi
    if grep -q "\[anypsp\] fatal" smoke.log; then cat smoke.log; echo "smoke: the game failed"; exit 1; fi
    if [ "$waited" -ge "$timeout" ]; then cat smoke.log; echo "smoke: no result after ${timeout}s"; exit 1; fi
    sleep 5
    waited=$((waited + 5))
done
cat smoke.log
grep -q "frame limit reached" smoke.log || { echo "smoke: the guest stopped early"; exit 1; }
sleep 2
adb exec-out run-as "$pkg" cat files/smoke.png > smoke.png
[ -s smoke.png ] || { echo "smoke: no frame written"; exit 1; }
echo "smoke: ok, last frame in smoke.png"
