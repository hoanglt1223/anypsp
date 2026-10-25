// SPDX-License-Identifier: GPL-2.0-or-later
package io.anyport.anypsp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

/**
 * PSP buttons drawn over the game in landscape: an 8-way d-pad on the left, the four face
 * buttons on the right, L and R in the top corners, SELECT and START at the bottom. Every touch
 * event recomputes the held buttons from all fingers and hands the sceCtrl mask to the host.
 */
public class TouchOverlay extends View {
    // sceCtrl button bits.
    static final int SELECT = 0x0001, START = 0x0008, UP = 0x0010, RIGHT = 0x0020, DOWN = 0x0040, LEFT = 0x0080;
    static final int L = 0x0100, R = 0x0200, TRIANGLE = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000, SQUARE = 0x8000;

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint pressed = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
    private int held;

    private static final class Round {
        final int mask;
        final String text;
        float x, y, r;
        Round(int mask, String text) { this.mask = mask; this.text = text; }
    }
    private final Round[] faces = {new Round(TRIANGLE, "△"), new Round(CIRCLE, "○"), new Round(CROSS, "✕"),
                                   new Round(SQUARE, "□")};
    private final Round[] small = {new Round(L, "L"), new Round(R, "R"), new Round(SELECT, "SELECT"), new Round(START, "START")};
    private float padX, padY, padR;

    public TouchOverlay(Context context) {
        super(context);
        fill.setColor(0x40FFFFFF);
        pressed.setColor(0x90FFFFFF);
        label.setColor(0xC0FFFFFF);
        label.setTextAlign(Paint.Align.CENTER);
        setHapticFeedbackEnabled(true);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        float unit = Math.min(w, h) / 10f;
        padR = unit * 1.9f;
        padX = unit * 0.6f + padR;
        padY = h - unit * 0.6f - padR;
        float fx = w - unit * 0.6f - padR, fy = padY, step = unit * 1.15f, br = unit * 0.62f;
        float[][] at = {{fx, fy - step}, {fx + step, fy}, {fx, fy + step}, {fx - step, fy}};
        for (int i = 0; i < 4; ++i) {
            faces[i].x = at[i][0];
            faces[i].y = at[i][1];
            faces[i].r = br;
        }
        small[0].x = unit * 1.4f; small[0].y = unit * 0.9f; small[0].r = unit * 0.8f;
        small[1].x = w - unit * 1.4f; small[1].y = unit * 0.9f; small[1].r = unit * 0.8f;
        small[2].x = w / 2f - unit * 1.3f; small[2].y = h - unit * 0.7f; small[2].r = unit * 0.55f;
        small[3].x = w / 2f + unit * 1.3f; small[3].y = h - unit * 0.7f; small[3].r = unit * 0.55f;
        label.setTextSize(unit * 0.55f);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        canvas.drawCircle(padX, padY, padR, fill);
        float arm = padR * 0.35f;
        RectF horizontal = new RectF(padX - padR * 0.85f, padY - arm, padX + padR * 0.85f, padY + arm);
        RectF vertical = new RectF(padX - arm, padY - padR * 0.85f, padX + arm, padY + padR * 0.85f);
        canvas.drawRoundRect(horizontal, arm * 0.4f, arm * 0.4f, (held & (LEFT | RIGHT)) != 0 ? pressed : fill);
        canvas.drawRoundRect(vertical, arm * 0.4f, arm * 0.4f, (held & (UP | DOWN)) != 0 ? pressed : fill);
        for (Round b : faces) drawRound(canvas, b);
        for (Round b : small) drawRound(canvas, b);
    }

    private void drawRound(Canvas canvas, Round b) {
        canvas.drawCircle(b.x, b.y, b.r, (held & b.mask) != 0 ? pressed : fill);
        canvas.drawText(b.text, b.x, b.y - (label.descent() + label.ascent()) / 2f, label);
    }

    private int buttonsAt(float x, float y) {
        float dx = x - padX, dy = y - padY;
        double distance = Math.hypot(dx, dy);
        if (distance <= padR * 1.25f && distance >= padR * 0.15f) {
            // Eight sectors of 45 degrees, centred on the directions.
            double angle = Math.toDegrees(Math.atan2(-dy, dx));
            if (angle < 0) angle += 360;
            int sector = (int) Math.floor((angle + 22.5) / 45.0) % 8;
            final int[] directions = {RIGHT, RIGHT | UP, UP, UP | LEFT, LEFT, LEFT | DOWN, DOWN, DOWN | RIGHT};
            return directions[sector];
        }
        for (Round b : faces) if (Math.hypot(x - b.x, y - b.y) <= b.r * 1.2f) return b.mask;
        for (Round b : small) if (Math.hypot(x - b.x, y - b.y) <= b.r * 1.3f) return b.mask;
        return 0;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        int mask = 0;
        if (action != MotionEvent.ACTION_UP && action != MotionEvent.ACTION_CANCEL) {
            int lifted = action == MotionEvent.ACTION_POINTER_UP ? event.getActionIndex() : -1;
            for (int i = 0; i < event.getPointerCount(); ++i)
                if (i != lifted) mask |= buttonsAt(event.getX(i), event.getY(i));
        }
        set(mask);
        return true;
    }

    /** Lets go of every button (the overlay was hidden). */
    void release() { set(0); }

    private void set(int mask) {
        if (mask == held) return;
        if ((mask & ~held) != 0) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        held = mask;
        GameActivity.nativeSetTouchButtons(mask);
        invalidate();
    }
}
