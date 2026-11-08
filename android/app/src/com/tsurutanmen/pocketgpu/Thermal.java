package com.tsurutanmen.pocketgpu;

import android.content.Context;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

// 温度。Android の熱の状態（PowerManager の THERMAL_STATUS_* 0〜6）を、iPhone と同じ 4 段階（0 正常・1 やや高い・2 高い・3 危険）にする。
// 約束の EVENT 3・CTRL の 102 も、この 4 段階で送る（PC のアプリは iPhone と同じ 3 色で出す）。
//   NONE → 正常・LIGHT → やや高い・MODERATE と SEVERE → 高い・CRITICAL から上 → 危険
//   MODERATE を「高い」にしたのは、Android はこの段で処理を絞り始めるため（熱でコマ数を下げるのもここから）。本物の機種で見直す
// 色は信号の 3 色：正常は緑・やや高いは黄・高いと危険は赤。危険は文字を太くして点を点滅させる（iPhone の ThermalBadge と同じ）
final class Thermal {
    static final int GREEN = 0xFF34C759, YELLOW = 0xFFFFCC00, RED = 0xFFFF3B30;

    private Thermal() {}

    static int level(int status) {
        switch (status) {
            case PowerManager.THERMAL_STATUS_NONE: return 0;
            case PowerManager.THERMAL_STATUS_LIGHT: return 1;
            case PowerManager.THERMAL_STATUS_MODERATE:
            case PowerManager.THERMAL_STATUS_SEVERE: return 2;
            default: return status < 0 ? 0 : 3;
        }
    }

    static int raw(Context c) {
        PowerManager pm = c.getSystemService(PowerManager.class);
        return pm == null ? 0 : pm.getCurrentThermalStatus();
    }

    static int now(Context c) {
        return level(raw(c));
    }

    static int color(int level) {
        return level <= 0 ? GREEN : level == 1 ? YELLOW : RED;
    }

    static String text(Context c, int level) {
        switch (level) {
            case 0: return c.getString(R.string.thermal_0);
            case 1: return c.getString(R.string.thermal_1);
            case 2: return c.getString(R.string.thermal_2);
            default: return c.getString(R.string.thermal_3);
        }
    }

    // 温度の点と文字。set で描き直す。危険のときは点が点滅する。dim は暗くした画面に出すとき（暗い色にする）
    static final class Badge extends LinearLayout {
        private final View dot;
        private final TextView label;
        private final boolean dim;
        private final Handler ui = new Handler(Looper.getMainLooper());
        private int level = -1;
        private boolean blinkOff;
        private final Runnable blink = new Runnable() {
            @Override public void run() {
                blinkOff = !blinkOff;
                paint();
                if (level >= 3) ui.postDelayed(this, 600);
            }
        };

        Badge(Context c, boolean dim) {
            super(c);
            this.dim = dim;
            setOrientation(HORIZONTAL);
            setGravity(Gravity.CENTER_VERTICAL);
            dot = new View(c);
            int d = Ui.dp(c, 8);
            LayoutParams lp = new LayoutParams(d, d);
            lp.rightMargin = Ui.dp(c, 6);
            addView(dot, lp);
            label = new TextView(c);
            label.setTextSize(TypedValue.COMPLEX_UNIT_SP, dim ? 12 : 16);
            addView(label);
        }

        void set(int lv) {
            if (lv == level) return;
            level = lv;
            ui.removeCallbacks(blink);
            blinkOff = false;
            if (lv >= 3) ui.postDelayed(blink, 600);
            paint();
        }

        private void paint() {
            GradientDrawable g = new GradientDrawable();
            g.setShape(GradientDrawable.OVAL);
            g.setColor(color(level));
            dot.setBackground(g);
            dot.setAlpha(level >= 3 && blinkOff ? 0.1f : dim ? 0.5f : 1f);
            label.setText(text(getContext(), level));
            label.setTypeface(level >= 3 ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
            if (level >= 3) label.setTextColor(dim ? 0x99FF3B30 : RED);
            else label.setTextColor(dim ? 0xFF474747 : Ui.themeColor(getContext(), android.R.attr.textColorSecondary));
        }

        @Override protected void onDetachedFromWindow() {
            ui.removeCallbacks(blink);
            super.onDetachedFromWindow();
        }
    }
}
