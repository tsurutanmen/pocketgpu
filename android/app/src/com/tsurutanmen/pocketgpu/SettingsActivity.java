package com.tsurutanmen.pocketgpu;

import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CameraMetadata;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Range;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;

// 設定の画面。並びは iPhone のカメラタブと同じ：
//   カメラ（使うカメラ・解像度・フレームレート・画質・PC からの指定を優先するか）
//   カメラの操作（向き・ズームと切り抜き・ピント・明るさ・色・ライト・手ぶれ補正・エフェクト・熱の対策）。この端末とこのカメラで使える物だけ出す
//   マイク（入力・音の種類・マイクの音量と入力レベル・ステレオ）・画面を暗くする・Wi-Fi・PC との接続・GPU・このアプリ
// 値は設定に書くだけ。カメラとマイクには、設定の変化を見ている CameraBridge と AudioBridge が当てる（PC から変えたときと同じ道）。
// PC から変えた値は、カメラの操作の部分を作り直して見せる
public class SettingsActivity extends BaseActivity {
    private TextView wifiCode, wifiCmd, micLevel, micPcNote, levelNow;
    private LinearLayout camBox;
    private SharedPreferences p;
    private boolean selfWrite;
    private int ticks;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Runnable refresh = new Runnable() {
        @Override public void run() {
            if (ticks++ % 4 == 0) updateWifi();
            updateLive();
            ui.postDelayed(this, 250);
        }
    };
    private final Runnable rebuild = this::buildCamControls;
    private static final HashSet<String> CAM_KEYS = new HashSet<>(Arrays.asList(Prefs.MIRROR, Prefs.FLIP_V, Prefs.ROTATION, Prefs.FOCUS, Prefs.FOCUS_POS,
            Prefs.FOCUS_TARGET, Prefs.EXPOSURE, Prefs.BIAS, Prefs.ISO, Prefs.SHUTTER, Prefs.WB, Prefs.WB_TEMP, Prefs.ZOOM, Prefs.TORCH, Prefs.TORCH_LEVEL,
            Prefs.STAB, Prefs.CENTER_STAGE, Prefs.CROP, Prefs.CROP_POS, Prefs.CROP_SIZE, Prefs.CROP_X, Prefs.CROP_Y, Prefs.LEVEL, Prefs.SCREEN_WARM,
            Prefs.THERMAL_FPS));
    // PC から変わったときだけ作り直す（自分で書いたときは作り直さない。スライダーを動かしている途中で消えないように）
    private final SharedPreferences.OnSharedPreferenceChangeListener prefListener = (sp, k) -> {
        if (selfWrite || k == null || !CAM_KEYS.contains(k)) return;
        ui.removeCallbacks(rebuild);
        ui.postDelayed(rebuild, 200);
    };

