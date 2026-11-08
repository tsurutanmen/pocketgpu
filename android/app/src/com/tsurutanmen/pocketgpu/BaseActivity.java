package com.tsurutanmen.pocketgpu;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;

// ホームと設定の画面の土台。前にいるかを数え（画面のライトと暗くする画面は、アプリが前にいるときだけ出す）、
// カメラかマイクが接続中に、操作が無いまま決めた秒数がたったら画面を暗くする（iPhone の ScreenDimmer.autoTick と同じ）
abstract class BaseActivity extends Activity {
    private final Handler dimUi = new Handler(Looper.getMainLooper());
    private long lastTouch = SystemClock.elapsedRealtime();
    private final Runnable dimTick = new Runnable() {
        @Override public void run() {
            autoDim();
            dimUi.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onResume() {
        super.onResume();
        ScreenState.enter();
        lastTouch = SystemClock.elapsedRealtime();
        dimUi.postDelayed(dimTick, 1000);
    }

    @Override
    protected void onPause() {
        dimUi.removeCallbacks(dimTick);
        ScreenState.leave();
        super.onPause();
    }

    @Override
    public void onUserInteraction() {
        super.onUserInteraction();
        lastTouch = SystemClock.elapsedRealtime();
    }

    private void autoDim() {
        BridgeService s = BridgeService.instance;
        boolean conn = s != null && (s.camera.connected || s.audio.connected);
        if (!conn) { lastTouch = SystemClock.elapsedRealtime(); return; } // つないでから数え始める
        android.content.SharedPreferences p = Prefs.get(this);
        if (!Prefs.autoDim(p) || ScreenState.light || ScreenState.dimmed) return;
        if (SystemClock.elapsedRealtime() - lastTouch >= Prefs.autoDimSecs(p) * 1000L) {
            lastTouch = SystemClock.elapsedRealtime();
            ScreenState.setDim(this, true);
        }
    }
}
