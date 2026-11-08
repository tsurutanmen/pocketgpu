package com.tsurutanmen.pocketgpu;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.TypedValue;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

// ホームの画面：状態（温度も）・使う機能のスイッチ・PC での使い方・ログ。
// adb から始めるときは：am start -n com.tsurutanmen.pocketgpu/.MainActivity --ez webcam true --ez audio true --ez gpu true
//   カメラと Wi-Fi の設定も渡せる：--ei cam_lens 1 --es cam_res 640x480 --ei cam_fps 15 --ei cam_quality 2 --ez cam_pc_priority false --ez wifi true
//   0.1.2 から：--ez legacy_tcp true（古い PC のツールも受け付ける）・マイクの設定（handleExtras の中の行）
public class MainActivity extends BaseActivity {
    private static final int REQ_PERMS = 1;

    private Switch swGpu, swCam, swAudio;
    private TextView stCamSub, stAudioSub, stGpuSub, stCam, stAudio, stGpu, stPcLink, stGpuWork, cmdUsb, wifiNote, cmdWifi, logView;
    private TextView thermalNote, pcNote, pendingNote, rotNote, problemNote, rejectNote;
    private Thermal.Badge badge;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private boolean binding; // スイッチを画面の値に合わせている間は、変更の知らせを無視する
    private long lastFrames, lastTick;
    private double camFps;

    private final Runnable refresh = new Runnable() {
        @Override public void run() {
            update();
            ui.postDelayed(this, 500);
        }
    };

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (getActionBar() != null) getActionBar().hide();
        int pad = Ui.dp(this, 16);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        root.addView(Ui.title(this, getString(R.string.app_name)));

        root.addView(Ui.header(this, getString(R.string.sec_status)));
        stCam = Ui.row(this, root, getString(R.string.row_webcam));
        stCamSub = Ui.subRow(this, root, stCam);
        stAudio = Ui.row(this, root, getString(R.string.row_audio));
        stAudioSub = Ui.subRow(this, root, stAudio);
        stGpu = Ui.row(this, root, getString(R.string.row_gpu));
        stGpuSub = Ui.subRow(this, root, stGpu);
        stPcLink = Ui.row(this, root, getString(R.string.row_pc_link)); // 0.1.5
        stGpuWork = Ui.row(this, root, getString(R.string.row_gpu_work));
        badge = new Thermal.Badge(this, false);
        Ui.rowWith(this, root, getString(R.string.row_temp), badge);
        thermalNote = addNote(root);
        pcNote = addNote(root);
        pendingNote = addNote(root);
        rotNote = addNote(root);
        problemNote = addNote(root);
        rejectNote = addNote(root);

        root.addView(Ui.header(this, getString(R.string.sec_features)));
        swGpu = addSwitch(root, R.string.sw_gpu, Prefs.GPU);
        swCam = addSwitch(root, R.string.sw_webcam, Prefs.WEBCAM);
        swAudio = addSwitch(root, R.string.sw_audio, Prefs.AUDIO);

        Button settings = new Button(this);
        settings.setText(R.string.btn_settings);
        settings.setOnClickListener(v -> startActivity(new Intent(this, SettingsActivity.class)));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = Ui.dp(this, 12);
        root.addView(settings, lp);

        root.addView(Ui.header(this, getString(R.string.sec_pc)));
        root.addView(Ui.note(this, getString(R.string.pc_usb_note)));
        cmdUsb = Ui.mono(this, 13);
        root.addView(cmdUsb);
        wifiNote = Ui.note(this, getString(R.string.pc_wifi_note));
        root.addView(wifiNote);
        cmdWifi = Ui.mono(this, 13);
        root.addView(cmdWifi);

        root.addView(Ui.header(this, getString(R.string.sec_log)));
        logView = Ui.mono(this, 11);
        root.addView(logView);

