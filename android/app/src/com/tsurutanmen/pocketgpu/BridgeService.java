package com.tsurutanmen.pocketgpu;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.net.wifi.WifiManager;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;

import java.util.ArrayList;
import java.util.List;

// 使う機能（ウェブカメラ・スピーカーとマイク・GPU）を持つ前面サービス。どれかがオンの間だけ動く。
// 前面サービスにする理由：画面を消したり別のアプリに切り替えたりしても、カメラ・マイク・受信アプリを止めないため。
// Android 14 からは、カメラとマイクの種類の前面サービスは、その許可があって、アプリが前面にいるときにしか始められない。
// そのため、画面のスイッチを入れたとき（アプリが前面にいるとき）に apply() を呼ぶ。
// 0.1.2 で直した（バグ探しの指摘 3）：カメラ・マイクの種類で始められず、種類なしで始め直したときは、前はカメラとマイクも動かしていた
// （カメラは開けず、マイクは無音なのに、画面は「オン」に見えた）。今は、ウェブカメラはオフにし、マイクは使わずにスピーカーだけにして、理由を画面に出す
public class BridgeService extends Service {
    static volatile BridgeService instance;
    // 画面に出す理由（0 は無し）。前面サービスの種類が足りずに、機能を止めたとき
    static volatile int problem;

    private static final String CHANNEL = "running";
    private static final int NOTIF_ID = 1;
    private static final int FG_OK = 1, FG_CAM = 2, FG_MIC = 4;

    CameraBridge camera;
    AudioBridge audio;
    GpuReceiver gpu;
    private PowerManager.WakeLock wake;
    private WifiManager.WifiLock wifiLock;
    // 0.1.5：PC とつながって GPU の仕事が来ている間だけ画面を点けたままにする（暗い明るさのまま。アプリが前にいなくても効く）
    private PowerManager.WakeLock screenLock;
    private boolean screenHeld, screenKit;
    // 0.1.6：同じ条件の間だけ、Android に性能が要ると伝える（GameState・性能のヒント）
    private PerfBoost perf;
    private long screenRenewedAt;
    private final Handler tickUi = new Handler(Looper.getMainLooper());
    private static final long SCREEN_LOCK_MS = 10_000, SCREEN_RENEW_MS = 3_000;
    // 画面を点けたままにしている間は true（画面の表示用）
    static volatile boolean keepingScreen;
    private final Runnable screenTick = new Runnable() {
        @Override public void run() {
            updateScreenLock();
            tickUi.postDelayed(this, 1000);
        }
    };

    // 画面から呼ぶ。どれかがオンならサービスを始めて（すでに動いていれば設定を読み直して）、全部オフなら止める
    static void apply(Context c) {
        boolean any = Prefs.on(c, Prefs.GPU) || Prefs.on(c, Prefs.WEBCAM) || Prefs.on(c, Prefs.AUDIO);
        Intent i = new Intent(c, BridgeService.class);
        if (any) c.startForegroundService(i);
        else if (instance != null) instance.stopAll();
    }

