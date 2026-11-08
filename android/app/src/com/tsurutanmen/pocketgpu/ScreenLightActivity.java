package com.tsurutanmen.pocketgpu;

import android.app.Activity;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

// 画面のライト：画面を白くして明るさを最大にし、前から顔を照らす（iPhone の ScreenLightView と同じ）。
// タップするか、アプリが前からいなくなると閉じる。明るさはこの画面の間だけ変わり、閉じれば元に戻る。
// 暖かい色は、出している間に PC から変えても変わるように、設定の変化を見ている
public class ScreenLightActivity extends Activity {
    private FrameLayout root;
    private final SharedPreferences.OnSharedPreferenceChangeListener prefListener = (p, k) -> {
        if (Prefs.SCREEN_WARM.equals(k)) paint();
    };

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.screenBrightness = 1f;
        getWindow().setAttributes(lp);
        root = new FrameLayout(this);
        TextView t = new TextView(this);
        t.setText(R.string.light_tap);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        t.setTextColor(0xFF999999);
        root.addView(t, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        root.setOnClickListener(v -> finish());
        setContentView(root);
        paint();
        Prefs.get(this).registerOnSharedPreferenceChangeListener(prefListener);
    }

    private void paint() {
        boolean warm = Prefs.b(Prefs.get(this), Prefs.SCREEN_WARM, false);
        int c = warm ? 0xFFFFDEB3 : 0xFFFFFFFF;
        root.setBackgroundColor(c);
        getWindow().setNavigationBarColor(c);
        getWindow().setStatusBarColor(c);
    }

    @Override
    protected void onResume() {
        super.onResume();
        ScreenState.enter();
        ScreenState.setLightShown(this, true);
    }

    @Override
    protected void onPause() {
        ScreenState.leave();
        super.onPause();
        finish(); // アプリが前からいなくなったら閉じる（iPhone と同じ）
    }

    @Override
    protected void onDestroy() {
        Prefs.get(this).unregisterOnSharedPreferenceChangeListener(prefListener);
        ScreenState.setLightShown(this, false);
        super.onDestroy();
    }
}
