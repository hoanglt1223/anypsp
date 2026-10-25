// SPDX-License-Identifier: GPL-2.0-or-later
package io.anyport.anypsp;

import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Runs the translated game: libmain.so (platform/android/src/android_main.cpp) through SDL,
 * with a touch overlay for the PSP buttons that hides while a gamepad or keyboard is used.
 */
public class GameActivity extends SDLActivity {
    /** Extra host arguments, e.g. adb shell am start ... --esa args --frames,600,--dump,last.png */
    public static final String EXTRA_ARGS = "args";

    private TouchOverlay overlay;

    static native void nativeSetTouchButtons(int mask);

    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected String[] getArguments() {
        List<String> args = new ArrayList<>();
        args.add(getFilesDir().getAbsolutePath());
        String[] extra = getIntent().getStringArrayExtra(EXTRA_ARGS);
        if (extra != null) args.addAll(Arrays.asList(extra));
        return args.toArray(new String[0]);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (mLayout != null) {
            overlay = new TouchOverlay(this);
            mLayout.addView(overlay, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
    }

    private static boolean fromController(InputDevice device, int source) {
        int classes = InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK | InputDevice.SOURCE_DPAD | InputDevice.SOURCE_KEYBOARD;
        return (source & classes) != 0 && device != null && !device.isVirtual();
    }

    private void showOverlay(boolean show) {
        if (overlay == null) return;
        int visibility = show ? View.VISIBLE : View.GONE;
        if (overlay.getVisibility() != visibility) {
            overlay.setVisibility(visibility);
            if (!show) overlay.release();
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (fromController(event.getDevice(), event.getSource())) showOverlay(false);
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (fromController(event.getDevice(), event.getSource())) showOverlay(false);
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        if (overlay != null && overlay.getVisibility() != View.VISIBLE && event.getActionMasked() == MotionEvent.ACTION_DOWN) {
            showOverlay(true);
        }
        return super.dispatchTouchEvent(event);
    }
}
