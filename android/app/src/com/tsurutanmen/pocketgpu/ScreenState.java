package com.tsurutanmen.pocketgpu;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;

import java.lang.ref.WeakReference;
import java.util.concurrent.CopyOnWriteArrayList;

// 画面のライトと、画面を暗くする（iPhone の ScreenDimmer の役目）。画面の糸から使う。
// どちらもアプリが前にいるときだけ出す（Android は後ろのアプリが画面を出すのを許さないため。iPhone と同じ決まり）。
// 出ているかは CTRL の 26（画面のライト）と 29（画面を暗くする）で PC に知らせる。設定には残さない（画面の状態）
final class ScreenState {
    static volatile int foreground;          // 前にいるこのアプリの画面の数
    static volatile boolean light, dimmed;
    private static final CopyOnWriteArrayList<Runnable> listeners = new CopyOnWriteArrayList<>();
    private static WeakReference<Activity> lightAct = new WeakReference<>(null), dimAct = new WeakReference<>(null);
    private static final Handler ui = new Handler(Looper.getMainLooper());

    private ScreenState() {}

    static void addListener(Runnable r) { listeners.add(r); }

    static void enter() { foreground++; }
    static void leave() { foreground = Math.max(0, foreground - 1); }

    static void setLightShown(Activity a, boolean on) {
        lightAct = new WeakReference<>(on ? a : null);
        if (light == on) return;
        light = on;
        for (Runnable r : listeners) r.run();
    }

    static void setDimShown(Activity a, boolean on) {
        dimAct = new WeakReference<>(on ? a : null);
        if (dimmed == on) return;
        dimmed = on;
        for (Runnable r : listeners) r.run();
    }

    // 画面のライトを出す・閉じる（どの糸から呼んでもよい）。出すのはアプリが前にいるときだけ
    static void setLight(Context c, boolean on) {
        final Context app = c.getApplicationContext();
        ui.post(() -> {
            if (!on) {
                Activity a = lightAct.get();
                if (a != null) a.finish();
                return;
            }
            if (light || foreground <= 0) return;
            Activity d = dimAct.get();
            if (d != null) d.finish();
            app.startActivity(new Intent(app, ScreenLightActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        });
    }

    // 画面を暗くする・戻す（どの糸から呼んでもよい）。画面のライトを出しているときは、閉じて明るさが戻ってから暗くする
    static void setDim(Context c, boolean on) {
        final Context app = c.getApplicationContext();
        ui.post(() -> {
            if (!on) {
                Activity a = dimAct.get();
                if (a != null) a.finish();
                return;
            }
            if (dimmed || foreground <= 0) return;
            Activity l = lightAct.get();
            Runnable go = () -> {
                if (dimmed || foreground <= 0) return;
                app.startActivity(new Intent(app, DimActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            };
            if (l != null) {
                l.finish();
                ui.postDelayed(go, 600);
            } else {
                go.run();
            }
        });
    }
}