    @Override
    @SuppressWarnings("deprecation") // SCREEN_DIM_WAKE_LOCK：アプリが後ろにいても画面を点けたままにできるのはこの種類だけ
    public void onCreate() {
        super.onCreate();
        camera = new CameraBridge(this);
        audio = new AudioBridge(this);
        gpu = new GpuReceiver(this);
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, getString(R.string.notif_channel), NotificationManager.IMPORTANCE_LOW));
        PowerManager pm = getSystemService(PowerManager.class);
        wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "pocketgpu:bridge");
        wake.setReferenceCounted(false);
        // 画面を点けたままにする鍵。SCREEN_DIM_WAKE_LOCK は非推奨だが、アプリが後ろにいても効き、明るさは暗いまま（電池に優しい）。
        // 時間切れを付けて 3 秒ごとに延ばす：サービスが落とされても、10 秒以内に自然に戻る
        screenLock = pm.newWakeLock(PowerManager.SCREEN_DIM_WAKE_LOCK, "pocketgpu:screen");
        screenLock.setReferenceCounted(false);
        perf = new PerfBoost(this);
        instance = this;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        boolean gpuOn = Prefs.on(this, Prefs.GPU), camOn = Prefs.on(this, Prefs.WEBCAM), audioOn = Prefs.on(this, Prefs.AUDIO);
        boolean camOk = checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED;
        boolean micOk = checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
        if (!gpuOn && !camOn && !audioOn) {
            stopAll();
            return START_NOT_STICKY;
        }
        boolean camWant = camOn && camOk, micWant = audioOn && micOk;
        int fg = goForeground(gpuOn, camWant, audioOn, micWant);
        if ((fg & FG_OK) == 0) {
            stopAll();
            return START_NOT_STICKY;
        }
        int prob = 0;
        if (camWant && (fg & FG_CAM) == 0) {
            // カメラの種類で始められなかった：ウェブカメラはオフにする（カメラを開けないまま「オン」に見せない）
            Prefs.get(this).edit().putBoolean(Prefs.WEBCAM, false).apply();
            camOn = false;
            prob = R.string.prob_fgs_camera;
            Logbook.i("ウェブカメラをオフにしました。前面サービスをカメラの種類で始められなかったためです。アプリを開いて、スイッチを入れ直してください", "The webcam was turned off because the foreground service could not start with the camera type. Open the app and turn the switch on again");
        }
        boolean micBlocked = micWant && (fg & FG_MIC) == 0;
        if (micBlocked) {
            prob = prob == 0 ? R.string.prob_fgs_mic : R.string.prob_fgs_both;
            Logbook.i("マイクを使わず、スピーカーだけにします。前面サービスをマイクの種類で始められなかったためです。アプリを開いて、スイッチを入れ直してください", "Only the speaker is used, not the mic, because the foreground service could not start with the microphone type. Open the app and turn the switch on again");
        }
        problem = prob;
        if (!gpuOn && !camOn && !audioOn) {
            stopAll();
            return START_NOT_STICKY;
        }
        wake.acquire();
        boolean legacy = Prefs.on(this, Prefs.LEGACY_TCP);
        boolean wifiOn = camOn && Prefs.on(this, Prefs.WIFI);
        setWifiLock(wifiOn);
        camera.setLegacy(legacy);
        camera.setWifi(wifiOn);
        camera.setEnabled(camOn);
        camera.settingsChanged();
        audio.setMicBlocked(micBlocked);
        audio.setLegacy(legacy);
        audio.setEnabled(audioOn);
        gpu.setEnabled(gpuOn); // 受信アプリが APK に無ければ「起動できませんでした」をログに出す
        tickUi.removeCallbacks(screenTick);
        if (gpuOn) tickUi.post(screenTick);
        else releaseScreen();
        return START_NOT_STICKY; // 落とされたら起こし直さない（後ろからはカメラとマイクの前面サービスを始められないため）
    }

    // 前面サービスにする。返すのは FG_OK・FG_CAM（カメラの種類で始めた）・FG_MIC（マイクの種類で始めた）
    private int goForeground(boolean gpuOn, boolean cam, boolean audioOn, boolean mic) {
        List<String> names = new ArrayList<>();
        if (cam) names.add(getString(R.string.row_webcam));
        if (audioOn) names.add(getString(R.string.row_audio));
        if (gpuOn) names.add(getString(R.string.row_gpu));
        PendingIntent pi = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification n = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_stat)
                .setContentTitle(getString(R.string.notif_title))
                .setContentText(getString(R.string.notif_text, String.join(getString(R.string.sep), names)))
                .setContentIntent(pi)
                .setOngoing(true)
                .build();
        try {
            if (Build.VERSION.SDK_INT >= 34) {
                int types = 0;
                if (cam) types |= ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA;
                if (mic) types |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
                if (audioOn) types |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK;
                if (gpuOn) types |= ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE;
                if (types == 0) types = ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE;
                startForeground(NOTIF_ID, n, types);
            } else {
                startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MANIFEST);
            }
            return FG_OK | (cam ? FG_CAM : 0) | (mic ? FG_MIC : 0);
        } catch (RuntimeException e) {
            // 後ろからカメラ・マイクの種類で始めようとした、など。カメラとマイク抜きでもう一度（呼ぶ側がその機能を止める）
            Logbook.i("前面サービスを始められません：" + e.getMessage(), "Could not start the foreground service: " + e.getMessage());
            if (cam || mic) return goForeground(gpuOn, false, audioOn, false) & FG_OK;
            return 0;
        }
    }

    private void setWifiLock(boolean on) {
        if (on && wifiLock == null) {
            WifiManager wm = getApplicationContext().getSystemService(WifiManager.class);
            if (wm == null) return;
            wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "pocketgpu:webcam");
            wifiLock.acquire();
        } else if (!on && wifiLock != null) {
            wifiLock.release();
            wifiLock = null;
        }
    }

    // 1 秒ごと。つながっていて仕事が来ている間は鍵を持つ・延ばす。つながりが切れる・仕事が 2 分来ない・GPU をオフにしたら放す。
    // キットの受け手のときは、GpuReceiver.keepScreenKit の決まり（記録が読めれば つながっている間・読めなければ USB か充電器につながっている間）
    private void updateScreenLock() {
        boolean gpuOn = Prefs.on(this, Prefs.GPU);
        boolean app = gpuOn && gpu.keepScreen();
        boolean kit = gpuOn && !app && gpu.keepScreenKit(pluggedIn());
        boolean want = app || kit;
        long now = android.os.SystemClock.elapsedRealtime();
        if (want) {
            if (!screenHeld || kit != screenKit) {
                screenHeld = true;
                screenKit = kit;
                keepingScreen = true;
                if (kit) {
                    Logbook.i("GPU：キットの受け手が動いていて USB か充電器につながっているので、画面を点けたままにします（受け手を止める・ケーブルを抜く・GPU をオフにすると戻します）",
                            "GPU: the kit receiver is running and the phone is on USB or a charger, so the screen is kept on (it goes back when the receiver stops, the cable is unplugged, or GPU is turned off)");
                } else {
                    Logbook.i("GPU：PC とつながっているので、画面を点けたままにします（つながりが切れる・仕事が 2 分来ない・GPU をオフにすると戻します）",
                            "GPU: the PC is connected, so the screen is kept on (it goes back when the PC disconnects, no GPU work arrives for 2 minutes, or GPU is turned off)");
                }
            }
            if (!screenLock.isHeld() || now - screenRenewedAt >= SCREEN_RENEW_MS) {
                screenLock.acquire(SCREEN_LOCK_MS);
                screenRenewedAt = now;
            }
        } else if (screenHeld) {
            releaseScreen();
        }
        perf.update(want && Prefs.perfBoost(Prefs.get(this)), gpu.serverPid);
    }

    // USB か充電器につながっているか（電池の知らせは最後の値が残る）
    private boolean pluggedIn() {
        Intent b = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        return b != null && b.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0;
    }

    private void releaseScreen() {
        if (perf != null) perf.release();
        if (screenLock != null && screenLock.isHeld()) screenLock.release();
        if (screenHeld) Logbook.i("GPU：画面を点けたままにするのをやめました", "GPU: the screen is no longer kept on");
        screenHeld = false;
        screenKit = false;
        keepingScreen = false;
    }

    void stopAll() {
        tickUi.removeCallbacks(screenTick);
        releaseScreen();
        camera.setEnabled(false);
        audio.setEnabled(false);
        gpu.setEnabled(false);
        setWifiLock(false);
        if (wake.isHeld()) wake.release();
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    @Override
    public void onDestroy() {
        tickUi.removeCallbacks(screenTick);
        releaseScreen();
        camera.shutdown();
        audio.setEnabled(false);
        gpu.setEnabled(false);
        setWifiLock(false);
        if (wake.isHeld()) wake.release();
        instance = null;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
