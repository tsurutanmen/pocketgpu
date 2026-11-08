package com.tsurutanmen.pocketgpu;

import android.annotation.TargetApi;
import android.app.GameManager;
import android.app.GameState;
import android.content.Context;
import android.os.Build;
import android.os.PerformanceHintManager;
import android.os.SystemClock;
import android.system.Os;
import android.system.OsConstants;

import java.io.File;
import java.util.Arrays;

// 0.1.6：GPU の仕事をしている間だけ、Android に「いま性能が要る」と伝える。BridgeService.updateScreenLock が 1 秒ごとに update を呼ぶ。
// 呼ぶ条件は、画面を点けたままにする鍵（0.1.5）と同じ。つながりが切れる・仕事が 2 分来ない・GPU をオフにする・設定でオフにすると release して元に戻す。
//
// 使うもの（どれも、使えない端末・断られた端末では黙って何もしない）：
//   ・GameManager.setGameState（Android 13＝API 33 から）：遊んでいる最中（MODE_GAMEPLAY_UNINTERRUPTIBLE）と伝え、終わったら MODE_NONE に戻す。
//     アプリがゲーム（appCategory=game）のときだけ system が受け付ける。ユーザーが「パフォーマンス」のゲームモードにしているゲームでは、
//     system が読み込み中の短い性能アップにも使う。そうでないときの効き目は端末しだいなので、効くとは言い切らない
//   ・PerformanceHintManager（Android 12＝API 31 から）：受け手（GPU の仕事をする別のプロセス）のスレッドに、性能のヒントのセッションを張る。
//     system が決めた「スレッドは自分のプロセスのものだけ」を満たさず断られる端末が多い（SecurityException など）。断られたら張らない。
//     張れたら 1 秒ごとに「受け手がほぼ CPU を使い切っている（0.8 コア分以上）」なら 目標の 1.3 倍かかった、そうでなければ目標どおり、と報告する
//     （受け手の 1 コマごとの時間はアプリから見えないので、CPU 時間の増え方で近似する）
//   Window.setSustainedPerformanceMode は使わない：ピークの周波数を下げて安定させる物なので、遅いと言われている今は逆になる
//   （それに、アプリが前面にいる窓にしか効かない）
// 電池：条件が続く間だけ。1 秒に /proc を 1 回読むのと、報告を 1 回送るだけ。
final class PerfBoost {
    private static final long TARGET_NS = 16_666_667L; // 60 コマ/秒の 1 コマ分
    private static final int MAX_TIDS = 16;

    private final Context ctx;
    private boolean active;
    private Object session;          // PerformanceHintManager.Session（API 31 未満では常に null）
    private int sessionPid = -1;
    private int failedPid = -1;      // 断られた pid（同じ pid では もう試さない）
    private long lastTicks = -1, lastAt;
    private final long hz;

    PerfBoost(Context c) {
        ctx = c.getApplicationContext();
        long h = 100;
        try { h = Math.max(1, Os.sysconf(OsConstants._SC_CLK_TCK)); } catch (RuntimeException ignored) {}
        hz = h;
    }

    // want：いま性能が要るか。pid：アプリが起動した受け手の pid（キットの受け手のときは -1）
    void update(boolean want, int pid) {
        if (!want) {
            release();
            return;
        }
        if (!active) {
            active = true;
            sendState(true);
        }
        hint(pid);
    }

    void release() {
        if (session != null) closeSession();
        if (active) {
            active = false;
            sendState(false);
            Logbook.i("GPU：性能を保つ合図をやめました", "GPU: stopped telling Android that performance is needed");
        }
        sessionPid = -1;
        failedPid = -1;
        lastTicks = -1;
    }

    private void sendState(boolean playing) {
        if (Build.VERSION.SDK_INT < 33) {
            if (playing) Logbook.i("GPU：性能を保つ合図は Android 13 から（この端末は Android " + Build.VERSION.RELEASE + "）。ゲームの種類の宣言だけです",
                    "GPU: the performance signal needs Android 13 or later (this phone has Android " + Build.VERSION.RELEASE + "). Only the game category is declared");
            return;
        }
        try {
            State.send(ctx, playing);
            if (playing) Logbook.i("GPU：Android に「遊んでいる最中」と伝えました（GameState）", "GPU: told Android the game is being played (GameState)");
        } catch (RuntimeException e) {
            Logbook.i("GPU：GameState を伝えられませんでした：" + e, "GPU: could not send GameState: " + e);
        }
    }

