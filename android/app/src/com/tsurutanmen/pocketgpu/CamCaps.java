package com.tsurutanmen.pocketgpu;

import android.content.Context;
import android.graphics.Rect;
import android.hardware.Sensor;
import android.hardware.SensorManager;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.params.ColorSpaceTransform;
import android.os.Build;
import android.util.Range;
import android.util.Rational;
import android.util.SizeF;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

// そのカメラでできること（iPhone の CameraCaps と同じ役目）。画面に出す操作と、PC に送る操作の一覧（CTRL の CAPS）を決める。
// できない操作は出さない。Camera2 の静的な情報（CameraCharacteristics）だけから決めるので、カメラを開かなくても読める
final class CamCaps {
    String camId;
    boolean front, back;
    int hwLevel, sensorOrientation;
    Rect active;
    // ピント
    boolean focusAuto, focusLock, focusManual, faceAf, centerAf;
    int afAuto; // 自動のときに使う AF の形
    float minFocus;
    // 明るさ
    boolean expLock, expManual;
    float biasMin, biasMax, biasStep;
    int compMin, compMax;
    int isoMin, isoMax;
    long expMinNs, expMaxNs;
    // 色
    boolean wbLock, wbTemp;
    ColorSpaceTransform cm1, cm2, cc1, cc2;
    int ill1, ill2;
    // ズーム（見え方の倍率。背面の広角を 1 とする）
    float m = 1f;               // 見え方 ÷ このカメラの倍率
    float ratioLo = 1f, ratioHi = 1f;
    boolean ratioApi;           // CONTROL_ZOOM_RATIO（Android 11 から）を使う。使えなければ SCALER_CROP_REGION
    float zoomMin = 1f, zoomMax = 1f, zoomOptical = 1f;
    float[] zoomStops = new float[0];
    // ライト
    boolean torch, torchOwn, torchLevels;
    String torchId;
    int torchMax = 1;
    // 手ぶれ補正（0 オフ・1 遅れの少ない補正＝PREVIEW_STABILIZATION・2 標準＝ON）
    int[] stabModes = { 0 };
    boolean ois;
    // そのほか
    boolean autoframing, face, level;
    int faceMode;

    boolean focusTarget() { return focusAuto && (faceAf || centerAf); }

    // シャッター速度の選択肢（分母）。1 コマの長さより短く、カメラが出せる物だけ（iPhone と同じ並び）
    int[] shutters(int fps) {
        if (!expManual) return new int[0];
        int[] all = { 30, 50, 60, 100, 120, 250, 500, 1000, 2000, 4000 };
        List<Integer> ok = new ArrayList<>();
        for (int s : all) {
            long ns = 1_000_000_000L / s;
            if (s >= fps && ns >= expMinNs && ns <= expMaxNs) ok.add(s);
        }
        int[] o = new int[ok.size()];
        for (int i = 0; i < o.length; i++) o[i] = ok.get(i);
        return o;
    }

    // ---- どのカメラを使うか（約束の camera 0 背面・1 前面・2 背面の超広角・3 背面の望遠）----
    static final class Lens {
        int status;            // 0 できた・2 そのカメラが無い
        String why = "";
        String camId;
        float ratio = 1f;      // 開いたときの倍率（超広角と望遠を 1 つの背面カメラの倍率で選んだとき）
        float m = 1f;          // 見え方 ÷ このカメラの倍率
        boolean realtimeClock;
        float baseZoom() { return ratio * m; } // そのカメラをそのまま使う見え方
    }

