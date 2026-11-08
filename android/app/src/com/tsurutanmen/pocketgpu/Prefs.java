package com.tsurutanmen.pocketgpu;

import android.content.Context;
import android.content.SharedPreferences;

// アプリの画面の設定。使う機能のオンとオフ・カメラ・Wi-Fi。
// 0.1.2 から：カメラの操作・マイク・画面を暗くする・熱の対策・古い PC のツールの受け付け。
// カメラの操作の鍵は iPhone の CameraBridge.Key と同じ名前にした（PC から操作する番号の表 work/camctl2/ctl_ids.md の「設定の鍵」）
final class Prefs {
    static final String GPU = "gpu", WEBCAM = "webcam", AUDIO = "audio";
    static final String LENS = "cam_lens", RES = "cam_res", FPS = "cam_fps", QUALITY = "cam_quality", PC_PRIORITY = "cam_pc_priority", WIFI = "wifi";
    // 0.1.2：古い PC のツール（adb forward tcp:50064 tcp:50064 の形）の接続も受け付ける。同じ端末のほかのアプリも 127.0.0.1 につなげるので、既定は切る
    static final String LEGACY_TCP = "legacy_tcp";

    // カメラの操作（iPhone と同じ鍵）
    static final String MIRROR = "webcamMirror", FLIP_V = "webcamFlipV", ROTATION = "webcamRotation";
    static final String FOCUS = "webcamFocus", FOCUS_POS = "webcamFocusPos", FOCUS_TARGET = "webcamFocusTarget";
    static final String EXPOSURE = "webcamExposure", BIAS = "webcamBias", ISO = "webcamISO", SHUTTER = "webcamShutter";
    static final String WB = "webcamWB", WB_TEMP = "webcamWBTemp";
    static final String ZOOM = "webcamZoom";          // 見え方の倍率。無ければ、そのカメラをそのまま使う見え方
    static final String TORCH = "webcamTorch", TORCH_LEVEL = "webcamTorchLevel";
    static final String STAB = "webcamStab";
    static final String CENTER_STAGE = "webcamCenterStage"; // Android は CONTROL_AUTOFRAMING（Android 14 から・対応する機種だけ）
    static final String CROP = "webcamCrop", CROP_POS = "webcamCropPos", CROP_SIZE = "webcamCropSize", CROP_X = "webcamCropX", CROP_Y = "webcamCropY";
    static final String LEVEL = "webcamLevel";
    static final String SCREEN_WARM = "webcamScreenWarm";
    static final String THERMAL_FPS = "webcamThermalFps";

    // マイク（iPhone の MicKey と同じ名前）
    static final String MIC_MODE = "audioMicMode";      // 1 加工なし・2 標準・3 雑音とエコーを消す
    static final String MIC_INPUT = "audioMicInput";    // auto・builtIn・wired・usb・bluetooth
    static final String MIC_STEREO = "audioMicStereo";
    static final String MIC_GAIN = "audioMicGainDb";    // -20〜+30 dB

    // 画面を暗くする（iPhone の ScreenDimmer と同じ名前）
    static final String AUTO_DIM = "screenAutoDim", AUTO_DIM_SECS = "screenAutoDimSeconds";
    static final int[] DIM_SECONDS = { 15, 30, 60, 120, 300 };

    // 0.1.6：GPU の仕事をしている間、Android に性能が要ると伝える（既定はオン）
    static final String PERF_BOOST = "gpuPerfBoost";

    static final String[] RES_LIST = { "640x480", "1280x720", "1920x1080" };
    static final int[] FPS_LIST = { 15, 24, 30, 60 };

    private Prefs() {}

    static SharedPreferences get(Context c) {
        return c.getSharedPreferences("pocketgpu", Context.MODE_PRIVATE);
    }

    static boolean on(Context c, String key) {
        return get(c).getBoolean(key, false);
    }

    // カメラの設定。lens は約束の camera と同じ番号（0 背面・1 前面・2 背面の超広角・3 背面の望遠）。quality は 1 標準・2 高画質・3 最高
    static final class Cam {
        int lens, w, h, fps, quality;
        boolean pcPriority;
    }

    static Cam cam(Context c) {
        SharedPreferences p = get(c);
        Cam s = new Cam();
        s.lens = p.getInt(LENS, 0);
        String r = p.getString(RES, "1280x720");
        try {
            int x = r.indexOf('x');
            s.w = Integer.parseInt(r.substring(0, x));
            s.h = Integer.parseInt(r.substring(x + 1));
        } catch (RuntimeException e) {
            s.w = 1280;
            s.h = 720;
        }
        s.fps = p.getInt(FPS, 30);
        s.quality = Math.max(1, Math.min(3, p.getInt(QUALITY, 1)));
        s.pcPriority = p.getBoolean(PC_PRIORITY, true);
        return s;
    }

    // ---- 型の違いに強い読み方（PC から書いた値・前の版の値のどちらでも読めるように）----

    static int i(SharedPreferences p, String k, int def) {
        try { return p.getInt(k, def); } catch (ClassCastException e) {
            try { return Math.round(p.getFloat(k, def)); } catch (ClassCastException e2) { return def; }
        }
    }

    static float f(SharedPreferences p, String k, float def) {
        try { return p.getFloat(k, def); } catch (ClassCastException e) {
            try { return p.getInt(k, Math.round(def)); } catch (ClassCastException e2) { return def; }
        }
    }

    static boolean b(SharedPreferences p, String k, boolean def) {
        try { return p.getBoolean(k, def); } catch (ClassCastException e) { return def; }
    }

    // カメラの操作の初めの値（iPhone の register(defaults:) と同じ）
    static boolean thermalFps(SharedPreferences p) { return b(p, THERMAL_FPS, true); }
    static float torchLevel(SharedPreferences p) { return f(p, TORCH_LEVEL, 0.5f); }
    static float cropSize(SharedPreferences p) { return Math.max(0.3f, Math.min(1f, f(p, CROP_SIZE, 0.7f))); }
    static float wbTemp(SharedPreferences p) { return f(p, WB_TEMP, 5000f); }
    static float focusPos(SharedPreferences p) { return f(p, FOCUS_POS, 0.5f); }
    static float cropX(SharedPreferences p) { return f(p, CROP_X, 0.5f); }
    static float cropY(SharedPreferences p) { return f(p, CROP_Y, 0.5f); }
    static boolean perfBoost(SharedPreferences p) { return b(p, PERF_BOOST, true); }
    static boolean autoDim(SharedPreferences p) { return b(p, AUTO_DIM, true); }
    static int autoDimSecs(SharedPreferences p) { return Math.max(5, i(p, AUTO_DIM_SECS, 30)); }
    static int micMode(SharedPreferences p) { int m = i(p, MIC_MODE, 2); return m >= 1 && m <= 3 ? m : 2; }
    static float micGain(SharedPreferences p) { return Math.max(-20f, Math.min(30f, f(p, MIC_GAIN, 0f))); }
    static String micInput(SharedPreferences p) { String s = p.getString(MIC_INPUT, "auto"); return s == null ? "auto" : s; }
}