    private void hint(int pid) {
        if (Build.VERSION.SDK_INT < 31 || pid <= 0) {
            if (session != null) closeSession();
            return;
        }
        if (session != null && sessionPid != pid) closeSession(); // 受け手が起こし直された
        long now = SystemClock.elapsedRealtime();
        long ticks = GpuReceiver.cpuTicks(pid);
        if (session == null) {
            if (pid == failedPid || ticks < 0) return;
            int[] tids = tids(pid);
            if (tids.length == 0) return;
            try {
                session = Hint.open(ctx, tids);
            } catch (RuntimeException e) {
                session = null;
                failedPid = pid;
                Logbook.i("GPU：性能のヒントのセッションは断られました（" + e.getClass().getSimpleName() + "）。この端末では使いません",
                        "GPU: the performance hint session was refused (" + e.getClass().getSimpleName() + "). It is not used on this phone");
                return;
            }
            if (session == null) {
                failedPid = pid;
                Logbook.i("GPU：この端末は性能のヒントのセッションに対応していません", "GPU: this phone does not support performance hint sessions");
                return;
            }
            sessionPid = pid;
            lastTicks = ticks;
            lastAt = now;
            Logbook.i("GPU：受け手のスレッド " + tids.length + " 個に、性能のヒントのセッションを張りました", "GPU: opened a performance hint session on " + tids.length + " receiver threads");
            return;
        }
        if (ticks < 0 || lastTicks < 0 || now <= lastAt) {
            lastTicks = ticks;
            lastAt = now;
            return;
        }
        double cores = (ticks - lastTicks) / (double) hz / ((now - lastAt) / 1000.0); // この間に使った CPU のコア数分
        lastTicks = ticks;
        lastAt = now;
        long actual = cores >= 0.8 ? TARGET_NS + TARGET_NS * 3 / 10 : TARGET_NS;
        try {
            Hint.report(session, actual);
        } catch (RuntimeException e) {
            closeSession();
            failedPid = pid;
            Logbook.i("GPU：性能のヒントを送れなくなったので、やめました（" + e.getClass().getSimpleName() + "）", "GPU: performance hints can no longer be sent, so they are stopped (" + e.getClass().getSimpleName() + ")");
        }
    }

    private void closeSession() {
        try { Hint.close(session); } catch (RuntimeException ignored) {}
        session = null;
        sessionPid = -1;
    }

    // /proc/<pid>/task の中の番号（スレッド id）。小さい順に MAX_TIDS 個まで
    private static int[] tids(int pid) {
        String[] names = new File("/proc/" + pid + "/task").list();
        if (names == null) return new int[0];
        int[] t = new int[names.length];
        int n = 0;
        for (String s : names) {
            try { t[n++] = Integer.parseInt(s); } catch (NumberFormatException ignored) { n--; }
        }
        Arrays.sort(t, 0, n);
        return Arrays.copyOf(t, Math.min(n, MAX_TIDS));
    }

    // 古い Android でこのクラスを読み込まないよう、新しい API を呼ぶ部分は入れ子のクラスに分けてある
    @TargetApi(31)
    private static final class Hint {
        static Object open(Context c, int[] tids) {
            PerformanceHintManager m = c.getSystemService(PerformanceHintManager.class);
            return m == null ? null : m.createHintSession(tids, TARGET_NS);
        }
        static void report(Object s, long ns) { ((PerformanceHintManager.Session) s).reportActualWorkDuration(ns); }
        static void close(Object s) { ((PerformanceHintManager.Session) s).close(); }
    }

    @TargetApi(33)
    private static final class State {
        static void send(Context c, boolean playing) {
            GameManager gm = c.getSystemService(GameManager.class);
            if (gm == null) return;
            gm.setGameState(new GameState(false, playing ? GameState.MODE_GAMEPLAY_UNINTERRUPTIBLE : GameState.MODE_NONE));
        }
    }
}
