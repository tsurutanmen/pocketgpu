package com.tsurutanmen.pocketgpu;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

// 画面を暗くする（熱と電池の対策。iPhone の ScreenDimOverlay と同じ）。画面を黒で覆って明るさを最小にする。カメラとマイクは続く。
// 小さく暗い色で温度と「接続中です。タップすると戻ります。」を出し、焼き付きを防ぐため 20 秒ごとに場所を動かす。
// タップするか、アプリが前からいなくなると戻す
public class DimActivity extends Activity {
    private static final float[][] SPOTS = { { 0.3f, 0.08f }, { 0.7f, 0.92f }, { 0.7f, 0.1f }, { 0.3f, 0.9f },
            { 0.5f, 0.12f }, { 0.5f, 0.88f }, { 0.35f, 0.5f }, { 0.65f, 0.5f } };
    private final Handler ui = new Handler(Looper.getMainLooper());
    private FrameLayout root;
    private LinearLayout box;
    private Thermal.Badge badge;
    private TextView text;
    private int spot, ticks;
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            update();
            if (++ticks % 20 == 0) {
                spot = (spot + 1) % SPOTS.length;
                place();
            }
            ui.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.screenBrightness = 0f; // いちばん暗く
        getWindow().setAttributes(lp);
        getWindow().setNavigationBarColor(0xFF000000);
        getWindow().setStatusBarColor(0xFF000000);
        root = new FrameLayout(this);
        root.setBackgroundColor(0xFF000000);
        box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER_HORIZONTAL);
        badge = new Thermal.Badge(this, true);
        box.addView(badge, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        text = new TextView(this);
        text.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        text.setTextColor(0xFF474747);
        LinearLayout.LayoutParams tl = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        tl.topMargin = Ui.dp(this, 6);
        box.addView(text, tl);
        root.addView(box, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT));
        root.setOnClickListener(v -> finish());
        root.addOnLayoutChangeListener((v, l, t, r, bt, ol, ot, or, ob) -> place());
        setContentView(root);
        update();
    }

    private void update() {
        badge.set(Thermal.now(this));
        BridgeService s = BridgeService.instance;
        boolean conn = s != null && (s.camera.connected || s.audio.connected);
        text.setText((conn ? getString(R.string.dim_connected) : "") + getString(R.string.dim_tap));
    }

    private void place() {
        int w = root.getWidth(), h = root.getHeight();
        if (w == 0 || box.getWidth() == 0) return;
        box.setX(Math.max(0, Math.min(w - box.getWidth(), w * SPOTS[spot][0] - box.getWidth() / 2f)));
        box.setY(Math.max(0, Math.min(h - box.getHeight(), h * SPOTS[spot][1] - box.getHeight() / 2f)));
    }

    @Override
    protected void onResume() {
        super.onResume();
        ScreenState.enter();
        ScreenState.setDimShown(this, true);
        ui.post(tick);
    }

    @Override
    protected void onPause() {
        ui.removeCallbacks(tick);
        ScreenState.leave();
        super.onPause();
        finish(); // アプリが前からいなくなったら戻す（iPhone と同じ）
    }

    @Override
    protected void onDestroy() {
        ScreenState.setDimShown(this, false);
        super.onDestroy();
    }
}