    private interface IntSink { void set(int i); }
    private interface BoolSink { void set(boolean on); }
    private interface FloatSink { void set(float v); }
    private interface Text { String of(float v); }
    private interface Edit { void put(SharedPreferences.Editor e); }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        p = Prefs.get(this);
        int pad = Ui.dp(this, 16);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);
        if (getActionBar() != null) getActionBar().setDisplayHomeAsUpEnabled(true);

        // カメラ
        root.addView(Ui.header(this, getString(R.string.sec_camera)));
        List<Integer> lensIds = availableLenses();
        List<String> lensNames = new ArrayList<>();
        for (int id : lensIds) lensNames.add(lensName(id));
        int curLens = p.getInt(Prefs.LENS, 0);
        addChoice(root, getString(R.string.set_lens), lensNames, Math.max(0, lensIds.indexOf(curLens)), i -> {
            // カメラを替えたら、倍率はそのカメラをそのまま使う見え方に戻す（iPhone と同じ）
            write(e -> e.putInt(Prefs.LENS, lensIds.get(i)).remove(Prefs.ZOOM));
            changed();
            buildCamControls();
        });

        List<String> res = new ArrayList<>();
        for (String r : Prefs.RES_LIST) res.add(r.replace("x", "×"));
        int curRes = Arrays.asList(Prefs.RES_LIST).indexOf(p.getString(Prefs.RES, "1280x720"));
        addChoice(root, getString(R.string.set_res), res, Math.max(0, curRes), i -> { write(e -> e.putString(Prefs.RES, Prefs.RES_LIST[i])); changed(); });

        List<String> fps = new ArrayList<>();
        int curFps = 2;
        for (int i = 0; i < Prefs.FPS_LIST.length; i++) {
            fps.add(getString(R.string.fps_item, Prefs.FPS_LIST[i]));
            if (Prefs.FPS_LIST[i] == p.getInt(Prefs.FPS, 30)) curFps = i;
        }
        addChoice(root, getString(R.string.set_fps), fps, curFps, i -> { write(e -> e.putInt(Prefs.FPS, Prefs.FPS_LIST[i])); changed(); buildCamControls(); });

        List<String> q = Arrays.asList(getString(R.string.q_standard), getString(R.string.q_high), getString(R.string.q_max));
        addChoice(root, getString(R.string.set_quality), q, Math.max(0, Math.min(2, p.getInt(Prefs.QUALITY, 1) - 1)), i -> { write(e -> e.putInt(Prefs.QUALITY, i + 1)); changed(); });

        addSwitch(root, getString(R.string.set_pc_priority), p.getBoolean(Prefs.PC_PRIORITY, true), on -> { write(e -> e.putBoolean(Prefs.PC_PRIORITY, on)); changed(); });
        root.addView(Ui.note(this, getString(R.string.set_cam_note)));

        // カメラの操作（作り直せるように 1 つの入れ物に入れる）
        camBox = new LinearLayout(this);
        camBox.setOrientation(LinearLayout.VERTICAL);
        root.addView(camBox);
        buildCamControls();

        // マイク
        buildMic(root);

        // 画面を暗くする
        root.addView(Ui.header(this, getString(R.string.sec_dim)));
        addSwitch(root, getString(R.string.ctl_auto_dim), Prefs.autoDim(p), on -> write(e -> e.putBoolean(Prefs.AUTO_DIM, on)));
        List<String> secs = new ArrayList<>();
        int curSec = 1;
        for (int i = 0; i < Prefs.DIM_SECONDS.length; i++) {
            int s = Prefs.DIM_SECONDS[i];
            secs.add(s < 60 ? getString(R.string.dim_secs, s) : getString(R.string.dim_mins, s / 60));
            if (s == Prefs.autoDimSecs(p)) curSec = i;
        }
        addChoice(root, getString(R.string.ctl_dim_after), secs, curSec, i -> write(e -> e.putInt(Prefs.AUTO_DIM_SECS, Prefs.DIM_SECONDS[i])));
        addButton(root, getString(R.string.btn_dim_now), () -> ScreenState.setDim(this, true));
        root.addView(Ui.note(this, getString(R.string.note_dim)));

        // Wi-Fi
        root.addView(Ui.header(this, getString(R.string.sec_wifi)));
        addSwitch(root, getString(R.string.set_wifi), p.getBoolean(Prefs.WIFI, false), on -> {
            write(e -> e.putBoolean(Prefs.WIFI, on));
            changed();
            updateWifi();
        });
        wifiCode = Ui.note(this, "");
        root.addView(wifiCode);
        wifiCmd = Ui.mono(this, 13);
        root.addView(wifiCmd);

        // PC との接続（古い PC のツール）
        root.addView(Ui.header(this, getString(R.string.sec_pc_link)));
        addSwitch(root, getString(R.string.set_legacy), p.getBoolean(Prefs.LEGACY_TCP, false), on -> { write(e -> e.putBoolean(Prefs.LEGACY_TCP, on)); changed(); });
        root.addView(Ui.note(this, getString(R.string.note_legacy)));

        // GPU
        root.addView(Ui.header(this, getString(R.string.sec_gpu)));
        root.addView(Ui.note(this, getString(R.string.gpu_note)));
        addSwitch(root, getString(R.string.set_perf), Prefs.perfBoost(p), on -> write(e -> e.putBoolean(Prefs.PERF_BOOST, on)));
        root.addView(Ui.note(this, getString(R.string.note_perf)));

        // このアプリ
        root.addView(Ui.header(this, getString(R.string.sec_about)));
        String ver = "?";
        try {
            PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
            ver = pi.versionName;
        } catch (PackageManager.NameNotFoundException ignored) {
        }
        root.addView(Ui.note(this, getString(R.string.about_version, ver)));

        ScrollView sv = new ScrollView(this);
        sv.addView(root);
        setContentView(sv);
        p.registerOnSharedPreferenceChangeListener(prefListener);
    }

    @Override
    protected void onDestroy() {
        p.unregisterOnSharedPreferenceChangeListener(prefListener);
        super.onDestroy();
    }

    @Override
    protected void onResume() {
        super.onResume();
        ui.post(refresh);
    }

    @Override
    protected void onPause() {
        ui.removeCallbacks(refresh);
        super.onPause();
    }

    @Override
    public boolean onNavigateUp() {
        finish();
        return true;
    }

    private void write(Edit ed) {
        selfWrite = true;
        SharedPreferences.Editor e = p.edit();
        ed.put(e);
        e.apply(); // 画面の糸から書くと、設定の変化の知らせはここで呼ばれる
        selfWrite = false;
    }

    // 変わったら、サービスに知らせる（流している途中なら、PC のツールが対応していればすぐ切り替わる）
    private void changed() {
        BridgeService.apply(this);
    }

    // ---- 部品 ----

    private void addChoice(LinearLayout root, String label, List<String> items, int selected, IntSink sink) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setMinimumHeight(Ui.dp(this, 48));
        TextView l = new TextView(this);
        l.setText(label);
        l.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        l.setTextColor(Ui.themeColor(this, android.R.attr.textColorPrimary));
        r.addView(l, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        Spinner sp = new Spinner(this);
        ArrayAdapter<String> ad = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, items);
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(ad);
        sp.setSelection(selected, false);
        sp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            int last = selected;
            @Override public void onItemSelected(AdapterView<?> parent, View view, int pos, long id) {
                if (pos == last) return;
                last = pos;
                sink.set(pos);
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });
        r.addView(sp);
        root.addView(r);
        root.addView(Ui.divider(this));
    }

    // 値の選択。values の中に今の値が無ければ、初めの値として見せる（iPhone と同じ）
    private void addValues(LinearLayout root, String label, String[] names, int[] values, int cur, IntSink sink) {
        int sel = 0;
        for (int i = 0; i < values.length; i++) if (values[i] == cur) sel = i;
        addChoice(root, label, Arrays.asList(names), sel, i -> sink.set(values[i]));
    }

    private void addSwitch(LinearLayout root, String text, boolean on, BoolSink sink) {
        Switch s = new Switch(this);
        s.setText(text);
        s.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        s.setTextColor(Ui.themeColor(this, android.R.attr.textColorPrimary));
        s.setMinHeight(Ui.dp(this, 48));
        s.setChecked(on);
        s.setOnCheckedChangeListener((v, x) -> sink.set(x));
        root.addView(s);
        root.addView(Ui.divider(this));
    }

    private void addButton(LinearLayout root, String text, Runnable r) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setOnClickListener(v -> r.run());
        root.addView(b, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
    }

    // 上に名前と値、下にスライダー。log なら対数（ズームと ISO）。steps は刻みの数
    private TextView addSlider(LinearLayout root, String label, float min, float max, float cur, boolean log, int steps, Text text, FloatSink sink) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setMinimumHeight(Ui.dp(this, 40));
        TextView l = new TextView(this);
        l.setText(label);
        l.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        l.setTextColor(Ui.themeColor(this, android.R.attr.textColorPrimary));
        r.addView(l, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView v = new TextView(this);
        v.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        v.setTextColor(Ui.themeColor(this, android.R.attr.textColorSecondary));
        v.setText(text.of(cur));
        r.addView(v);
        root.addView(r);
        SeekBar sb = new SeekBar(this);
        sb.setMax(steps);
        sb.setProgress(Math.round(toT(cur, min, max, log) * steps));
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int prog, boolean fromUser) {
                if (!fromUser) return;
                float x = fromT(prog / (float) steps, min, max, log);
                v.setText(text.of(x));
                sink.set(x);
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        root.addView(sb);
        root.addView(Ui.divider(this));
        return v;
    }

    // 左右に文字があるスライダー（近く―遠く）
    private void addRange(LinearLayout root, String left, String right, float cur, FloatSink sink) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setMinimumHeight(Ui.dp(this, 48));
        TextView a = Ui.note(this, left), z = Ui.note(this, right);
        SeekBar sb = new SeekBar(this);
        sb.setMax(100);
        sb.setProgress(Math.round(cur * 100));
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int prog, boolean fromUser) { if (fromUser) sink.set(prog / 100f); }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        r.addView(a);
        r.addView(sb, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        r.addView(z);
        root.addView(r);
        root.addView(Ui.divider(this));
    }

    private static float toT(float v, float min, float max, boolean log) {
        if (max <= min) return 0;
        if (log && min > 0) return (float) Math.max(0, Math.min(1, Math.log(Math.max(v, min) / min) / Math.log(max / min)));
        return Math.max(0, Math.min(1, (v - min) / (max - min)));
    }

    private static float fromT(float t, float min, float max, boolean log) {
        if (log && min > 0) return (float) (min * Math.pow(max / min, t));
        return min + (max - min) * t;
    }

    private TextView note(LinearLayout root, String s) {
        TextView t = Ui.note(this, s);
        root.addView(t);
        return t;
    }

    // ---- カメラの操作（iPhone の CameraControlsView と同じ並びと文言）----

    private void buildCamControls() {
        camBox.removeAllViews();
        int lens = p.getInt(Prefs.LENS, 0);
        CamCaps c = CamCaps.forLens(this, lens);
        if (c == null) {
            note(camBox, getString(R.string.note_no_camera));
            return;
        }
        int fps = p.getInt(Prefs.FPS, 30);
        LinearLayout box = camBox;
        BridgeService svc = BridgeService.instance;

        // 向き
        if (!CameraBridge.glFailed) {
            box.addView(Ui.header(this, getString(R.string.sec_orient)));
            addSwitch(box, getString(R.string.ctl_mirror), Prefs.b(p, Prefs.MIRROR, false), on -> write(e -> e.putBoolean(Prefs.MIRROR, on)));
            addSwitch(box, getString(R.string.ctl_flipv), Prefs.b(p, Prefs.FLIP_V, false), on -> write(e -> e.putBoolean(Prefs.FLIP_V, on)));
            int rot = Prefs.i(p, Prefs.ROTATION, 0);
            addValues(box, getString(R.string.ctl_rotation), new String[] { "0°", "90°", "180°", "270°" }, new int[] { 0, 90, 180, 270 }, rot,
                    v -> { write(e -> e.putInt(Prefs.ROTATION, v)); buildCamControls(); });
            if (rot == 90 || rot == 270) note(box, getString(R.string.note_portrait));
            if (svc != null && svc.camera.connected && svc.camera.rotSkipped) note(box, getString(R.string.note_rot_skipped));
        } else {
            box.addView(Ui.header(this, getString(R.string.sec_orient)));
            note(box, getString(R.string.note_gl_failed));
        }

        // ズームと切り抜き
        box.addView(Ui.header(this, getString(R.string.sec_zoom)));
        if (c.zoomMax > c.zoomMin + 0.01f) {
            float z = Math.max(c.zoomMin, Math.min(c.zoomMax, p.contains(Prefs.ZOOM) ? Prefs.f(p, Prefs.ZOOM, 1f) : CamCaps.pick(this, lens).baseZoom()));
            if (c.zoomStops.length > 0) {
                LinearLayout row = new LinearLayout(this);
                row.setOrientation(LinearLayout.HORIZONTAL);
                for (float s : c.zoomStops) {
                    Button b = new Button(this);
                    b.setText(CamCaps.zoomLabel(s));
                    b.setAllCaps(false);
                    b.setAlpha(Math.abs(z - s) < 0.05f ? 1f : 0.6f);
                    b.setOnClickListener(v -> { write(e -> e.putFloat(Prefs.ZOOM, s)); buildCamControls(); });
                    row.addView(b, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
                }
                box.addView(row);
            }
            addSlider(box, getString(R.string.ctl_digital_zoom), c.zoomMin, c.zoomMax, z, true, 1000, CamCaps::zoomLabel,
                    v -> write(e -> e.putFloat(Prefs.ZOOM, Math.round(v * 100) / 100f)));
            note(box, getString(R.string.note_zoom_soft, CamCaps.zoomLabel(c.zoomOptical)));
        }
        if (!CameraBridge.glFailed) {
            boolean crop = Prefs.b(p, Prefs.CROP, false);
            addSwitch(box, getString(R.string.ctl_crop), crop, on -> { write(e -> e.putBoolean(Prefs.CROP, on)); buildCamControls(); });
            if (crop) {
                int pos = Prefs.i(p, Prefs.CROP_POS, 0);
                List<String> names = new ArrayList<>(Arrays.asList(getString(R.string.crop_center), getString(R.string.crop_left), getString(R.string.crop_right)));
                List<Integer> vals = new ArrayList<>(Arrays.asList(0, 1, 2));
                if (c.face) { names.add(getString(R.string.crop_face)); vals.add(3); }
                names.add(getString(R.string.crop_manual));
                vals.add(4);
                int[] va = new int[vals.size()];
                for (int i = 0; i < va.length; i++) va[i] = vals.get(i);
                addValues(box, getString(R.string.ctl_crop_pos), names.toArray(new String[0]), va, pos, v -> { write(e -> e.putInt(Prefs.CROP_POS, v)); buildCamControls(); });
                addSlider(box, getString(R.string.ctl_crop_size), 0.3f, 1f, Prefs.cropSize(p), false, 70, v -> Math.round(v * 100) + "%",
                        v -> write(e -> e.putFloat(Prefs.CROP_SIZE, Math.round(v * 100) / 100f)));
                if (pos == 4) {
                    addSlider(box, getString(R.string.ctl_crop_x), 0, 1, Prefs.cropX(p), false, 100, v -> Math.round(v * 100) + "%",
                            v -> write(e -> e.putFloat(Prefs.CROP_X, Math.round(v * 100) / 100f)));
                    addSlider(box, getString(R.string.ctl_crop_y), 0, 1, Prefs.cropY(p), false, 100, v -> Math.round(v * 100) + "%",
                            v -> write(e -> e.putFloat(Prefs.CROP_Y, Math.round(v * 100) / 100f)));
                }
                if (pos == 3 && c.face) note(box, getString(R.string.note_crop_face));
            }
            note(box, getString(R.string.note_zoom_crop));
            if (c.level) {
                boolean lv = Prefs.b(p, Prefs.LEVEL, false);
                addSwitch(box, getString(R.string.ctl_level), lv, on -> { write(e -> e.putBoolean(Prefs.LEVEL, on)); buildCamControls(); });
                if (lv) {
                    note(box, getString(R.string.note_level));
                    levelNow = note(box, "");
                    levelNow.setVisibility(View.GONE);
                } else {
                    levelNow = null;
                }
            }
        }

        // ピント
        box.addView(Ui.header(this, getString(R.string.sec_focus)));
        if (c.focusAuto || c.focusManual) {
            List<String> names = new ArrayList<>(Arrays.asList(getString(R.string.mode_auto)));
            List<Integer> vals = new ArrayList<>(Arrays.asList(0));
            if (c.focusLock) { names.add(getString(R.string.mode_lock)); vals.add(1); }
            if (c.focusManual) { names.add(getString(R.string.mode_manual)); vals.add(2); }
            int fm = Prefs.i(p, Prefs.FOCUS, 0);
            int mode = vals.contains(fm) ? fm : 0;
            addValues(box, getString(R.string.sec_focus), names.toArray(new String[0]), toInts(vals), mode, v -> { write(e -> e.putInt(Prefs.FOCUS, v)); buildCamControls(); });
            if (mode == 2) {
                addRange(box, getString(R.string.ctl_near), getString(R.string.ctl_far), Prefs.focusPos(p), v -> write(e -> e.putFloat(Prefs.FOCUS_POS, v)));
            } else if (mode == 1) {
                note(box, getString(R.string.note_focus_lock));
            } else if (c.focusTarget()) {
                addValues(box, getString(R.string.ctl_focus_target), new String[] { getString(R.string.target_faces), getString(R.string.target_center) },
                        new int[] { 0, 1 }, Prefs.i(p, Prefs.FOCUS_TARGET, 0) == 1 ? 1 : 0, v -> write(e -> e.putInt(Prefs.FOCUS_TARGET, v)));
            }
        } else {
            note(box, getString(R.string.note_focus_fixed));
        }

        // 明るさ
        box.addView(Ui.header(this, getString(R.string.sec_exposure)));
        {
            List<String> names = new ArrayList<>(Arrays.asList(getString(R.string.mode_auto)));
            List<Integer> vals = new ArrayList<>(Arrays.asList(0));
            if (c.expLock) { names.add(getString(R.string.mode_lock)); vals.add(1); }
            if (c.expManual) { names.add(getString(R.string.mode_manual)); vals.add(2); }
            int em = Prefs.i(p, Prefs.EXPOSURE, 0);
            int mode = vals.contains(em) ? em : 0;
            addValues(box, getString(R.string.sec_exposure), names.toArray(new String[0]), toInts(vals), mode, v -> { write(e -> e.putInt(Prefs.EXPOSURE, v)); buildCamControls(); });
            if (mode == 2) {
                float iso = Prefs.f(p, Prefs.ISO, 0);
                float lo = Math.max(c.isoMin, 1), hi = Math.max(c.isoMax, lo + 1);
                float shown = iso > 0 ? iso : (float) Math.sqrt(lo * hi);
                TextView isoText = addSlider(box, "ISO", lo, hi, shown, true, 1000, v -> String.valueOf(Math.round(v)), v -> write(e -> e.putFloat(Prefs.ISO, Math.round(v))));
                if (iso <= 0) isoText.setText(R.string.iso_current);
                int[] sh = c.shutters(fps);
                if (sh.length > 0) {
                    String[] sn = new String[sh.length];
                    for (int i = 0; i < sh.length; i++) sn[i] = "1/" + sh[i];
                    int s = Prefs.i(p, Prefs.SHUTTER, 0);
                    int fb = sh[0];
                    for (int x : sh) if (x >= Math.max(fps, 30) * 2) { fb = x; break; }
                    boolean in = false;
                    for (int x : sh) if (x == s) in = true;
                    addValues(box, getString(R.string.ctl_shutter), sn, sh, in ? s : fb, v -> write(e -> e.putInt(Prefs.SHUTTER, v)));
                }
                note(box, getString(R.string.note_flicker));
            } else if (c.biasMax > c.biasMin) {
                int steps = Math.max(1, Math.round((c.biasMax - c.biasMin) / c.biasStep));
                addSlider(box, getString(R.string.ctl_bias), c.biasMin, c.biasMax, Prefs.f(p, Prefs.BIAS, 0), false, steps,
                        v -> String.format(Locale.US, "%+.1f", v), v -> write(e -> e.putFloat(Prefs.BIAS, Math.round(v / c.biasStep) * c.biasStep)));
                if (mode == 1) note(box, getString(R.string.note_exp_lock));
            }
        }

        // 色
        box.addView(Ui.header(this, getString(R.string.sec_color)));
        {
            List<String> names = new ArrayList<>(Arrays.asList(getString(R.string.mode_auto)));
            List<Integer> vals = new ArrayList<>(Arrays.asList(0));
            if (c.wbLock) { names.add(getString(R.string.mode_lock)); vals.add(1); }
            if (c.wbTemp) { names.add(getString(R.string.wb_temp_mode)); vals.add(2); }
            int wm = Prefs.i(p, Prefs.WB, 0);
            int mode = vals.contains(wm) ? wm : 0;
            addValues(box, getString(R.string.ctl_wb), names.toArray(new String[0]), toInts(vals), mode, v -> { write(e -> e.putInt(Prefs.WB, v)); buildCamControls(); });
            if (mode == 2) {
                addSlider(box, getString(R.string.ctl_wb_temp), 2500, 8000, Prefs.wbTemp(p), false, 55, v -> Math.round(v / 100) * 100 + " K",
                        v -> write(e -> e.putFloat(Prefs.WB_TEMP, Math.round(v / 100) * 100)));
                note(box, getString(R.string.note_wb_temp));
            } else if (mode == 1) {
                note(box, getString(R.string.note_wb_lock));
            }
        }

        // ライト
        box.addView(Ui.header(this, getString(R.string.sec_light)));
        if (c.torch) {
            boolean torch = Prefs.b(p, Prefs.TORCH, false);
            addSwitch(box, getString(R.string.ctl_torch), torch, on -> { write(e -> e.putBoolean(Prefs.TORCH, on)); buildCamControls(); });
            if (torch && c.torchLevels) {
                int step = Math.min(Math.max(Math.round(Prefs.torchLevel(p) * 4), 1), 4) * 25;
                addValues(box, getString(R.string.ctl_torch_level), new String[] { "25%", "50%", "75%", "100%" }, new int[] { 25, 50, 75, 100 }, step,
                        v -> write(e -> e.putFloat(Prefs.TORCH_LEVEL, v / 100f)));
            }
            note(box, getString(c.front ? R.string.note_torch_front : R.string.note_torch));
        }
        addButton(box, getString(R.string.btn_screen_light), () -> ScreenState.setLight(this, true));
        addSwitch(box, getString(R.string.ctl_screen_warm), Prefs.b(p, Prefs.SCREEN_WARM, false), on -> write(e -> e.putBoolean(Prefs.SCREEN_WARM, on)));
        note(box, getString(R.string.note_screen_light));

        // 手ぶれ補正
        if (c.stabModes.length > 1) {
            box.addView(Ui.header(this, getString(R.string.sec_stab)));
            String[] names = new String[c.stabModes.length];
            for (int i = 0; i < names.length; i++) names[i] = getString(c.stabModes[i] == 1 ? R.string.stab_low : c.stabModes[i] == 2 ? R.string.stab_std : R.string.stab_off);
            int st = Prefs.i(p, Prefs.STAB, 0);
            boolean in = false;
            for (int x : c.stabModes) if (x == st) in = true;
            int cur = in ? st : 0;
            addValues(box, getString(R.string.sec_stab), names, c.stabModes, cur, v -> { write(e -> e.putInt(Prefs.STAB, v)); buildCamControls(); });
            note(box, getString(cur == 1 ? R.string.stab_note_1 : cur == 2 ? R.string.stab_note_2 : R.string.stab_note_0));
        }

        // エフェクト（Android 14 の自動フレーミング）
        if (c.autoframing) {
            box.addView(Ui.header(this, getString(R.string.sec_effects)));
            addSwitch(box, getString(R.string.ctl_center_stage), Prefs.b(p, Prefs.CENTER_STAGE, false), on -> write(e -> e.putBoolean(Prefs.CENTER_STAGE, on)));
            note(box, getString(R.string.note_center_stage));
        }

        // 熱の対策
        box.addView(Ui.header(this, getString(R.string.sec_heat)));
        addSwitch(box, getString(R.string.ctl_thermal_fps), Prefs.thermalFps(p), on -> write(e -> e.putBoolean(Prefs.THERMAL_FPS, on)));
        note(box, getString(R.string.note_thermal_fps));
    }

    private static int[] toInts(List<Integer> l) {
        int[] a = new int[l.size()];
        for (int i = 0; i < a.length; i++) a[i] = l.get(i);
        return a;
    }

    // ---- マイク（iPhone の MicSettingsView と同じ並びと文言）----

    private void buildMic(LinearLayout root) {
        root.addView(Ui.header(this, getString(R.string.sec_mic)));
        List<String> inNames = new ArrayList<>(Arrays.asList(getString(R.string.mic_in_auto), getString(R.string.mic_in_builtin), getString(R.string.mic_in_wired), getString(R.string.mic_in_usb)));
        List<String> inKeys = new ArrayList<>(Arrays.asList("auto", "builtIn", "wired", "usb"));
        if (Build.VERSION.SDK_INT >= 31) { inNames.add(getString(R.string.mic_in_bt)); inKeys.add("bluetooth"); }
        String in = Prefs.micInput(p);
        TextView inNote = Ui.note(this, inputNote(in));
        addChoice(root, getString(R.string.mic_input), inNames, Math.max(0, inKeys.indexOf(in)), i -> {
            write(e -> e.putString(Prefs.MIC_INPUT, inKeys.get(i)));
            inNote.setText(inputNote(inKeys.get(i)));
        });
        root.addView(inNote);
        int mode = Prefs.micMode(p);
        TextView modeNote = Ui.note(this, modeNote(mode));
        addValues(root, getString(R.string.mic_mode), new String[] { getString(R.string.mic_mode_raw), getString(R.string.mic_mode_std), getString(R.string.mic_mode_voice) },
                new int[] { 1, 2, 3 }, mode, v -> { write(e -> e.putInt(Prefs.MIC_MODE, v)); modeNote.setText(modeNote(v)); });
        root.addView(modeNote);
        addSlider(root, getString(R.string.mic_gain), -20, 30, Prefs.micGain(p), false, 50,
                v -> Math.round(v) == 0 ? "0 dB" : String.format(Locale.US, "%+d dB", Math.round(v)), v -> write(e -> e.putFloat(Prefs.MIC_GAIN, Math.round(v))));
        micLevel = new TextView(this);
        micLevel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        micLevel.setTextColor(Ui.themeColor(this, android.R.attr.textColorSecondary));
        Ui.rowWith(this, root, getString(R.string.mic_level), micLevel);
        root.addView(Ui.note(this, getString(R.string.mic_level_note)));
        micPcNote = Ui.note(this, "");
        micPcNote.setVisibility(View.GONE);
        root.addView(micPcNote);
        boolean stereo = Prefs.b(p, Prefs.MIC_STEREO, false);
        TextView stNote = Ui.note(this, getString(R.string.mic_stereo_note));
        stNote.setVisibility(stereo ? View.VISIBLE : View.GONE);
        addSwitch(root, getString(R.string.mic_stereo), stereo, on -> { write(e -> e.putBoolean(Prefs.MIC_STEREO, on)); stNote.setVisibility(on ? View.VISIBLE : View.GONE); });
        root.addView(stNote);
    }

    private String inputNote(String in) {
        switch (in) {
            case "auto": return getString(R.string.mic_in_note_auto);
            case "bluetooth": return getString(R.string.mic_in_note_bt);
            default: return getString(R.string.mic_in_note_other);
        }
    }

    private String modeNote(int m) {
        return getString(m == 1 ? R.string.mic_mode_note_raw : m == 3 ? R.string.mic_mode_note_voice : R.string.mic_mode_note_std);
    }

    // 0.25 秒ごと：入力レベル（マイクが止まっている間は「無音」）・PC が指定した音の種類・水平の角度
    private void updateLive() {
        float pk = AudioBridge.MicLevel.takePeak();
        micLevel.setText(pk <= 0.00001f ? getString(R.string.mic_silent) : String.format(Locale.US, "%.0f dBFS", 20 * Math.log10(pk)));
        micLevel.setTextColor(pk >= 0.89f ? Thermal.RED : Ui.themeColor(this, android.R.attr.textColorSecondary));
        BridgeService s = BridgeService.instance;
        int pm = s != null && s.audio.connected ? s.audio.pcMicMode : 0;
        micPcNote.setVisibility(pm != 0 ? View.VISIBLE : View.GONE);
        if (pm != 0) micPcNote.setText(getString(R.string.mic_pc_note, getString(pm == 1 ? R.string.mic_mode_raw : pm == 3 ? R.string.mic_mode_voice : R.string.mic_mode_std)));
        if (levelNow != null) {
            boolean on = s != null && s.camera.connected;
            levelNow.setVisibility(on ? View.VISIBLE : View.GONE);
            if (on) levelNow.setText(getString(R.string.note_level_now, String.format(Locale.US, "%.1f", s.camera.levelDeg())));
        }
    }

    private void updateWifi() {
        BridgeService s = BridgeService.instance;
        boolean on = Prefs.on(this, Prefs.WIFI);
        String ip = Ui.wifiAddress(this);
        if (!on || s == null) {
            wifiCode.setVisibility(View.GONE);
            wifiCmd.setVisibility(View.GONE);
            return;
        }
        wifiCode.setVisibility(View.VISIBLE);
        if (s.camera.wifiBlocked) {
            wifiCode.setText(R.string.wifi_blocked);
            wifiCmd.setVisibility(View.GONE);
        } else if (ip == null) {
            wifiCode.setText(getString(R.string.wifi_code, String.valueOf(s.camera.wifiCode)) + "\n" + getString(R.string.wifi_no_addr));
            wifiCmd.setVisibility(View.GONE);
        } else {
            wifiCode.setText(getString(R.string.wifi_code, String.valueOf(s.camera.wifiCode)));
            wifiCmd.setVisibility(View.VISIBLE);
            wifiCmd.setText("webcam_rx.exe --addr " + ip + ":" + CameraBridge.PORT + " --code " + s.camera.wifiCode);
        }
    }

    // この端末にあるカメラだけを並べる（背面・前面・超広角・望遠）
    private List<Integer> availableLenses() {
        List<Integer> out = new ArrayList<>();
        boolean back = false, front = false, wide = false, tele = false;
        try {
            CameraManager cm = getSystemService(CameraManager.class);
            for (String id : cm.getCameraIdList()) {
                CameraCharacteristics ch = cm.getCameraCharacteristics(id);
                Integer f = ch.get(CameraCharacteristics.LENS_FACING);
                if (f == null) continue;
                if (f == CameraMetadata.LENS_FACING_FRONT) front = true;
                if (f != CameraMetadata.LENS_FACING_BACK) continue;
                if (back) { wide = true; tele = true; } // 背面が 2 つ以上見えるなら、どちらかは使える。無ければ PC に「そのカメラが無い」を返す
                back = true;
                if (Build.VERSION.SDK_INT >= 30) {
                    Range<Float> zr = ch.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
                    if (zr != null && zr.getLower() < 0.95f) wide = true;
                    if (zr != null && zr.getUpper() >= 2f && CamCaps.isMultiCamera(ch)) tele = true;
                }
            }
        } catch (Exception ignored) {
        }
        if (back) out.add(0);
        if (front) out.add(1);
        if (wide) out.add(2);
        if (tele) out.add(3);
        if (out.isEmpty()) out.add(0);
        return out;
    }

    private String lensName(int id) {
        switch (id) {
            case 1: return getString(R.string.lens_front);
            case 2: return getString(R.string.lens_ultrawide);
            case 3: return getString(R.string.lens_tele);
            default: return getString(R.string.lens_back);
        }
    }
}