        ScrollView sv = new ScrollView(this);
        sv.addView(root);
        setContentView(sv);
        handleExtras(getIntent());
    }

    private TextView addNote(LinearLayout root) {
        TextView t = Ui.note(this, "");
        t.setVisibility(View.GONE);
        root.addView(t);
        return t;
    }

    private Switch addSwitch(LinearLayout root, int text, String key) {
        Switch s = new Switch(this);
        s.setText(text);
        s.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        s.setTextColor(Ui.themeColor(this, android.R.attr.textColorPrimary));
        s.setMinHeight(Ui.dp(this, 48));
        s.setOnCheckedChangeListener((v, on) -> { if (!binding) toggled(key, on); });
        root.addView(s);
        root.addView(Ui.divider(this));
        return s;
    }

    @Override
    protected void onNewIntent(Intent i) {
        super.onNewIntent(i);
        setIntent(i);
        handleExtras(i);
    }

    // adb やキットの台本から機能を入れる・切る（--ez gpu true など）
    private void handleExtras(Intent i) {
        if (i == null || i.getExtras() == null) return;
        boolean changed = false;
        android.content.SharedPreferences.Editor e = Prefs.get(this).edit();
        for (String k : new String[] { Prefs.GPU, Prefs.WEBCAM, Prefs.AUDIO, Prefs.PC_PRIORITY, Prefs.WIFI, Prefs.LEGACY_TCP }) {
            if (i.hasExtra(k)) { e.putBoolean(k, i.getBooleanExtra(k, false)); changed = true; }
        }
        for (String k : new String[] { Prefs.LENS, Prefs.FPS, Prefs.QUALITY }) {
            if (i.hasExtra(k)) { e.putInt(k, i.getIntExtra(k, 0)); changed = true; }
        }
        if (i.hasExtra(Prefs.RES)) { e.putString(Prefs.RES, i.getStringExtra(Prefs.RES)); changed = true; }
        // マイク（0.1.2）：--ei audioMicMode 1〜3・--es audioMicInput auto|builtIn|wired|usb|bluetooth・--ez audioMicStereo true・--ef audioMicGainDb 6
        if (i.hasExtra(Prefs.MIC_MODE)) { e.putInt(Prefs.MIC_MODE, i.getIntExtra(Prefs.MIC_MODE, 2)); changed = true; }
        if (i.hasExtra(Prefs.MIC_INPUT)) { e.putString(Prefs.MIC_INPUT, i.getStringExtra(Prefs.MIC_INPUT)); changed = true; }
        if (i.hasExtra(Prefs.MIC_STEREO)) { e.putBoolean(Prefs.MIC_STEREO, i.getBooleanExtra(Prefs.MIC_STEREO, false)); changed = true; }
        if (i.hasExtra(Prefs.MIC_GAIN)) { e.putFloat(Prefs.MIC_GAIN, Math.max(-20f, Math.min(30f, i.getFloatExtra(Prefs.MIC_GAIN, 0f)))); changed = true; }
        if (i.hasExtra(Prefs.LENS)) e.remove(Prefs.ZOOM); // カメラを替えたら、倍率はそのカメラをそのまま使う見え方に戻す
        e.apply();
        if (changed) {
            Logbook.i("外から機能の切り替えを受けました", "Received a feature change from outside");
            askPermissionsThenApply();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        bindSwitches();
        ui.post(refresh);
    }

    @Override
    protected void onPause() {
        ui.removeCallbacks(refresh);
        super.onPause();
    }

    private void bindSwitches() {
        binding = true;
        swGpu.setChecked(Prefs.on(this, Prefs.GPU));
        swCam.setChecked(Prefs.on(this, Prefs.WEBCAM));
        swAudio.setChecked(Prefs.on(this, Prefs.AUDIO));
        binding = false;
    }

    private void toggled(String key, boolean on) {
        Prefs.get(this).edit().putBoolean(key, on).apply();
        if (on) BridgeService.problem = 0; // 入れ直したら、前の理由は消す（まだ足りなければ、また出る）
        askPermissionsThenApply();
    }

    // 入れた機能に要る許可をまとめて頼み、答えが来てからサービスに伝える
    private void askPermissionsThenApply() {
        List<String> need = new ArrayList<>();
        if (Prefs.on(this, Prefs.WEBCAM) && !granted(Manifest.permission.CAMERA)) need.add(Manifest.permission.CAMERA);
        if (Prefs.on(this, Prefs.AUDIO) && !granted(Manifest.permission.RECORD_AUDIO)) need.add(Manifest.permission.RECORD_AUDIO);
        boolean any = Prefs.on(this, Prefs.GPU) || Prefs.on(this, Prefs.WEBCAM) || Prefs.on(this, Prefs.AUDIO);
        if (any && Build.VERSION.SDK_INT >= 33 && !granted(Manifest.permission.POST_NOTIFICATIONS)
                && !Prefs.get(this).getBoolean("asked_notif", false)) {
            need.add(Manifest.permission.POST_NOTIFICATIONS);
            Prefs.get(this).edit().putBoolean("asked_notif", true).apply();
        }
        if (need.isEmpty()) applyNow();
        else requestPermissions(need.toArray(new String[0]), REQ_PERMS);
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        if (code != REQ_PERMS) return;
        if (Prefs.on(this, Prefs.WEBCAM) && !granted(Manifest.permission.CAMERA)) {
            Prefs.get(this).edit().putBoolean(Prefs.WEBCAM, false).apply();
            Toast.makeText(this, R.string.perm_camera, Toast.LENGTH_LONG).show();
        }
        if (Prefs.on(this, Prefs.AUDIO) && !granted(Manifest.permission.RECORD_AUDIO)) {
            Toast.makeText(this, R.string.perm_mic, Toast.LENGTH_LONG).show();
        }
        bindSwitches();
        applyNow();
    }

    private void applyNow() {
        BridgeService.apply(this);
        boolean any = Prefs.on(this, Prefs.GPU) || Prefs.on(this, Prefs.WEBCAM) || Prefs.on(this, Prefs.AUDIO);
        // 使う機能がオンで、この画面が前面にある間は、画面を消さない
        if (any) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    private boolean granted(String p) {
        return checkSelfPermission(p) == PackageManager.PERMISSION_GRANTED;
    }

    private static void show(TextView t, String s) {
        t.setVisibility(s == null ? View.GONE : View.VISIBLE);
        if (s != null) t.setText(s);
    }

    // ---- 表示 ----

    private void update() {
        BridgeService s = BridgeService.instance;
        boolean any = Prefs.on(this, Prefs.GPU) || Prefs.on(this, Prefs.WEBCAM) || Prefs.on(this, Prefs.AUDIO);
        if (any) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (swCam.isChecked() != Prefs.on(this, Prefs.WEBCAM)) bindSwitches(); // サービスが止めた（前面サービスの種類が足りない）

        // ウェブカメラ。値は「接続中」、下の小さな行に大きさ・カメラ・毎秒のコマ数
        boolean camLive = Prefs.on(this, Prefs.WEBCAM) && s != null && s.camera.connected && !s.camera.cameraStopped;
        stCamSub.setVisibility(camLive ? View.VISIBLE : View.GONE);
        if (!Prefs.on(this, Prefs.WEBCAM) || s == null) stCam.setText(R.string.st_off);
        else if (!s.camera.connected) stCam.setText(R.string.st_waiting);
        else if (s.camera.cameraStopped) stCam.setText(R.string.st_cam_stopped);
        else {
            long now = SystemClock.elapsedRealtime();
            long f = s.camera.sentFrames;
            if (lastTick != 0 && now - lastTick >= 900) {
                camFps = (f - lastFrames) * 1000.0 / (now - lastTick);
                lastFrames = f;
                lastTick = now;
            } else if (lastTick == 0) {
                lastFrames = f;
                lastTick = now;
            }
            stCam.setText(R.string.st_connected);
            int lens = s.camera.curCamera;
            String name = getString(lens == 1 ? R.string.lens_front : lens == 2 ? R.string.lens_ultrawide : lens == 3 ? R.string.lens_tele : R.string.lens_back);
            stCamSub.setText(s.camera.curW + "×" + s.camera.curH + getString(R.string.sep) + name + getString(R.string.sep) + getString(R.string.st_fps, String.format(Locale.US, "%.1f", Math.max(0, camFps))));
        }

        // スピーカーとマイク。値は「接続中」、下の小さな行に使っている物とバッファ
        boolean audioLive = Prefs.on(this, Prefs.AUDIO) && s != null && s.audio.connected;
        stAudioSub.setVisibility(audioLive ? View.VISIBLE : View.GONE);
        if (!Prefs.on(this, Prefs.AUDIO) || s == null) stAudio.setText(R.string.st_off);
        else if (!s.audio.connected) stAudio.setText(R.string.st_waiting);
        else {
            stAudio.setText(R.string.st_connected);
            List<String> parts = new ArrayList<>();
            if (s.audio.speakerActive) parts.add(getString(R.string.st_spk));
            if (s.audio.micActive) {
                parts.add(getString(R.string.st_mic));
                if (!s.audio.micDetail.isEmpty()) parts.add(s.audio.micDetail);
            } else if (s.audio.micRetrying) {
                parts.add(getString(R.string.st_mic_retry));
            }
            if (s.audio.speakerActive) parts.add(getString(R.string.st_buffer, (int) s.audio.fillMs));
            stAudioSub.setText(String.join(getString(R.string.sep), parts));
        }

        // GPU。キットの受け手（start_android.cmd）が動いていれば、そちらを使う。終わったときは理由を下の行に
        String gpuState = s == null ? "off" : s.gpu.state;
        boolean gpuOn = Prefs.on(this, Prefs.GPU) && s != null;
        if (s != null && !s.gpu.available() && !"kit".equals(gpuState)) stGpu.setText(R.string.st_gpu_none);
        else if (!gpuOn) stGpu.setText(R.string.st_off);
        else {
            switch (gpuState) {
                case "running": stGpu.setText(R.string.st_gpu_running); break;
                case "starting": stGpu.setText(R.string.st_gpu_starting); break;
                case "failed": stGpu.setText(R.string.st_gpu_failed); break;
                case "kit": stGpu.setText(R.string.st_gpu_kit); break;
                default: stGpu.setText(R.string.st_off); break;
            }
        }
        String gpuSub = !gpuOn ? null : "failed".equals(gpuState) && !s.gpu.reason.isEmpty() ? s.gpu.reason : "kit".equals(gpuState) ? getString(R.string.st_gpu_kit_sub)
                : "running".equals(gpuState) && !s.gpu.portNote.isEmpty() ? s.gpu.portNote : null; // 0.1.4：50060 を使えなかった理由と、使った番号
        stGpuSub.setVisibility(gpuSub == null ? View.GONE : View.VISIBLE);
        if (gpuSub != null) stGpuSub.setText(gpuSub);

        // 0.1.5：PC とつながっているか・この数秒で GPU の仕事が来ているか（アプリの受け手のときだけ分かる）
        boolean gpuLive = gpuOn && "running".equals(gpuState);
        if (gpuLive && s.gpu.pcConnected) {
            stPcLink.setText(R.string.st_connected);
            stGpuWork.setText(s.gpu.working() ? R.string.st_gpu_busy : R.string.st_gpu_idle);
        } else if (gpuOn && "kit".equals(gpuState)) {
            // キットの受け手：記録を読めるときだけ、つながっているかが分かる。仕事の有無は分からない
            stPcLink.setText(!s.gpu.kitLogOk ? R.string.st_unknown : s.gpu.kitConnected ? R.string.st_connected : R.string.st_waiting);
            stGpuWork.setText(R.string.st_dash);
        } else if (gpuLive) {
            stPcLink.setText(R.string.st_waiting);
            stGpuWork.setText(R.string.st_dash);
        } else {
            stPcLink.setText(R.string.st_dash);
            stGpuWork.setText(R.string.st_dash);
        }

        // 温度（信号の 3 色）と、熱でコマ数を下げているときの文
        badge.set(Thermal.now(this));
        boolean conn = s != null && s.camera.connected;
        show(thermalNote, conn && s.camera.lowFps > 0 ? getString(R.string.thermal_note, s.camera.lowFps) : null);
        show(pcNote, conn && s.camera.pcControl ? getString(R.string.note_pc_control) : null);
        show(pendingNote, conn && s.camera.pendingNote ? getString(R.string.note_pending) : null);
        show(rotNote, conn && s.camera.rotSkipped ? getString(R.string.note_rot_skipped) : null);
        show(problemNote, BridgeService.problem != 0 ? getString(BridgeService.problem) : null);
        // 断った接続（1 分の間だけ出す）
        long now = System.currentTimeMillis();
        String rej = null;
        if (s != null) {
            long foreign = Math.max(s.camera.foreignRejectedAt(), s.audio.foreignRejectedAt());
            long legacy = Math.max(s.camera.legacyRejectedAt(), s.audio.legacyRejectedAt());
            if (now - legacy < 60_000) rej = getString(R.string.warn_legacy);
            else if (now - foreign < 60_000) rej = getString(R.string.warn_foreign);
        }
        show(rejectNote, rej);

        // PC で打つコマンド
        StringBuilder c = new StringBuilder();
        c.append("adb forward tcp:50064 localabstract:").append(CameraBridge.USB_NAME).append("\nwebcam_rx.exe --addr 127.0.0.1:50064\n\n");
        c.append("adb forward tcp:50063 localabstract:").append(AudioBridge.USB_NAME).append("\nipaudio.exe --connect 127.0.0.1:50063 --speaker --mic\n\n");
        if (gpuOn && "kit".equals(gpuState)) c.append("android\\start_android.cmd\npocketgpu_android.cmd app.exe"); // キットの受け手を使うとき
        else c.append("adb forward tcp:50300 ").append(s != null ? s.gpu.forwardTarget() : "localabstract:" + GpuReceiver.USB_NAME).append("\npocketgpu_android.cmd app.exe"); // 0.1.4：USB の抽象名か、使えた TCP の番号
        cmdUsb.setText(c);
        String ip = Ui.wifiAddress(this);
        boolean wifi = Prefs.on(this, Prefs.WEBCAM) && Prefs.on(this, Prefs.WIFI) && s != null && ip != null && !s.camera.wifiBlocked;
        wifiNote.setVisibility(wifi ? View.VISIBLE : View.GONE);
        cmdWifi.setVisibility(wifi ? View.VISIBLE : View.GONE);
        if (wifi) cmdWifi.setText("webcam_rx.exe --addr " + ip + ":" + CameraBridge.PORT + " --code " + s.camera.wifiCode);

        logView.setText(Logbook.tail(40));
    }
}