    // 超広角と望遠は、別のカメラとして見えればそれを、見えなければ 1 つの背面カメラの倍率（CONTROL_ZOOM_RATIO）で選ぶ
    static Lens pick(Context ctx, int which) {
        Lens r = new Lens();
        CameraManager cm = ctx.getSystemService(CameraManager.class);
        List<String> backs = new ArrayList<>(), fronts = new ArrayList<>();
        try {
            for (String id : cm.getCameraIdList()) {
                Integer f = cm.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING);
                if (f == null) continue;
                if (f == CameraMetadata.LENS_FACING_BACK) backs.add(id);
                else if (f == CameraMetadata.LENS_FACING_FRONT) fronts.add(id);
            }
            if (which == 1) {
                if (fronts.isEmpty()) return fail(r, Logbook.t("前面のカメラが無い", "no front camera"));
                r.camId = fronts.get(0);
            } else if (backs.isEmpty()) {
                if (which != 0 || fronts.isEmpty()) return fail(r, Logbook.t("背面のカメラが無い", "no back camera"));
                r.camId = fronts.get(0); // 背面の無い機種（タブレットなど）は、おまかせのときだけ前面で代わりにする
            } else {
                String main = backs.get(0);
                r.camId = main;
                if (which == 2 || which == 3) {
                    double mainT = tanHalf(cm.getCameraCharacteristics(main));
                    String best = null;
                    double bestT = mainT;
                    for (String id : backs) {
                        double v = tanHalf(cm.getCameraCharacteristics(id));
                        if (which == 2 ? v > bestT * 1.15 : v < bestT / 1.5) { best = id; bestT = v; }
                    }
                    if (best != null) {
                        r.camId = best;
                        if (mainT > 0 && bestT > 0) r.m = (float) (mainT / bestT);
                    } else {
                        CameraCharacteristics ch = cm.getCameraCharacteristics(main);
                        Range<Float> zr = Build.VERSION.SDK_INT >= 30 ? ch.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) : null;
                        if (zr != null && which == 2 && zr.getLower() < 0.95f) r.ratio = zr.getLower();
                        else if (zr != null && which == 3 && zr.getUpper() >= 2f && isMultiCamera(ch)) r.ratio = Math.min(zr.getUpper(), 3f);
                        else return fail(r, which == 2 ? Logbook.t("超広角のカメラが無い", "no ultra wide camera") : Logbook.t("望遠のカメラが無い", "no telephoto camera"));
                    }
                }
            }
            Integer ts = cm.getCameraCharacteristics(r.camId).get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE);
            r.realtimeClock = ts != null && ts == CameraMetadata.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
            return r;
        } catch (CameraAccessException | RuntimeException e) {
            return fail(r, Logbook.t("カメラの一覧を読めない：", "cannot read the camera list: ") + e);
        }
    }

    private static Lens fail(Lens r, String why) {
        r.status = 2;
        r.why = why;
        return r;
    }

    static double tanHalf(CameraCharacteristics ch) {
        SizeF sz = ch.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE);
        float[] fl = ch.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS);
        if (sz == null || fl == null || fl.length == 0 || fl[0] <= 0) return 0;
        return sz.getWidth() / (2.0 * fl[0]);
    }

    static boolean isMultiCamera(CameraCharacteristics ch) {
        return hasCap(ch, CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA);
    }

    static boolean hasCap(CameraCharacteristics ch, int cap) {
        int[] caps = ch.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
        if (caps == null) return false;
        for (int c : caps) if (c == cap) return true;
        return false;
    }

    private static boolean has(int[] a, int v) {
        if (a == null) return false;
        for (int x : a) if (x == v) return true;
        return false;
    }

    // ---- 調べる（カメラごとに 1 回。m はレンズの選び方で変わるので鍵に入れる）----
    private static final Map<String, CamCaps> cache = new HashMap<>();

    static synchronized CamCaps of(Context ctx, String camId, float m) {
        String key = camId + "/" + m;
        CamCaps c = cache.get(key);
        if (c == null) {
            c = build(ctx, camId, m);
            cache.put(key, c);
        }
        return c;
    }

    // 画面の設定のカメラ（Prefs.LENS）で調べる。無ければ null
    static CamCaps forLens(Context ctx, int lens) {
        Lens l = pick(ctx, lens);
        if (l.status != 0) return null;
        return of(ctx, l.camId, l.m);
    }

    // API 35 の鍵（コンパイルは 34 で組むので名前で作る）。無い版・無い機種では null が返る
    static final CameraCharacteristics.Key<Integer> TORCH_STRENGTH_MAX = new CameraCharacteristics.Key<>("android.flash.torchStrengthMaxLevel", Integer.class);

    private static CamCaps build(Context ctx, String camId, float m) {
        CamCaps c = new CamCaps();
        c.camId = camId;
        c.m = m;
        CameraManager cm = ctx.getSystemService(CameraManager.class);
        CameraCharacteristics ch;
        try {
            ch = cm.getCameraCharacteristics(camId);
        } catch (CameraAccessException | RuntimeException e) {
            return c;
        }
        Integer facing = ch.get(CameraCharacteristics.LENS_FACING);
        c.front = facing != null && facing == CameraMetadata.LENS_FACING_FRONT;
        c.back = facing != null && facing == CameraMetadata.LENS_FACING_BACK;
        Integer lv = ch.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL);
        c.hwLevel = lv == null ? 0 : lv;
        Integer so = ch.get(CameraCharacteristics.SENSOR_ORIENTATION);
        c.sensorOrientation = so == null ? 90 : so;
        c.active = ch.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
        boolean manualSensor = hasCap(ch, CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR);
        boolean manualPost = hasCap(ch, CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING);

        // ピント
        int[] af = ch.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES);
        if (has(af, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO)) c.afAuto = CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO;
        else if (has(af, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE)) c.afAuto = CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE;
        else if (has(af, CameraMetadata.CONTROL_AF_MODE_AUTO)) c.afAuto = CameraMetadata.CONTROL_AF_MODE_AUTO;
        else c.afAuto = -1;
        c.focusAuto = c.afAuto >= 0;
        Float mf = ch.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE);
        c.minFocus = mf == null ? 0 : mf;
        c.focusManual = c.minFocus > 0 && has(af, CameraMetadata.CONTROL_AF_MODE_OFF);
        c.focusLock = c.focusManual && c.focusAuto; // 固定は「今のレンズの位置のまま手動にする」で作る
        int[] scenes = ch.get(CameraCharacteristics.CONTROL_AVAILABLE_SCENE_MODES);
        c.faceAf = has(scenes, CameraMetadata.CONTROL_SCENE_MODE_FACE_PRIORITY);
        Integer maxAf = ch.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AF);
        c.centerAf = maxAf != null && maxAf > 0 && c.active != null;

        // 明るさ
        Boolean aeLock = ch.get(CameraCharacteristics.CONTROL_AE_LOCK_AVAILABLE);
        c.expLock = aeLock != null && aeLock;
        int[] ae = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_MODES);
        Range<Integer> iso = ch.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE);
        Range<Long> et = ch.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE);
        c.expManual = manualSensor && has(ae, CameraMetadata.CONTROL_AE_MODE_OFF) && iso != null && et != null;
        if (iso != null) { c.isoMin = iso.getLower(); c.isoMax = iso.getUpper(); }
        if (et != null) { c.expMinNs = et.getLower(); c.expMaxNs = et.getUpper(); }
        Range<Integer> comp = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE);
        Rational step = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP);
        if (comp != null && step != null && step.floatValue() > 0 && comp.getUpper() > comp.getLower()) {
            c.biasStep = step.floatValue();
            c.compMin = comp.getLower();
            c.compMax = comp.getUpper();
            c.biasMin = Math.max(comp.getLower() * c.biasStep, -3f);
            c.biasMax = Math.min(comp.getUpper() * c.biasStep, 3f);
        }

        // 色。色温度は、センサーの色の表（DNG と同じ ColorMatrix）があって、手動の色の処理ができるときだけ
        Boolean awbLock = ch.get(CameraCharacteristics.CONTROL_AWB_LOCK_AVAILABLE);
        c.wbLock = awbLock != null && awbLock;
        int[] awb = ch.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES);
        c.cm1 = ch.get(CameraCharacteristics.SENSOR_COLOR_TRANSFORM1);
        c.cm2 = ch.get(CameraCharacteristics.SENSOR_COLOR_TRANSFORM2);
        c.cc1 = ch.get(CameraCharacteristics.SENSOR_CALIBRATION_TRANSFORM1);
        c.cc2 = ch.get(CameraCharacteristics.SENSOR_CALIBRATION_TRANSFORM2);
        Integer i1 = ch.get(CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1);
        Byte i2 = ch.get(CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT2);
        c.ill1 = i1 == null ? 0 : i1;
        c.ill2 = i2 == null ? 0 : i2;
        c.wbTemp = manualPost && has(awb, CameraMetadata.CONTROL_AWB_MODE_OFF) && c.cm1 != null && c.ill1 != 0;

        // ズーム
        Float maxDz = ch.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM);
        Range<Float> zr = Build.VERSION.SDK_INT >= 30 ? ch.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) : null;
        if (zr != null) {
            c.ratioApi = true;
            c.ratioLo = zr.getLower();
            c.ratioHi = zr.getUpper();
        } else {
            c.ratioLo = 1f;
            c.ratioHi = maxDz == null ? 1f : Math.max(1f, maxDz);
        }
        c.zoomMin = c.ratioLo * m;
        c.zoomMax = Math.max(c.zoomMin, Math.min(c.ratioHi * m, 10f)); // 見え方で 10 倍まで（iPhone と同じ）
        List<Float> bases = new ArrayList<>();
        bases.add(m);
        if (isMultiCamera(ch) && Build.VERSION.SDK_INT >= 28) {
            double t0 = tanHalf(ch);
            try {
                Set<String> phys = ch.getPhysicalCameraIds();
                for (String p : phys) {
                    double t = tanHalf(cm.getCameraCharacteristics(p));
                    if (t0 > 0 && t > 0) bases.add((float) (t0 / t) * m);
                }
            } catch (CameraAccessException | RuntimeException ignored) {
            }
        }
        if (c.ratioLo < 0.95f) bases.add(c.ratioLo * m); // 超広角へ切り替わる倍率（物理カメラが見えない機種でも）
        List<Float> stops = new ArrayList<>();
        float optical = c.zoomMin;
        for (float b : bases) {
            if (b < c.zoomMin - 0.01f || b > c.zoomMax + 0.01f) continue;
            optical = Math.max(optical, b);
            addStop(stops, roundZoom(b), c);
            if (b >= 0.99f) addStop(stops, roundZoom(b * 2), c);
        }
        java.util.Collections.sort(stops);
        c.zoomStops = new float[stops.size()];
        for (int i = 0; i < c.zoomStops.length; i++) c.zoomStops[i] = stops.get(i);
        c.zoomOptical = optical;

        // ライト。前面カメラのときは背面のライトを CameraManager で点ける（ほかのカメラを開いている間は、機種によっては点かない）
        Boolean fl = ch.get(CameraCharacteristics.FLASH_INFO_AVAILABLE);
        c.torchOwn = fl != null && fl;
        if (c.torchOwn) {
            c.torchId = camId;
            if (Build.VERSION.SDK_INT >= 35) {
                try {
                    Integer mx = ch.get(TORCH_STRENGTH_MAX);
                    if (mx != null && mx > 1) c.torchMax = mx;
                } catch (RuntimeException ignored) {
                }
            }
        } else if (c.front) {
            try {
                for (String id : cm.getCameraIdList()) {
                    CameraCharacteristics o = cm.getCameraCharacteristics(id);
                    Integer f = o.get(CameraCharacteristics.LENS_FACING);
                    Boolean ofl = o.get(CameraCharacteristics.FLASH_INFO_AVAILABLE);
                    if (f != null && f == CameraMetadata.LENS_FACING_BACK && ofl != null && ofl) {
                        c.torchId = id;
                        if (Build.VERSION.SDK_INT >= 33) {
                            Integer mx = o.get(CameraCharacteristics.FLASH_INFO_STRENGTH_MAXIMUM_LEVEL);
                            if (mx != null && mx > 1) c.torchMax = mx;
                        }
                        break;
                    }
                }
            } catch (CameraAccessException | RuntimeException ignored) {
            }
        }
        c.torch = c.torchId != null;
        c.torchLevels = c.torch && c.torchMax > 1;

        // 手ぶれ補正
        int[] vs = ch.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES);
        List<Integer> sm = new ArrayList<>();
        sm.add(0);
        if (Build.VERSION.SDK_INT >= 33 && has(vs, CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_PREVIEW_STABILIZATION)) sm.add(1);
        if (has(vs, CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_ON)) sm.add(2);
        c.stabModes = new int[sm.size()];
        for (int i = 0; i < c.stabModes.length; i++) c.stabModes[i] = sm.get(i);
        c.ois = has(ch.get(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION), CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_ON);

        // センターフレーム（Android 14 の自動フレーミング）
        if (Build.VERSION.SDK_INT >= 34) {
            Boolean a = ch.get(CameraCharacteristics.CONTROL_AUTOFRAMING_AVAILABLE);
            c.autoframing = a != null && a;
        }
        // 顔（切り抜きの「顔」）
        int[] fd = ch.get(CameraCharacteristics.STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES);
        if (has(fd, CameraMetadata.STATISTICS_FACE_DETECT_MODE_SIMPLE)) c.faceMode = CameraMetadata.STATISTICS_FACE_DETECT_MODE_SIMPLE;
        else if (has(fd, CameraMetadata.STATISTICS_FACE_DETECT_MODE_FULL)) c.faceMode = CameraMetadata.STATISTICS_FACE_DETECT_MODE_FULL;
        c.face = c.faceMode != 0;
        // 水平を保つ：背面のカメラで、重さの向きを読めるとき（前面はセンサーの向きが機種で違うので、本物で確かめるまで出さない。iPhone と同じ）
        SensorManager smg = ctx.getSystemService(SensorManager.class);
        c.level = c.back && smg != null && (smg.getDefaultSensor(Sensor.TYPE_GRAVITY) != null || smg.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) != null);
        return c;
    }

    private static void addStop(List<Float> stops, float z, CamCaps c) {
        if (z < c.zoomMin - 0.01f || z > c.zoomMax + 0.01f) return;
        for (float s : stops) if (Math.abs(s - z) < 0.05f) return;
        stops.add(z);
    }

    // 0.1 刻みに丸め、整数に近ければ整数にする（iPhone の roundZoom と同じ）
    static float roundZoom(float z) {
        float n = Math.round(z);
        if (n >= 1 && Math.abs(z - n) < 0.15f) return n;
        return Math.round(z * 10) / 10f;
    }

    // 倍率の文字（0.5×・1×・2.5× など）
    static String zoomLabel(float z) {
        float r = Math.round(z * 10) / 10f;
        return (Math.abs(r - Math.round(r)) < 0.001f ? String.valueOf(Math.round(r)) : String.format(java.util.Locale.US, "%.1f", r)) + "×";
    }

    // ---- 色温度 → 色の倍率（DNG の決まり。ColorMatrix を色温度の逆数で混ぜ、その光の白がセンサーでどう見えるかから倍率を出す）----
    // 返すのは R・G・B の倍率（G が 1）。作れなければ null
    float[] wbGains(float kelvin) {
        if (cm1 == null) return null;
        double t = Math.max(2000, Math.min(10000, kelvin));
        double t1 = illuminantK(ill1), t2 = illuminantK(ill2);
        double[] cm = mat(cm1), cc = cc1 != null ? mat(cc1) : identity();
        if (cm2 != null && t2 > 0 && t1 > 0 && Math.abs(t1 - t2) > 1) {
            // t1 が低い方になるように並べてから、1/T で混ぜる
            double[] m1 = mat(cm1), m2 = mat(cm2), c1 = cc1 != null ? mat(cc1) : identity(), c2 = cc2 != null ? mat(cc2) : identity();
            if (t1 > t2) { double x = t1; t1 = t2; t2 = x; double[] y = m1; m1 = m2; m2 = y; y = c1; c1 = c2; c2 = y; }
            double w = (1 / t - 1 / t2) / (1 / t1 - 1 / t2);
            w = Math.max(0, Math.min(1, w));
            cm = new double[9];
            cc = new double[9];
            for (int i = 0; i < 9; i++) { cm[i] = w * m1[i] + (1 - w) * m2[i]; cc[i] = w * c1[i] + (1 - w) * c2[i]; }
        }
        double[] xyz = planckXyz(t);
        double[] n = mul(cc, mul(cm, xyz));
        if (n[0] <= 0 || n[1] <= 0 || n[2] <= 0) return null;
        return new float[] { (float) (n[1] / n[0]), 1f, (float) (n[1] / n[2]) };
    }

    private static double[] identity() { return new double[] { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; }

    private static double[] mat(ColorSpaceTransform t) {
        double[] m = new double[9];
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[r * 3 + c] = t.getElement(c, r).doubleValue();
        return m;
    }

    private static double[] mul(double[] m, double[] v) {
        return new double[] { m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2] };
    }

    // 黒体の色（Kim たち 2002 の近似）を XYZ（Y = 1）で
    private static double[] planckXyz(double t) {
        double t2 = t * t, t3 = t2 * t, x, y;
        if (t <= 4000) x = -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910;
        else x = -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
        double x2 = x * x, x3 = x2 * x;
        if (t <= 2222) y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
        else if (t <= 4000) y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
        else y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
        return new double[] { x / y, 1, (1 - x - y) / y };
    }

    // SENSOR_REFERENCE_ILLUMINANT の色温度（K）。知らない物は 0
    private static double illuminantK(int v) {
        switch (v) {
            case 1: case 4: case 9: return 5500;  // DAYLIGHT・FLASH・FINE_WEATHER
            case 2: case 14: return 4150;         // FLUORESCENT・COOL_WHITE_FLUORESCENT
            case 3: return 2850;                  // TUNGSTEN
            case 10: return 6500;                 // CLOUDY_WEATHER
            case 11: return 7500;                 // SHADE
            case 12: return 6430;                 // DAYLIGHT_FLUORESCENT
            case 13: return 5000;                 // DAY_WHITE_FLUORESCENT
            case 15: return 3450;                 // WHITE_FLUORESCENT
            case 17: return 2856;                 // STANDARD_A
            case 18: return 4874;                 // STANDARD_B
            case 19: return 6774;                 // STANDARD_C
            case 20: return 5503;                 // D55
            case 21: return 6504;                 // D65
            case 22: return 7504;                 // D75
            case 23: return 5003;                 // D50
            case 24: return 3200;                 // ISO_STUDIO_TUNGSTEN
            default: return 0;
        }
    }
}
