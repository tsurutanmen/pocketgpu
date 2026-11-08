package com.tsurutanmen.pocketgpu;

import android.Manifest;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Rect;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.ColorSpaceTransform;
import android.hardware.camera2.params.Face;
import android.hardware.camera2.params.MeteringRectangle;
import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.RggbChannelVector;
import android.hardware.camera2.params.SessionConfiguration;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Range;
import android.util.Size;
import android.view.Surface;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.atomic.AtomicInteger;

// Android のカメラを PC のウェブカメラにする送り手。iPhone の CameraBridge.swift と同じ約束（work/camctl2/pc/wcam.h）。
// PC は webcam_rx.exe（と PocketGPU.exe）。USB は adb forward tcp:50064 localabstract:pocketgpu-webcam のあと webcam_rx --addr 127.0.0.1:50064。
// つなぎ方（0.1.2 で直した。Link.java）：
//   USB は抽象名の UNIX ソケット pocketgpu-webcam で待ち、相手の uid が adb のときだけ受ける（同じ端末のほかのアプリには映像を渡さない）。
//   古い PC のツール（adb forward tcp:50064 tcp:50064）は、設定で「古い PC のツールも受け付ける」を入れたときだけ 127.0.0.1:50064 で受ける。
//   Wi-Fi を入れると 0.0.0.0:50064 で待ち、HELLO の 33〜36 バイト目の 6 けたのコードが合ってから、今の接続と入れ替える（バグ探しの指摘 2）。
//   一度に 1 本だけ流す（新しい方を残す）。
// 流れ：PC が HELLO → エンコーダ（MediaCodec の H.264・Surface 入力）→（回す・反転・切り抜き・水平のときは GlStage）→ カメラ（Camera2）→ ACK
//   → Annex B の 1 コマずつ FRAME。camset の足し分（quality・CAMERA_SET・LIVE・ACK の flags）も読む。古い PC は先頭の 32 バイトだけ送るので、それでも動く。
// 0.1.2 で足した（iPhone の camctl・camctl2 に追いつく）：
//   PC が HELLO の flags に WC_HELLO_CTRL（8）を立てたつなぎだけ、CTRL_CAPS（5）と CTRL_STATE（6）を送り、CTRL_SET（18）と CTRL_GET（19）を受ける。
//   番号と形は iPhone と同じ（work/camctl/PROTOCOL.md・work/camctl2/ctl_ids.md）。CAPS には、この機種とこのカメラで使える操作だけを入れる。
//   PC の SET は、画面で変えたときと同じ道を通す（設定に書き、設定の変化を見て当てる）。だから画面にもすぐ出て、次に開いたときも残る。
//   熱：EVENT 3 の arg は iPhone と同じ 4 段階（Thermal.level）。熱いときはコマ数を下げる（ACK の THERMAL と EVENT 5。iPhone と同じ順）
final class CameraBridge {
    static final int PORT = 50064;
    static final String USB_NAME = "pocketgpu-webcam";
    private static final int VERSION = 1;
    private static final int T_HELLO = 1, T_ACK = 2, T_FRAME = 3, T_EVENT = 4, T_CAPS = 5, T_STATE = 6;
    private static final int T_REQ_KEY = 16, T_SET_KBPS = 17, T_SET = 18, T_GET = 19, T_PING = 20, T_PONG = 21;
    private static final int HF_MIRROR = 1, HF_CAMERA_SET = 2, HF_LIVE = 4, HF_CTRL = 8;
    private static final int AF_APP_SETTINGS = 1, AF_APP_PRIORITY = 2, AF_LIVE = 4, AF_CHANGED = 8, AF_THERMAL = 16, AF_CTRL = 32;
    private static final int MAX_INFLIGHT = 4; // 送り終わっていないコマがこれ以上なら捨てて、次のキーフレームから送り直す
    private static final int MAX_PENDING = 4;  // Wi-Fi でコードを待っている接続の数の上限
    private static final byte[] POISON = new byte[0];
    static final float LEVEL_MAX_DEG = 6f;
    static volatile boolean glFailed;          // この端末では回転・切り抜きの描き手を作れなかった（以後は変形なしで送る）
    static final CaptureRequest.Key<Integer> FLASH_STRENGTH = new CaptureRequest.Key<>("android.flash.strengthLevel", Integer.class); // API 35

    private final Context ctx;
    private final SharedPreferences prefs;
    private final CameraManager cm;
    final int wifiCode = 100000 + new SecureRandom().nextInt(900000); // 起動のたびに変わる 6 けた

    // 画面から読む
    volatile boolean connected, cameraStopped, wifiBlocked, pcControl, rotSkipped, pendingNote;
    volatile int curW, curH, curCamera, curFps, lowFps; // lowFps：熱で下げたコマ数（下げていなければ 0）
    volatile long sentFrames;

    private final Link.UsbPort usb;
    private final Link.TcpPort tcp;
    private volatile boolean enabled, wifi, legacy;
    private volatile boolean realtimeClock; // カメラの時刻が elapsedRealtime の時計か（違えば System.nanoTime）

    // ここから下は h の上だけで触る
    private final Handler h;
    private Conn conn;
    private final List<Conn> pending = new ArrayList<>();
    private Hello hello;
    private Resolved cur;
    private CamCaps caps;
    private CameraDevice device;
    private CameraCaptureSession session;
    private CaptureRequest.Builder req;
    private MediaCodec enc;
    private Surface encSurface, camTarget;
    private volatile GlStage gl;
    private byte[] csd;
    private boolean needKey, ackSent, livePosted, wbWaitCcm;
    private int seq, generation, wifiMisses, settingsGen, thermalLevel;
    private Float lockedFocus, lastFocus;
    private Integer lastIso;
    private ColorSpaceTransform lastCcm;
    private Rect lastCrop;
    private boolean torchFired;
    // 前面カメラのときに点ける背面のライト（CameraManager）
    private final Map<String, Boolean> torchAvail = new HashMap<>(), torchOn = new HashMap<>();
    private String mgrTorchId;
    private boolean mgrWant;
    private int mgrLevel = -1;
    // CTRL
    private boolean ctl;
    private CtlCaps ctlNow = new CtlCaps();
    private final Map<Integer, Float> ctlSent = new HashMap<>();
    private final HashSet<Integer> ctlReplies = new HashSet<>();
    private Float ctlZoom;
    private int ctlPollGen;
    private boolean ctlFlushQueued;

    private final Runnable retryCamera = this::reopenCamera;
    private final SharedPreferences.OnSharedPreferenceChangeListener prefListener;

    CameraBridge(Context c) {
        ctx = c.getApplicationContext();
        prefs = Prefs.get(ctx);
        cm = ctx.getSystemService(CameraManager.class);
        HandlerThread t = new HandlerThread("camera-bridge");
        t.start();
        h = new Handler(t.getLooper());
        prefListener = (sp, k) -> { if (k != null) h.post(() -> prefChanged(k)); };
        usb = new Link.UsbPort(USB_NAME, "ウェブカメラ", "Webcam", l -> h.post(() -> acceptTrusted(new Conn(l))));
        tcp = new Link.TcpPort(PORT, "ウェブカメラ", "Webcam", l -> h.post(() -> {
            if (l.remote) acceptPending(new Conn(l));
            else acceptTrusted(new Conn(l));
        }));
        thermalLevel = Thermal.now(ctx);
        PowerManager pm = ctx.getSystemService(PowerManager.class);
        if (pm != null) pm.addThermalStatusListener(ctx.getMainExecutor(), st -> h.post(() -> thermalChanged(st)));
        prefs.registerOnSharedPreferenceChangeListener(prefListener);
        try {
            cm.registerTorchCallback(new CameraManager.TorchCallback() {
                @Override public void onTorchModeUnavailable(String id) { torchAvail.put(id, false); torchOn.put(id, false); }
                @Override public void onTorchModeChanged(String id, boolean on) { torchAvail.put(id, true); torchOn.put(id, on); }
            }, h);
        } catch (RuntimeException ignored) {
        }
        ScreenState.addListener(() -> h.post(this::ctlSchedule));
    }

    // ---- 画面（サービス）から呼ぶ ----

    synchronized void setEnabled(boolean on) {
        if (on == enabled) return;
        enabled = on;
        if (on) wifiBlocked = false;
        usb.setEnabled(on);
        reconfigureTcp();
        if (!on) {
            h.post(() -> {
                if (conn != null) drop(conn);
                for (Conn c : new ArrayList<>(pending)) dropPending(c);
            });
            Logbook.i("ウェブカメラの受け付けを止めました", "Stopped accepting webcam connections");
        }
    }

    synchronized void setWifi(boolean on) {
        if (on == wifi && !(on && wifiBlocked)) return;
        wifi = on;
        wifiBlocked = false;
        h.post(() -> wifiMisses = 0);
        if (on) Logbook.i("ウェブカメラ：Wi-Fi でも受け付けます・接続コード " + wifiCode, "Webcam: also accepting Wi-Fi, connection code " + wifiCode);
        reconfigureTcp();
    }

    synchronized void setLegacy(boolean on) {
        if (on == legacy) return;
        legacy = on;
        reconfigureTcp();
    }

    private synchronized void reconfigureTcp() {
        tcp.configure(enabled, wifi && !wifiBlocked, legacy);
    }

    // 水平を保つで、いま直している角度（度。画面に出す）
    float levelDeg() {
        GlStage g = gl;
        return g == null ? 0 : g.levelShownDeg;
    }

    // 同じ端末のほかのアプリ・古い形の接続を断った時刻（画面に出す）
    long foreignRejectedAt() { return usb.lastRejectAt; }
    long legacyRejectedAt() { return tcp.lastLoopbackRejectAt; }

    // 画面の設定が変わった。続けて変えたときは、最後の 1 回だけ作り直す（iPhone と同じく 0.3 秒まとめる）
    void settingsChanged() {
        h.post(() -> {
            final int g = ++settingsGen;
            h.postDelayed(() -> { if (g == settingsGen) applySettings(); }, 300);
        });
    }

    void shutdown() {
        setEnabled(false);
        prefs.unregisterOnSharedPreferenceChangeListener(prefListener);
        h.post(() -> h.getLooper().quitSafely());
    }

    // ---- つなぎ（h の上）----

    // USB（adb）か、古い PC のツールの接続。すぐ入れ替える
    private void acceptTrusted(Conn c) {
        if (!enabled) { c.close(); return; }
        if (conn != null) drop(conn);
        conn = c;
        c.start();
        Logbook.i("ウェブカメラ：PC が接続しました（" + c.link.desc + "）", "Webcam: the PC connected (" + c.link.desc + ")");
    }

    // Wi-Fi の接続。HELLO のコードを確かめるまでは、今の接続に触らない
    private void acceptPending(Conn c) {
        if (!enabled) { c.close(); return; }
        if (pending.size() >= MAX_PENDING) {
            Logbook.i("ウェブカメラ：Wi-Fi の接続が多すぎるので断りました（" + c.link.desc + "）", "Webcam: refused a Wi-Fi connection because there are too many (" + c.link.desc + ")");
            c.close();
            return;
        }
        pending.add(c);
        c.start();
        h.postDelayed(() -> {
            if (pending.contains(c)) {
                Logbook.i("ウェブカメラ：Wi-Fi の接続から 5 秒たっても HELLO が来ないので切りました（" + c.link.desc + "）", "Webcam: closed a Wi-Fi connection that sent no HELLO within 5 s (" + c.link.desc + ")");
                dropPending(c);
            }
        }, 5000);
    }

    private void dropPending(Conn c) {
        pending.remove(c);
        c.close();
    }

    private void dropAny(Conn c) {
        if (pending.contains(c)) dropPending(c);
        else drop(c);
    }

    private void drop(Conn c) {
        if (c != conn) { c.close(); return; }
        conn = null;
        stopPipeline();
        hello = null;
        cur = null;
        c.close();
        connected = false;
        pcControl = false;
        lowFps = 0;
        ctlReset(false);
        Logbook.i("ウェブカメラ：切れました", "Webcam: disconnected");
    }

    private void handlePending(Conn c, int type, byte[] b) {
        if (type != T_HELLO || b.length < 32) {
            Logbook.i("ウェブカメラ：Wi-Fi の接続が HELLO から始まらないので切りました", "Webcam: closed a Wi-Fi connection that did not start with HELLO");
            dropPending(c);
            return;
        }
        Hello hl = Hello.parse(b);
        if (!hl.hasCode || hl.code != wifiCode) {
            wifiMisses++;
            Logbook.i("ウェブカメラ：Wi-Fi の接続コードが違います（" + wifiMisses + " 回目）。流している接続はそのままです", "Webcam: wrong Wi-Fi connection code (" + wifiMisses + " times). The current stream continues");
            pending.remove(c);
            c.send(ackMsg(6, 0, 0, 0, 0, (int) hl.camera, 0));
            h.postDelayed(c::close, 200);
            if (wifiMisses >= 5) {
                wifiBlocked = true;
                Logbook.i("ウェブカメラ：違うコードが 5 回続いたので、Wi-Fi での受け付けを止めます", "Webcam: 5 wrong codes in a row, so Wi-Fi connections are stopped");
                reconfigureTcp();
            }
            return;
        }
        wifiMisses = 0;
        pending.remove(c);
        if (conn != null) drop(conn);
        conn = c;
        Logbook.i("ウェブカメラ：PC が接続しました（" + c.link.desc + "・接続コードを確かめました）", "Webcam: the PC connected (" + c.link.desc + ", connection code checked)");
        start(c, hl, false);
    }

    private void handle(Conn c, int type, byte[] b) {
        if (pending.contains(c)) { handlePending(c, type, b); return; }
        if (c != conn) return;
        switch (type) {
            case T_HELLO: {
                if (b.length < 32) { Logbook.i("ウェブカメラ：HELLO が短すぎます（" + b.length + " バイト）", "Webcam: HELLO is too short (" + b.length + " bytes)"); return; }
                Hello hl = Hello.parse(b);
                if (c.link.remote && (!hl.hasCode || hl.code != wifiCode)) { Logbook.i("ウェブカメラ：頼み直しの HELLO の接続コードが違うので読み捨てました", "Webcam: ignored a repeated HELLO with a wrong connection code"); return; }
                start(c, hl, cur != null); // 同じつなぎの 2 回目の HELLO は形の頼み直し（PC アプリの set_format も）
                break;
            }
            case T_REQ_KEY:
                requestKey();
                break;
            case T_SET_KBPS:
                if (b.length >= 4) setBitrate(le(b).getInt(0));
                break;
            case T_PING:
                if (b.length >= 8) {
                    ByteBuffer o = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
                    o.put(b, 0, 8).putLong(nowUs());
                    c.send(msg(T_PONG, o.array()));
                }
                break;
            case T_SET:
                if (ctl) ctlSet(b);
                break;
            case T_GET:
                if (ctl) {
                    ctlSendCaps();
                    ctlSendState(true, true, WHY_ALL);
                }
                break;
            default:
                break; // 知らない型は読み捨てる
        }
    }

    // ---- 始める：設定を決める → エンコーダ →（描き手）→ カメラ → ACK → 流す ----

    private void start(Conn c, Hello hl, boolean changed) {
        stopPipeline();
        hello = hl;
        boolean wantCtl = (hl.flags & HF_CTRL) != 0;
        if (wantCtl != ctl) {
            ctlReset(wantCtl);
            if (wantCtl) Logbook.i("ウェブカメラ：PC が CTRL を名乗りました。PC からカメラを操作できます", "Webcam: the PC supports CTRL. The camera can be controlled from the PC");
        }
        Resolved r = resolve(hl);
        if (r.status != 0) { refuse(c, r.status, (int) hl.camera, r.why); return; }
        if (ctx.checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            refuse(c, 1, r.camera, Logbook.t("カメラの使用が許可されていない", "camera access is not allowed"));
            return;
        }
        caps = CamCaps.of(ctx, r.camId, r.m);
        if (!openEncoder(r)) { refuse(c, 4, r.camera, Logbook.t("エンコーダを作れない", "cannot create the encoder")); return; }
        if (r.gl) {
            gl = GlStage.create(ctx, encSurface, r.camW, r.camH, r.outW, r.outH);
            if (gl == null) {
                glFailed = true;
                Logbook.i("ウェブカメラ：回転・反転・切り抜き・水平を使わずに送ります", "Webcam: sending without rotation, flipping, cropping, and keep level");
                start(c, hl, changed); // 描き手なしで決め直す
                return;
            }
            gl.setStage(stageFor(r));
            camTarget = gl.input();
        } else {
            camTarget = encSurface;
        }
        cur = r;
        realtimeClock = r.realtimeClock;
        ackSent = false;
        lockedFocus = null;
        rotSkipped = r.rotSkipped;
        pendingNote = false;
        openCamera(c, r, changed);
    }

    private void refuse(Conn c, int status, int camera, String why) {
        Logbook.i("ウェブカメラ：断りました（" + status + "・" + why + "）", "Webcam: refused (" + status + ", " + why + ")");
        stopPipeline();
        c.send(ackMsg(status, 0, 0, 0, 0, camera, 0));
        h.postDelayed(() -> { if (c == conn) drop(c); }, 200);
    }

    private static final class Hello {
        long version, width, height, fps, kbps, gop, camera, flags, code, quality;
        boolean hasCode, hasQuality;

        static Hello parse(byte[] b) {
            ByteBuffer bb = le(b);
            Hello x = new Hello();
            long[] f = new long[8];
            for (int i = 0; i < 8; i++) f[i] = bb.getInt(i * 4) & 0xffffffffL;
            x.version = f[0]; x.width = f[1]; x.height = f[2]; x.fps = f[3]; x.kbps = f[4]; x.gop = f[5]; x.camera = f[6]; x.flags = f[7];
            if (b.length >= 36) { x.hasCode = true; x.code = bb.getInt(32) & 0xffffffffL; }
            if (b.length >= 40) { x.hasQuality = true; x.quality = bb.getInt(36) & 0xffffffffL; }
            return x;
        }
    }

    // 決まった形
    private static final class Resolved {
        int status;
        String why = "";
        String camId;
        float ratio = 1f, m = 1f;     // 開いたときの倍率・見え方 ÷ カメラの倍率（CamCaps.Lens）
        int camera;
        boolean pcCam;                // PC がカメラを決めた（画面のカメラと違えば、ズームはこのつなぎの間だけ）
        int camW, camH;               // カメラが撮る大きさ
        int outW, outH;               // 送る大きさ（90 度と 270 度は縦長）
        int fps, baseFps, kbps, gop, ackFlags;
        Range<Integer> fpsRange, baseRange;
        boolean realtimeClock;
        boolean gl, portrait, mirror, rotSkipped;
        int rot, stab;

        boolean sameSession(Resolved o) {
            return o != null && camId.equals(o.camId) && camera == o.camera && camW == o.camW && camH == o.camH && outW == o.outW && outH == o.outH
                    && baseFps == o.baseFps && gop == o.gop && gl == o.gl && stab == o.stab;
        }
    }

    private static Resolved fail(int status, String why) {
        Resolved r = new Resolved();
        r.status = status;
        r.why = why;
        return r;
    }

    // PC の頼み（0 はおまかせ）とアプリの画面の設定から、使う形を決める（iPhone の resolve と同じ決まり）
    private Resolved resolve(Hello hl) {
        if (hl.version != VERSION) return fail(5, Logbook.t("約束の版が違う（", "wrong protocol version (") + hl.version + Logbook.t("）", ")"));
        Prefs.Cam s = Prefs.cam(ctx);
        SharedPreferences p = prefs;
        int flags = AF_LIVE;
        boolean pcSize = hl.width != 0 && hl.height != 0;
        boolean pcFps = hl.fps != 0;
        boolean pcCam = (hl.flags & HF_CAMERA_SET) != 0 || hl.camera != 0;
        boolean pcRate = hl.kbps != 0 || (hl.hasQuality && hl.quality != 0);
        boolean pcMirror = (hl.flags & HF_MIRROR) != 0;
        long w, hh, fps, camera, quality = 0, kbps = 0;
        if (s.pcPriority) {
            w = pcSize ? hl.width : s.w;
            hh = pcSize ? hl.height : s.h;
            fps = pcFps ? hl.fps : s.fps;
            camera = pcCam ? hl.camera : s.lens;
            if (hl.kbps != 0) kbps = hl.kbps;
            else quality = hl.hasQuality && hl.quality != 0 ? hl.quality : s.quality;
            if (!pcSize || !pcFps || !pcCam || !pcRate) flags |= AF_APP_SETTINGS;
        } else {
            w = s.w; hh = s.h; fps = s.fps; camera = s.lens; quality = s.quality;
            flags |= AF_APP_SETTINGS;
            if (pcSize || pcFps || pcCam || pcRate || pcMirror) flags |= AF_APP_PRIORITY;
        }
        if (w < 64 || hh < 64 || w > 4096 || hh > 3072 || fps < 1 || fps > 120 || camera > 3) return fail(5, Logbook.t("頼みが変（", "bad request (") + w + "x" + hh + ", " + fps + ", camera " + camera + Logbook.t("）", ")"));
        CamCaps.Lens lens = CamCaps.pick(ctx, (int) camera);
        if (lens.status != 0) return fail(lens.status, lens.why);
        Resolved r = new Resolved();
        r.camId = lens.camId;
        r.ratio = lens.ratio;
        r.m = lens.m;
        r.realtimeClock = lens.realtimeClock;
        r.camera = (int) camera;
        r.pcCam = s.pcPriority && pcCam;
        CamCaps c = CamCaps.of(ctx, r.camId, r.m);
        // 左右反転：PC が mirror を立てたら「PC が決めた」。立っていなければおまかせで、画面の設定を使う
        boolean useMirror = pcMirror && s.pcPriority;
        boolean appMirror = Prefs.b(p, Prefs.MIRROR, false);
        if (!useMirror && appMirror) flags |= AF_APP_SETTINGS;
        // 形はいつも横長で選ぶ（カメラの形は横長）。縦長にするのは回転
        if (hh > w) { long x = w; w = hh; hh = x; }
        int want = Prefs.i(p, Prefs.ROTATION, 0);
        int rot = want == 90 || want == 180 || want == 270 ? want : 0;
        boolean flipV = Prefs.b(p, Prefs.FLIP_V, false);
        // 古い webcam_rx（WC_HELLO_LIVE が無い）は縦長の絵を仮想カメラに書かないので、90 度と 270 度は使わない
        if (rot % 180 != 0 && (hl.flags & HF_LIVE) == 0) { rot = 0; r.rotSkipped = true; }
        if (rot != 0 || flipV) flags |= AF_APP_SETTINGS;
        boolean cropOn = Prefs.b(p, Prefs.CROP, false);
        boolean levelOn = Prefs.b(p, Prefs.LEVEL, false) && c.level;
        // 回す・反転・切り抜き・水平は、描き手（GlStage）を通す。作れなかった端末では使わない
        boolean mirror = useMirror || appMirror;
        r.gl = (rot != 0 || flipV || mirror || cropOn || levelOn) && !glFailed;
        r.portrait = r.gl && rot % 180 != 0;
        r.rot = r.gl ? (flipV ? (rot + 180) % 360 : rot) : 0;
        r.mirror = r.gl && (mirror != flipV);
        // 縦長の 4K は送らない（Windows の H.264 のデコーダの資料は 4096×2304 まで）。1080 の高さに縮める
        if (r.portrait && hh > 1080) { w = (w * 1080 / hh) & ~1; hh = 1080; }
        int stab = Prefs.i(p, Prefs.STAB, 0);
        r.stab = contains(c.stabModes, stab) ? stab : 0;
        r.ackFlags = flags;
        if (!pickShape(r, (int) w, (int) hh, (int) fps)) return fail(2, Logbook.t("カメラの形を選べない", "cannot choose the camera format"));
        if (r.portrait) {
            if (!avcSupports(r.camH, r.camW) && r.camW * r.camH > 1280 * 720) {
                Logbook.i("ウェブカメラ：エンコーダが縦長の " + r.camH + "x" + r.camW + " に対応しないので、720p で撮ります", "Webcam: the encoder does not support portrait " + r.camH + "x" + r.camW + ", so capturing at 720p");
                if (!pickShape(r, 1280, 720, (int) fps)) return fail(2, Logbook.t("カメラの形を選べない", "cannot choose the camera format"));
            }
            if (!avcSupports(r.camH, r.camW)) {
                Logbook.i("ウェブカメラ：エンコーダが縦長に対応しないので、回転を使いません", "Webcam: the encoder does not support portrait, so rotation is not used");
                r.portrait = false;
                r.rot = flipV ? 180 : 0;
                r.gl = r.rot != 0 || r.mirror || cropOn || levelOn;
            }
        }
        r.outW = r.portrait ? r.camH : r.camW;
        r.outH = r.portrait ? r.camW : r.camH;
        // 切り抜きと水平：大きめに撮って、送る大きさに縮める（拡大が 1.25 倍までに収まる、いちばん小さい大きさ）
        if (r.gl) {
            float sc = stageScale(cropOn ? Prefs.cropSize(p) : 1f, levelOn, r.outW, r.outH);
            if (sc > 1.001f) pickCapture(r, sc);
        }
        r.baseFps = r.fps;
        r.baseRange = r.fpsRange;
        // 熱いときは、コマ数を下げて始める（設定で切れる）。下げたあとに冷えたら applyThermal が戻す
        int hot = thermalFps(r.fps);
        if (hot < r.fps) {
            Range<Integer> rr = pickRange(r.camId, hot, true);
            if (rr != null) {
                r.fps = Math.min(hot, rr.getUpper());
                r.fpsRange = rr;
            }
        }
        if (kbps == 0) {
            double mult = quality >= 3 ? 2.5 : quality == 2 ? 1.5 : 1.0;
            kbps = (long) (defaultKbps(r.outW, r.outH, r.baseFps) * mult);
        }
        r.kbps = (int) Math.max(200, Math.min(100_000, kbps));
        r.gop = hl.gop > 0 ? (int) Math.min(hl.gop, 600) : r.baseFps * 2;
        return r;
    }

    private static boolean contains(int[] a, int v) {
        for (int x : a) if (x == v) return true;
        return false;
    }

    // iPhone と同じ目安。高画質は 1.5 倍・最高は 2.5 倍
    private static int defaultKbps(int w, int h, int fps) {
        boolean big = w * h >= 1920 * 1080;
        return big ? (fps >= 60 ? 12000 : 8000) : (fps >= 60 ? 6000 : 4000);
    }

    // 熱いときのコマ数。高い（2）は 60 を 30 に・30 を 24 に、危険（3）は 15。設定を切っているときはそのまま（iPhone と同じ）
    private int thermalFps(int fps) {
        if (!Prefs.thermalFps(prefs)) return fps;
        if (thermalLevel >= 3) return Math.min(fps, 15);
        if (thermalLevel == 2) return Math.min(fps, fps > 30 ? 30 : 24);
        return fps;
    }

    // 水平の余白に要る拡大：枠を θ 回して、同じ縦横比の元の絵に収めるには cos θ ＋ 長辺/短辺 × sin θ 倍（iPhone と同じ）
    static float levelScale(int w, int h) {
        double a = Math.toRadians(LEVEL_MAX_DEG);
        double r = Math.max(w, h) / (double) Math.max(1, Math.min(w, h));
        return (float) (Math.cos(a) + r * Math.sin(a));
    }

    static float stageScale(float cropSize, boolean level, int w, int h) {
        float c = 1f / Math.min(Math.max(cropSize, 0.3f), 1f);
        return Math.max(c, level ? levelScale(w, h) : 1f);
    }

    // 大きさ：ぴったりで、そのフレームレートを出せる物。無ければ、出せる中で面積がいちばん近い物。
    // フレームレート：AE の範囲の [fps, fps] を選ぶ。無ければ上が fps の物、それも無ければ fps を含んで上がいちばん小さい物
    private boolean pickShape(Resolved r, int w, int h, int fps) {
        try {
            CameraCharacteristics ch = cm.getCameraCharacteristics(r.camId);
            StreamConfigurationMap map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            if (map == null) return false;
            Class<?> cls = r.gl ? android.graphics.SurfaceTexture.class : MediaCodec.class;
            Size[] sizes = map.getOutputSizes(cls);
            if (sizes == null || sizes.length == 0) { cls = android.graphics.SurfaceTexture.class; sizes = map.getOutputSizes(cls); }
            if (sizes == null || sizes.length == 0) return false;
            Size best = null;
            long bestD = Long.MAX_VALUE;
            for (int pass = 0; pass < 2 && best == null; pass++) {
                for (Size s : sizes) {
                    if (s.getWidth() > 4096 || s.getHeight() > 3072 || (s.getWidth() & 1) != 0 || (s.getHeight() & 1) != 0) continue;
                    long minFd = map.getOutputMinFrameDuration(cls, s);
                    double maxFps = minFd > 0 ? 1e9 / minFd : 1000;
                    if (pass == 0 && maxFps + 0.5 < fps) continue;
                    long d = Math.abs((long) s.getWidth() * s.getHeight() - (long) w * h);
                    if (s.getWidth() == w && s.getHeight() == h) d = -1;
                    if (d < bestD) { bestD = d; best = s; }
                }
            }
            if (best == null) return false;
            r.camW = best.getWidth();
            r.camH = best.getHeight();
            long minFd = map.getOutputMinFrameDuration(cls, best);
            int maxFps = minFd > 0 ? (int) Math.floor(1e9 / minFd + 0.5) : fps;
            int want = Math.min(fps, Math.max(1, maxFps));
            Range<Integer> pick = pickRange(r.camId, want, false);
            r.fpsRange = pick;
            r.fps = pick == null ? want : Math.min(want, pick.getUpper());
            return true;
        } catch (CameraAccessException | RuntimeException e) {
            Logbook.i("ウェブカメラ：形を選べません：" + e, "Webcam: cannot choose the camera format: " + e);
            return false;
        }
    }

    // 切り抜くときに撮る大きさ：同じ縦横比で、拡大が 1.25 倍までに収まる、いちばん小さい物（撮れなければ今のまま）。4K まで
    private void pickCapture(Resolved r, float scale) {
        try {
            StreamConfigurationMap map = cm.getCameraCharacteristics(r.camId).get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            Size[] sizes = map == null ? null : map.getOutputSizes(android.graphics.SurfaceTexture.class);
            if (sizes == null) return;
            double aspect = r.camW / (double) r.camH, need = r.camH * scale / 1.25;
            Size best = null, biggest = null;
            for (Size s : sizes) {
                if (Math.abs(s.getWidth() / (double) s.getHeight() - aspect) > 0.02) continue;
                if (s.getWidth() < r.camW || s.getWidth() > 3840 || s.getHeight() > 2160) continue;
                long minFd = map.getOutputMinFrameDuration(android.graphics.SurfaceTexture.class, s);
                if (minFd > 0 && 1e9 / minFd + 0.5 < r.fps) continue;
                if (biggest == null || s.getHeight() > biggest.getHeight()) biggest = s;
                if (s.getHeight() >= need && (best == null || s.getHeight() < best.getHeight())) best = s;
            }
            Size pick = best != null ? best : biggest;
            if (pick != null && pick.getHeight() > r.camH) {
                r.camW = pick.getWidth();
                r.camH = pick.getHeight();
            }
        } catch (CameraAccessException | RuntimeException ignored) {
        }
    }

    // AE の範囲を選ぶ。strict なら上が want 以下の物だけ（熱でコマ数を下げるとき）
    private Range<Integer> pickRange(String camId, int want, boolean strict) {
        Range<Integer>[] ranges;
        try {
            ranges = cm.getCameraCharacteristics(camId).get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        } catch (CameraAccessException | RuntimeException e) {
            return null;
        }
        if (ranges == null) return null;
        Range<Integer> pick = null;
        for (Range<Integer> q : ranges) if (q.getLower() == want && q.getUpper() == want) pick = q;
        if (pick == null) for (Range<Integer> q : ranges) if (q.getUpper() == want && (pick == null || q.getLower() > pick.getLower())) pick = q;
        if (pick == null && !strict) for (Range<Integer> q : ranges) if (q.contains(want) && (pick == null || q.getUpper() < pick.getUpper())) pick = q;
        if (pick == null) for (Range<Integer> q : ranges) if (q.getUpper() <= want && (pick == null || q.getUpper() > pick.getUpper())) pick = q;
        return pick;
    }

    // ---- エンコーダ（MediaCodec の H.264・Surface 入力・B フレームなし）----

    // その大きさに対応する H.264 のエンコーダがあるか
    private static boolean avcSupports(int w, int h) {
        try {
            for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
                if (!info.isEncoder()) continue;
                for (String t : info.getSupportedTypes()) {
                    if (!t.equalsIgnoreCase(MediaFormat.MIMETYPE_VIDEO_AVC)) continue;
                    MediaCodecInfo.VideoCapabilities vc = info.getCapabilitiesForType(t).getVideoCapabilities();
                    if (vc == null || vc.isSizeSupported(w, h)) return true;
                }
            }
        } catch (RuntimeException ignored) {
        }
        return false;
    }

    // いつもの H.264 のエンコーダ（createEncoderByType が選ぶ物）。その大きさに対応しなければ、対応するほかのエンコーダ
    private static MediaCodec createAvc(int w, int h) throws IOException {
        MediaCodec c = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
        MediaCodecInfo.VideoCapabilities vc = c.getCodecInfo().getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC).getVideoCapabilities();
        if (vc == null || vc.isSizeSupported(w, h)) return c;
        String first = c.getName();
        c.release();
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (!info.isEncoder() || info.getName().equals(first)) continue;
            for (String t : info.getSupportedTypes()) {
                if (!t.equalsIgnoreCase(MediaFormat.MIMETYPE_VIDEO_AVC)) continue;
                MediaCodecInfo.VideoCapabilities v2 = info.getCapabilitiesForType(t).getVideoCapabilities();
                if (v2 == null || v2.isSizeSupported(w, h)) return MediaCodec.createByCodecName(info.getName());
            }
        }
        Logbook.i("ウェブカメラ：" + w + "x" + h + " に対応する H.264 のエンコーダがありません", "Webcam: no H.264 encoder supports " + w + "x" + h);
        return null;
    }

    private boolean openEncoder(Resolved r) {
        MediaCodec c = null;
        try {
            c = createAvc(r.outW, r.outH);
            if (c == null) return false;
            MediaCodecInfo.CodecCapabilities caps = c.getCodecInfo().getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC);
            MediaFormat f = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, r.outW, r.outH);
            f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            f.setInteger(MediaFormat.KEY_BIT_RATE, r.kbps * 1000);
            f.setInteger(MediaFormat.KEY_FRAME_RATE, r.baseFps);
            f.setFloat(MediaFormat.KEY_I_FRAME_INTERVAL, r.gop / (float) r.baseFps);
            f.setInteger(MediaFormat.KEY_PRIORITY, 0);   // 実時間
            f.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);
            f.setInteger("latency", 1);                  // KEY_LATENCY。対応しない機種は読まない
            MediaCodecInfo.EncoderCapabilities ec = caps.getEncoderCapabilities();
            if (ec != null && ec.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR))
                f.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR);
            final int gen = ++generation;
            c.setCallback(new MediaCodec.Callback() {
                @Override public void onInputBufferAvailable(MediaCodec mc, int i) {}
                @Override public void onOutputBufferAvailable(MediaCodec mc, int i, MediaCodec.BufferInfo info) { encoded(gen, mc, i, info); }
                @Override public void onError(MediaCodec mc, MediaCodec.CodecException e) {
                    if (gen != generation) return;
                    Logbook.i("ウェブカメラ：エンコーダの失敗 " + e.getDiagnosticInfo(), "Webcam: encoder error " + e.getDiagnosticInfo());
                    restartQuietly();
                }
                @Override public void onOutputFormatChanged(MediaCodec mc, MediaFormat of) {
                    if (gen != generation) return;
                    ByteBuffer a = of.getByteBuffer("csd-0"), b = of.getByteBuffer("csd-1");
                    if (a != null) {
                        byte[] x = new byte[a.remaining() + (b != null ? b.remaining() : 0)];
                        a.get(x, 0, a.remaining());
                        if (b != null) b.get(x, x.length - b.remaining(), b.remaining());
                        csd = x;
                    }
                }
            }, h);
            c.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            encSurface = c.createInputSurface();
            c.start();
            enc = c;
            csd = null;
            needKey = true;
            seq = 0;
            sentFrames = 0;
            Logbook.i("ウェブカメラ：エンコーダ " + c.getName() + "（" + r.outW + "x" + r.outH + "・" + r.baseFps + "・" + r.kbps + " kbps）", "Webcam: encoder " + c.getName() + " (" + r.outW + "x" + r.outH + ", " + r.baseFps + " fps, " + r.kbps + " kbps)");
            return true;
        } catch (IOException | RuntimeException e) {
            Logbook.i("ウェブカメラ：エンコーダを作れません：" + e, "Webcam: cannot create the encoder: " + e);
            if (c != null) try { c.release(); } catch (RuntimeException ignored) {}
            if (encSurface != null) { encSurface.release(); encSurface = null; }
            return false;
        }
    }

    // できた 1 コマ（h の上）。MediaCodec の H.264 は最初から Annex B（00 00 00 01 ＋ NAL）
    private void encoded(int gen, MediaCodec mc, int idx, MediaCodec.BufferInfo info) {
        byte[] data = null;
        try {
            if (gen == generation && info.size > 0) {
                ByteBuffer b = mc.getOutputBuffer(idx);
                if (b != null) {
                    b.position(info.offset);
                    b.limit(info.offset + info.size);
                    data = new byte[info.size];
                    b.get(data);
                }
            }
            mc.releaseOutputBuffer(idx, false);
        } catch (RuntimeException e) {
            return; // 止めた直後
        }
        if (data == null || gen != generation) return;
        if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) { csd = data; return; }
        Conn c = conn;
        Resolved r = cur;
        if (c == null || r == null || !ackSent) return;
        boolean key = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
        int n = seq++;
        if (needKey && !key) return; // 参照が切れている。キーフレームを待つ
        if (c.frames.get() >= MAX_INFLIGHT) { // PC が受け切れていない。捨てて、次をキーフレームにする
            needKey = true;
            requestKey();
            return;
        }
        if (key) needKey = false;
        if (n == 0) Logbook.i("ウェブカメラ：最初のコマ・撮った時刻からエンコードが終わるまで " + (nowUs() - info.presentationTimeUs) / 1000 + " ms", "Webcam: first frame, " + (nowUs() - info.presentationTimeUs) / 1000 + " ms from capture to encoded");
        byte[] pre = key && csd != null && !hasSps(data) ? csd : null; // キーフレームには SPS と PPS を付ける
        int plen = (pre != null ? pre.length : 0) + data.length;
        ByteBuffer o = ByteBuffer.allocate(8 + 32 + plen).order(ByteOrder.LITTLE_ENDIAN);
        o.putInt(T_FRAME).putInt(32 + plen);
        o.putLong(info.presentationTimeUs).putLong(0); // 送る時刻は送る糸が書く
        o.putInt(n).putInt(key ? 1 : 0).putInt(r.outW).putInt(r.outH);
        if (pre != null) o.put(pre);
        o.put(data);
        c.sendFrame(o.array());
    }

    private static boolean hasSps(byte[] d) {
        for (int i = 0; i + 4 < d.length && i < 64; i++) {
            if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] & 0x1f) == 7) return true;
        }
        return false;
    }

    private void requestKey() {
        if (enc == null) return;
        try {
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
            enc.setParameters(b);
        } catch (RuntimeException ignored) {}
    }

    private void setBitrate(int kbps) {
        if (kbps < 200 || kbps > 100_000 || enc == null || cur == null) return;
        try {
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, kbps * 1000);
            enc.setParameters(b);
            cur.kbps = kbps;
            Logbook.i("ウェブカメラ：ビットレートを " + kbps + " kbps に変えました", "Webcam: bitrate changed to " + kbps + " kbps");
        } catch (RuntimeException ignored) {}
    }

    // ---- カメラ（Camera2）----

    private void openCamera(Conn c, Resolved r, boolean changed) {
        final int gen = generation;
        try {
            cm.openCamera(r.camId, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice d) {
                    if (gen != generation) { d.close(); return; }
                    device = d;
                    createSession(c, r, changed, gen);
                }
                @Override public void onDisconnected(CameraDevice d) {
                    d.close();
                    if (gen != generation) return;
                    if (device == d) device = null;
                    cameraLost(c, Logbook.t("ほかのアプリがカメラを使い始めた", "another app started using the camera"));
                }
                @Override public void onError(CameraDevice d, int err) {
                    d.close();
                    if (gen != generation) return;
                    if (device == d) device = null;
                    if (!ackSent) refuse(c, err == ERROR_CAMERA_DISABLED ? 3 : 2, r.camera, Logbook.t("カメラを開けない（", "cannot open the camera (") + err + Logbook.t("）", ")"));
                    else cameraLost(c, Logbook.t("カメラの失敗 ", "camera error ") + err);
                }
            }, h);
        } catch (CameraAccessException | SecurityException | IllegalArgumentException e) {
            if (!ackSent) refuse(c, 2, r.camera, Logbook.t("カメラを開けない：", "cannot open the camera: ") + e.getMessage());
            else cameraLost(c, Logbook.t("開き直せない：", "cannot reopen: ") + e.getMessage());
        }
    }

    private void createSession(Conn c, Resolved r, boolean changed, int gen) {
        try {
            List<OutputConfiguration> outs = Collections.singletonList(new OutputConfiguration(camTarget));
            device.createCaptureSession(new SessionConfiguration(SessionConfiguration.SESSION_REGULAR, outs, h::post, new CameraCaptureSession.StateCallback() {
                @Override public void onConfigured(CameraCaptureSession s) {
                    if (gen != generation) { s.close(); return; }
                    session = s;
                    try {
                        req = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
                        req.addTarget(camTarget);
                        repeat(true);
                    } catch (CameraAccessException | RuntimeException e) {
                        if (!ackSent) refuse(c, 2, r.camera, Logbook.t("撮り始められない：", "cannot start capturing: ") + e.getMessage());
                        else cameraLost(c, Logbook.t("撮り直せない：", "cannot restart capturing: ") + e.getMessage());
                        return;
                    }
                    applyMgrTorch();
                    if (!ackSent) {
                        ackSent = true;
                        boolean hot = r.fps < r.baseFps;
                        int flags = r.ackFlags | (changed ? AF_CHANGED : 0) | (ctl ? AF_CTRL | (hot ? AF_THERMAL : 0) : 0);
                        c.send(ackMsg(0, r.outW, r.outH, r.fps, r.kbps, r.camera, flags));
                        // 温度の状態は変わったときに EVENT 3 で送っている。つないだ直後にも今の状態を 1 回送る（古い PC は記録に出すだけ）
                        if (!changed) sendEvent(c, 3, thermalLevel);
                        connected = true;
                        cameraStopped = false;
                        curW = r.outW;
                        curH = r.outH;
                        curCamera = r.camera;
                        curFps = r.fps;
                        lowFps = hot ? r.fps : 0;
                        pcControl = ctl;
                        Logbook.i("ウェブカメラ：始めます " + r.outW + "x" + r.outH + "・毎秒 " + r.fps + " コマ・" + r.kbps + " kbps・カメラ " + r.camera
                                + "（id " + r.camId + (r.gl ? "・撮る " + r.camW + "x" + r.camH + "・回転 " + r.rot + "・反転 " + (r.mirror ? 1 : 0) : "")
                                + "・手ぶれ補正 " + r.stab + "）" + (changed ? "・途中の切り替え" : "") + "・ACK flags " + flags + (hot ? "・熱でコマ数を下げた" : ""), "Webcam: starting " + r.outW + "x" + r.outH + ", " + r.fps + " fps, " + r.kbps + " kbps, camera " + r.camera + " (id " + r.camId + (r.gl ? ", capture " + r.camW + "x" + r.camH + ", rotation " + r.rot + ", mirror " + (r.mirror ? 1 : 0) : "") + ", stabilization " + r.stab + ")" + (changed ? ", switched while streaming" : "") + ", ACK flags " + flags + (hot ? ", frame rate lowered for heat" : ""));
                        // CTRL の PC には、熱で下げて始めたことも EVENT 5 で知らせ、操作の一覧（CAPS）と今の値（STATE）を送る
                        if (ctl) {
                            if (hot) sendEvent(c, 5, r.fps);
                            ctlStart();
                        }
                    } else {
                        cameraStopped = false;
                        needKey = true;
                        requestKey();
                        sendEvent(c, 2, 0);
                        Logbook.i("ウェブカメラ：カメラが戻りました", "Webcam: the camera is back");
                    }
                }
                @Override public void onConfigureFailed(CameraCaptureSession s) {
                    if (gen != generation) return;
                    if (!ackSent) refuse(c, 2, r.camera, Logbook.t("カメラの出力を作れない", "cannot create the camera output"));
                    else cameraLost(c, Logbook.t("カメラの出力を作り直せない", "cannot recreate the camera output"));
                }
            }));
        } catch (CameraAccessException | RuntimeException e) {
            if (!ackSent) refuse(c, 2, r.camera, Logbook.t("カメラの出力を作れない：", "cannot create the camera output: ") + e.getMessage());
            else cameraLost(c, Logbook.t("カメラの出力を作り直せない：", "cannot recreate the camera output: ") + e.getMessage());
        }
    }

    // 今の操作で撮り続ける。当てられない値があったら、操作を当てずに撮る（映像を止めない）
    private void repeat(boolean first) throws CameraAccessException {
        try {
            applyControls(req, cur);
            session.setRepeatingRequest(req.build(), resultCb, h);
        } catch (IllegalArgumentException e) {
            Logbook.i("ウェブカメラ：カメラの操作を当てられません（" + e.getMessage() + "）。操作なしで撮ります", "Webcam: cannot apply the camera controls (" + e.getMessage() + "). Capturing without them");
            req = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
            req.addTarget(camTarget);
            if (cur.fpsRange != null) req.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, cur.fpsRange);
            session.setRepeatingRequest(req.build(), resultCb, h);
        }
    }

    // 流している途中でカメラが止まった。PC に知らせて、2 秒ごとに開き直す（エンコーダとつなぎはそのまま）
    private void cameraLost(Conn c, String why) {
        if (c != conn) return;
        if (!cameraStopped) {
            Logbook.i("ウェブカメラ：カメラが止まりました（" + why + "）", "Webcam: the camera stopped (" + why + ")");
            sendEvent(c, 1, 0);
        }
        cameraStopped = true;
        closeCamera();
        h.removeCallbacks(retryCamera);
        h.postDelayed(retryCamera, 2000);
    }

    private void reopenCamera() {
        if (conn == null || cur == null || enc == null) return;
        openCamera(conn, cur, false);
    }

    // エンコーダが壊れた：同じ形のまま全部作り直す（形は変わらないので ACK は送らない）
    private void restartQuietly() {
        Conn c = conn;
        Resolved r = cur;
        if (c == null || r == null) return;
        stopPipeline();
        if (!openEncoder(r)) { refuse(c, 4, r.camera, Logbook.t("エンコーダを作り直せない", "cannot recreate the encoder")); return; }
        if (r.gl) {
            gl = GlStage.create(ctx, encSurface, r.camW, r.camH, r.outW, r.outH);
            if (gl == null) { glFailed = true; refuse(c, 4, r.camera, Logbook.t("描き手を作り直せない", "cannot recreate the renderer")); return; }
            gl.setStage(stageFor(r));
            camTarget = gl.input();
        } else {
            camTarget = encSurface;
        }
        cur = r;
        ackSent = true;
        openCamera(c, r, false);
    }

    private void closeCamera() {
        if (session != null) { try { session.close(); } catch (RuntimeException ignored) {} session = null; }
        if (device != null) { try { device.close(); } catch (RuntimeException ignored) {} device = null; }
        req = null;
    }

    private void stopPipeline() {
        generation++;
        h.removeCallbacks(retryCamera);
        closeCamera();
        if (gl != null) { gl.release(); gl = null; }
        camTarget = null;
        if (enc != null) {
            try { enc.stop(); } catch (RuntimeException ignored) {}
            try { enc.release(); } catch (RuntimeException ignored) {}
            enc = null;
        }
        if (encSurface != null) { encSurface.release(); encSurface = null; }
        mgrTorchOff();
        cameraStopped = false;
        ackSent = false;
        torchFired = false;
    }

    // ---- 画面の設定が変わった（h の上）----

    // 鍵ごとに：すぐ効く物はその場で、作り直すかもしれない物は 0.3 秒まとめてから。どれも PC に今の値を知らせる
    private static final HashSet<String> LIVE_KEYS = new HashSet<>(Arrays.asList(Prefs.FOCUS, Prefs.FOCUS_POS, Prefs.FOCUS_TARGET,
            Prefs.EXPOSURE, Prefs.BIAS, Prefs.ISO, Prefs.SHUTTER, Prefs.WB, Prefs.WB_TEMP, Prefs.ZOOM, Prefs.TORCH, Prefs.TORCH_LEVEL,
            Prefs.CENTER_STAGE, Prefs.CROP_POS, Prefs.CROP_X, Prefs.CROP_Y));
    private static final HashSet<String> REBUILD_KEYS = new HashSet<>(Arrays.asList(Prefs.STAB, Prefs.MIRROR, Prefs.FLIP_V, Prefs.ROTATION,
            Prefs.CROP, Prefs.CROP_SIZE, Prefs.LEVEL, Prefs.LENS, Prefs.RES, Prefs.FPS, Prefs.QUALITY, Prefs.PC_PRIORITY));

    private void prefChanged(String k) {
        if (LIVE_KEYS.contains(k)) requestLive();
        else if (REBUILD_KEYS.contains(k)) settingsChanged();
        else if (Prefs.THERMAL_FPS.equals(k)) applyThermal();
        ctlSchedule();
    }

    private void requestLive() {
        if (livePosted) return;
        livePosted = true;
        h.post(() -> { livePosted = false; liveApply(); });
    }

    // 作り直さずに、今のカメラへ当てる（ピント・明るさ・色・ズーム・ライト・切り抜きの位置・水平）
    private void liveApply() {
        if (gl != null && cur != null) gl.setStage(stageFor(cur));
        if (session != null && req != null && cur != null) {
            try {
                repeat(false);
            } catch (CameraAccessException | RuntimeException e) {
                Logbook.i("ウェブカメラ：カメラの設定を変えられません：" + e.getMessage(), "Webcam: cannot change the camera settings: " + e.getMessage());
            }
        }
        applyMgrTorch();
    }

    private void applySettings() {
        Conn c = conn;
        if (c == null || hello == null || cur == null) return; // つないでいなければ、次の HELLO のときに使う
        Resolved r = resolve(hello);
        if (r.status != 0) return;
        if (r.sameSession(cur)) { // 作り直さなくてよい変更（回転の 0 度と 180 度・反転・切り抜きの位置と大きさなど）
            if (r.kbps != cur.kbps) setBitrate(r.kbps);
            cur.rot = r.rot;
            cur.mirror = r.mirror;
            cur.portrait = r.portrait;
            cur.rotSkipped = r.rotSkipped;
            rotSkipped = r.rotSkipped;
            liveApply();
            if (ctl) { ctlSendCaps(); ctlSendState(true, true, WHY_ALL); }
            return;
        }
        boolean sameSize = r.outW == cur.outW && r.outH == cur.outH && r.fps == cur.fps;
        if ((hello.flags & HF_LIVE) != 0 || sameSize) {
            Logbook.i("ウェブカメラ：設定が変わったので、カメラとエンコーダを作り直します", "Webcam: settings changed, recreating the camera and encoder");
            start(c, hello, true);
        } else {
            Logbook.i("ウェブカメラ：PC のツールが途中の切り替えに対応していないので、次の接続から新しい設定を使います", "Webcam: the PC tool cannot switch while streaming, so the new settings apply from the next connection");
            pendingNote = true;
            liveApply();
        }
    }

    // ---- 操作を当てる（h の上。iPhone の liveApply と同じ中身を Camera2 の値で）----

    private GlStage.Stage stageFor(Resolved r) {
        SharedPreferences p = prefs;
        GlStage.Stage s = new GlStage.Stage();
        s.rot = r.rot;
        s.mirror = r.mirror;
        boolean cropOn = Prefs.b(p, Prefs.CROP, false);
        float c = cropOn ? 1f / Prefs.cropSize(p) : 1f;
        s.level = Prefs.b(p, Prefs.LEVEL, false) && caps != null && caps.level;
        s.scale = Math.max(c, s.level ? levelScale(r.outW, r.outH) : 1f);
        int pos = cropOn ? Math.max(0, Math.min(4, Prefs.i(p, Prefs.CROP_POS, 0))) : 0;
        if (pos == 3 && (caps == null || !caps.face)) pos = 0;
        s.pos = pos;
        s.cx = Math.max(0, Math.min(1, Prefs.cropX(p)));
        s.cy = Math.max(0, Math.min(1, Prefs.cropY(p)));
        s.levelMax = (float) Math.toRadians(LEVEL_MAX_DEG);
        // 水平のときの傾き：センサーの向きの分だけ時計回りに回した端末で、撮った絵がまっすぐ。回す角度ぶん進む
        s.ref = (float) Math.toRadians((caps == null ? 90 : caps.sensorOrientation) + r.rot);
        s.sign = r.mirror ? -1f : 1f;
        return s;
    }

    // 切り抜きと水平のために、カメラの倍率を下げる分（描き手で拡大する）
    private float stageExtra() {
        if (gl == null || cur == null) return 1f;
        boolean cropOn = Prefs.b(prefs, Prefs.CROP, false);
        float c = cropOn ? 1f / Prefs.cropSize(prefs) : 1f;
        return stageFor(cur).scale / c;
    }

    // 倍率を画面の値（Prefs.ZOOM）で決めるか。PC がカメラを決めて画面のカメラと違うときは、画面の倍率は別のレンズの物なので使わない
    private boolean zoomStored(Resolved r) {
        return !r.pcCam || r.camera == Prefs.i(prefs, Prefs.LENS, 0);
    }

    private float zoomView(Resolved r) {
        float base = r.ratio * r.m;
        float z;
        if (zoomStored(r)) z = prefs.contains(Prefs.ZOOM) ? Prefs.f(prefs, Prefs.ZOOM, base) : base;
        else z = ctlZoom != null ? ctlZoom : base;
        if (caps != null) z = Math.max(caps.zoomMin, Math.min(caps.zoomMax, z));
        return z;
    }

    private void applyControls(CaptureRequest.Builder b, Resolved r) {
        SharedPreferences p = prefs;
        CamCaps c = caps;
        if (r.fpsRange != null) b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, r.fpsRange);
        if (c == null) return;
        // ピント：0 自動（連続）・1 固定（今のレンズの位置）・2 手動（レンズの位置）。合わせる所は 0 顔を優先・1 画面の真ん中
        int fm = Prefs.i(p, Prefs.FOCUS, 0);
        int target = Prefs.i(p, Prefs.FOCUS_TARGET, 0) == 1 ? 1 : 0;
        boolean autoFocus = false;
        if (fm == 2 && c.focusManual) {
            lockedFocus = null;
            b.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_OFF);
            float pos = Math.max(0, Math.min(1, Prefs.focusPos(p)));
            b.set(CaptureRequest.LENS_FOCUS_DISTANCE, c.minFocus * (1 - pos)); // 0 は無限遠（遠く）・minFocus がいちばん近く
        } else if (fm == 1 && c.focusLock) {
            if (lockedFocus == null) lockedFocus = lastFocus != null ? lastFocus : 0f;
            b.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_OFF);
            b.set(CaptureRequest.LENS_FOCUS_DISTANCE, lockedFocus);
        } else if (c.focusAuto) {
            lockedFocus = null;
            autoFocus = true;
            b.set(CaptureRequest.CONTROL_AF_MODE, c.afAuto);
            if (target == 1 && c.centerAf) {
                Rect a = c.active;
                int w = a.width() / 4, hh = a.height() / 4;
                b.set(CaptureRequest.CONTROL_AF_REGIONS, new MeteringRectangle[] {
                        new MeteringRectangle(a.width() / 2 - w / 2, a.height() / 2 - hh / 2, w, hh, MeteringRectangle.METERING_WEIGHT_MAX) });
            } else {
                b.set(CaptureRequest.CONTROL_AF_REGIONS, null);
            }
        }
        boolean faceFirst = autoFocus && target == 0 && c.faceAf;
        b.set(CaptureRequest.CONTROL_MODE, faceFirst ? CameraMetadata.CONTROL_MODE_USE_SCENE_MODE : CameraMetadata.CONTROL_MODE_AUTO);
        b.set(CaptureRequest.CONTROL_SCENE_MODE, faceFirst ? CameraMetadata.CONTROL_SCENE_MODE_FACE_PRIORITY : CameraMetadata.CONTROL_SCENE_MODE_DISABLED);
        // 明るさ：0 自動・1 固定（今の明るさ）・2 手動（ISO とシャッター速度）。露出補正は自動と固定で効く
        int em = Prefs.i(p, Prefs.EXPOSURE, 0);
        if (em == 2 && c.expManual) {
            long frameNs = 1_000_000_000L / Math.max(1, r.fps);
            int den = Prefs.i(p, Prefs.SHUTTER, 0);
            if (den <= 0) den = Math.max(r.fps, 30) * 2;
            long exp = Math.max(c.expMinNs, Math.min(Math.min(c.expMaxNs, frameNs), 1_000_000_000L / den));
            int iso = Math.round(Prefs.f(p, Prefs.ISO, 0));
            if (iso <= 0) iso = lastIso != null ? lastIso : (int) Math.sqrt((double) Math.max(1, c.isoMin) * Math.max(1, c.isoMax)); // 初めは今の値から
            iso = Math.max(c.isoMin, Math.min(c.isoMax, iso));
            b.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_OFF);
            if (c.expLock) b.set(CaptureRequest.CONTROL_AE_LOCK, false);
            b.set(CaptureRequest.SENSOR_EXPOSURE_TIME, exp);
            b.set(CaptureRequest.SENSOR_SENSITIVITY, iso);
            b.set(CaptureRequest.SENSOR_FRAME_DURATION, frameNs);
        } else {
            b.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_ON);
            if (c.expLock) b.set(CaptureRequest.CONTROL_AE_LOCK, em == 1);
            if (c.biasStep > 0) {
                int comp = Math.round(Prefs.f(p, Prefs.BIAS, 0) / c.biasStep);
                b.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, Math.max(c.compMin, Math.min(c.compMax, comp)));
            }
        }
        // 色：0 自動・1 固定（今の色）・2 色温度（センサーの色の表から倍率を出す。自動のときの色の変換をそのまま使う）
        int wm = Prefs.i(p, Prefs.WB, 0);
        float[] g = null;
        wbWaitCcm = false;
        if (wm == 2 && c.wbTemp) {
            if (lastCcm == null) wbWaitCcm = true; // 自動の結果が 1 つ届いたら当て直す
            else g = c.wbGains(Prefs.wbTemp(p));
        }
        if (g != null) {
            float k = 1f / Math.min(Math.min(g[0], 1f), g[2]);
            b.set(CaptureRequest.CONTROL_AWB_MODE, CameraMetadata.CONTROL_AWB_MODE_OFF);
            if (c.wbLock) b.set(CaptureRequest.CONTROL_AWB_LOCK, false);
            b.set(CaptureRequest.COLOR_CORRECTION_MODE, CameraMetadata.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX);
            b.set(CaptureRequest.COLOR_CORRECTION_GAINS, new RggbChannelVector(g[0] * k, k, k, g[2] * k));
            b.set(CaptureRequest.COLOR_CORRECTION_TRANSFORM, lastCcm);
        } else {
            b.set(CaptureRequest.CONTROL_AWB_MODE, CameraMetadata.CONTROL_AWB_MODE_AUTO);
            if (c.wbLock) b.set(CaptureRequest.CONTROL_AWB_LOCK, wm == 1);
            if (c.wbTemp) b.set(CaptureRequest.COLOR_CORRECTION_MODE, CameraMetadata.COLOR_CORRECTION_MODE_FAST);
        }
        // ズーム：見え方の倍率をカメラの倍率に直す。切り抜きと水平の分は、カメラの倍率を下げて描き手で拡大する（広角より下には下げない）
        float own = zoomView(r) / c.m;
        float floor = own >= 1f ? 1f : c.ratioLo;
        float ratio = Math.max(c.ratioLo, Math.min(c.ratioHi, Math.max(floor, own / stageExtra())));
        if (c.ratioApi && Build.VERSION.SDK_INT >= 30) {
            b.set(CaptureRequest.CONTROL_ZOOM_RATIO, ratio);
        } else if (c.active != null) {
            float z = Math.max(1f, ratio);
            int w = Math.round(c.active.width() / z), hh = Math.round(c.active.height() / z);
            int x = (c.active.width() - w) / 2, y = (c.active.height() - hh) / 2;
            b.set(CaptureRequest.SCALER_CROP_REGION, new Rect(x, y, x + w, y + hh));
        }
        // ライト（このカメラのライト。前面カメラのときの背面のライトは applyMgrTorch）
        if (c.torchOwn) {
            boolean on = Prefs.b(p, Prefs.TORCH, false);
            b.set(CaptureRequest.FLASH_MODE, on ? CameraMetadata.FLASH_MODE_TORCH : CameraMetadata.FLASH_MODE_OFF);
            if (on && c.torchLevels && Build.VERSION.SDK_INT >= 35) b.set(FLASH_STRENGTH, torchLevelSteps(c));
        }
        // 手ぶれ補正（作り直して決める）。光学式の補正がある機種は、補正を入れたときだけ入れる
        if (c.stabModes.length > 1) {
            int mode = r.stab == 2 ? CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_ON
                    : r.stab == 1 ? CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_PREVIEW_STABILIZATION : CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_OFF;
            b.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, mode);
        }
        if (c.ois && r.stab > 0) b.set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_ON);
        // センターフレーム（Android 14 の自動フレーミング）。画面で触ったことがあるときだけアプリが決める
        if (c.autoframing && Build.VERSION.SDK_INT >= 34 && p.contains(Prefs.CENTER_STAGE)) {
            b.set(CaptureRequest.CONTROL_AUTOFRAMING, Prefs.b(p, Prefs.CENTER_STAGE, false)
                    ? CameraMetadata.CONTROL_AUTOFRAMING_ON : CameraMetadata.CONTROL_AUTOFRAMING_OFF);
        }
        // 顔を追って切り抜くときだけ、顔を探す
        if (c.face) b.set(CaptureRequest.STATISTICS_FACE_DETECT_MODE, faceCrop() ? c.faceMode : CameraMetadata.STATISTICS_FACE_DETECT_MODE_OFF);
    }

    private boolean faceCrop() {
        return gl != null && caps != null && caps.face && Prefs.b(prefs, Prefs.CROP, false) && Prefs.i(prefs, Prefs.CROP_POS, 0) == 3;
    }

    private int torchLevelSteps(CamCaps c) {
        float lv = Math.max(0.05f, Math.min(1f, Prefs.torchLevel(prefs)));
        return Math.max(1, Math.min(c.torchMax, Math.round(lv * c.torchMax)));
    }

    // 前面カメラのとき：背面のライトを CameraManager で点ける。ほかのカメラを開いている間は、機種によっては点かない（そのときは「今は使えない」）
    private void applyMgrTorch() {
        CamCaps c = caps;
        boolean want = c != null && cur != null && session != null && !c.torchOwn && c.torch && Prefs.b(prefs, Prefs.TORCH, false);
        if (!want) { mgrTorchOff(); return; }
        int lv = c.torchLevels ? torchLevelSteps(c) : 0;
        if (mgrWant && lv == mgrLevel && c.torchId.equals(mgrTorchId)) return;
        mgrWant = true;
        mgrLevel = lv;
        mgrTorchId = c.torchId;
        try {
            if (c.torchLevels && Build.VERSION.SDK_INT >= 33) cm.turnOnTorchWithStrengthLevel(c.torchId, lv);
            else cm.setTorchMode(c.torchId, true);
        } catch (CameraAccessException | RuntimeException e) {
            torchAvail.put(c.torchId, false); // 同じ値で何度も試さない。CAPS では「今は使えない」
            Logbook.i("ウェブカメラ：前面カメラの間は背面のライトを点けられません（" + e.getMessage() + "）", "Webcam: cannot turn on the back light while the front camera is in use (" + e.getMessage() + ")");
        }
    }

    private void mgrTorchOff() {
        if (mgrTorchId != null && mgrWant) {
            try { cm.setTorchMode(mgrTorchId, false); } catch (CameraAccessException | RuntimeException ignored) {}
        }
        mgrWant = false;
        mgrLevel = -1;
        mgrTorchId = null;
    }

    // 1 コマごとの結果（h の上）。今のレンズの位置・ISO・色の変換・ライト・顔を覚える
    private final CameraCaptureSession.CaptureCallback resultCb = new CameraCaptureSession.CaptureCallback() {
        @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest rq, TotalCaptureResult res) {
            onResult(res);
        }
    };

    private void onResult(TotalCaptureResult res) {
        Float fd = res.get(CaptureResult.LENS_FOCUS_DISTANCE);
        if (fd != null) lastFocus = fd;
        Integer iso = res.get(CaptureResult.SENSOR_SENSITIVITY);
        if (iso != null) lastIso = iso;
        Integer awb = res.get(CaptureResult.CONTROL_AWB_MODE);
        if (awb != null && awb == CameraMetadata.CONTROL_AWB_MODE_AUTO) {
            ColorSpaceTransform t = res.get(CaptureResult.COLOR_CORRECTION_TRANSFORM);
            if (t != null) {
                lastCcm = t;
                if (wbWaitCcm) { wbWaitCcm = false; requestLive(); }
            }
        }
        Integer fs = res.get(CaptureResult.FLASH_STATE);
        torchFired = fs != null && fs == CameraMetadata.FLASH_STATE_FIRED;
        Rect crop = res.get(CaptureResult.SCALER_CROP_REGION);
        if (crop != null) lastCrop = crop;
        logResult(res);
        if (gl != null && cur != null && faceCrop()) {
            Face[] faces = res.get(CaptureResult.STATISTICS_FACES);
            if (faces != null && faces.length > 0) {
                Face best = faces[0];
                for (Face f : faces) if (area(f.getBounds()) > area(best.getBounds())) best = f;
                float[] o = faceToStage(best.getBounds());
                if (o != null) gl.setFace(o[0], o[1]);
            }
        }
    }

    // カメラに実際に当たった値を、変わったときだけ記録に書く（1 秒に 1 回まで）。本物の機種で、PC や画面の操作が効いたかを記録で確かめるため
    private long lastResLog;
    private String lastResSig = "";

    private static String L(String en, String ja) { return Logbook.t(ja, en); }

    private void logResult(TotalCaptureResult res) {
        long now = SystemClock.elapsedRealtime();
        if (now - lastResLog < 1000) return;
        lastResLog = now;
        StringBuilder b = new StringBuilder();
        // 自動のときに毎コマ変わる値（ISO・露光・焦点・色の倍率）は、手動のときだけ書く
        Integer ae = res.get(CaptureResult.CONTROL_AE_MODE), afm = res.get(CaptureResult.CONTROL_AF_MODE), awb = res.get(CaptureResult.CONTROL_AWB_MODE);
        b.append("AE ").append(ae);
        if (ae != null && ae == CameraMetadata.CONTROL_AE_MODE_OFF) {
            Integer iso = res.get(CaptureResult.SENSOR_SENSITIVITY);
            Long exp = res.get(CaptureResult.SENSOR_EXPOSURE_TIME);
            if (iso != null) b.append(L(", ISO ", "・ISO ")).append(iso);
            if (exp != null && exp > 0) b.append(L(", exposure 1/", "・露光 1/")).append(Math.round(1e9 / exp));
        } else {
            b.append(L(", compensation ", "・補正 ")).append(res.get(CaptureResult.CONTROL_AE_EXPOSURE_COMPENSATION));
            Boolean lk = res.get(CaptureResult.CONTROL_AE_LOCK);
            if (lk != null && lk) b.append(L(", AE locked", "・AE 固定"));
        }
        b.append(L(", AF ", "・AF ")).append(afm);
        if (afm != null && afm == CameraMetadata.CONTROL_AF_MODE_OFF) {
            Float fd = res.get(CaptureResult.LENS_FOCUS_DISTANCE);
            if (fd != null) b.append(L(", focus ", "・焦点 ")).append(Math.round(fd * 100) / 100f);
        }
        b.append(L(", AWB ", "・AWB ")).append(awb);
        if (awb != null && awb == CameraMetadata.CONTROL_AWB_MODE_OFF) {
            RggbChannelVector g = res.get(CaptureResult.COLOR_CORRECTION_GAINS);
            if (g != null) b.append(String.format(java.util.Locale.US, L(", gains %.2f/%.2f/%.2f", "・色の倍率 %.2f/%.2f/%.2f"), g.getRed(), g.getGreenEven(), g.getBlue()));
        } else {
            Boolean lk = res.get(CaptureResult.CONTROL_AWB_LOCK);
            if (lk != null && lk) b.append(L(", AWB locked", "・AWB 固定"));
        }
        if (Build.VERSION.SDK_INT >= 30) {
            Float z = res.get(CaptureResult.CONTROL_ZOOM_RATIO);
            if (z != null) b.append(L(", zoom ", "・倍率 ")).append(Math.round(z * 100) / 100f);
        }
        b.append(L(", flash ", "・ライト ")).append(res.get(CaptureResult.FLASH_STATE));
        b.append(L(", stabilization ", "・手ぶれ ")).append(res.get(CaptureResult.CONTROL_VIDEO_STABILIZATION_MODE));
        Range<Integer> fr = res.get(CaptureResult.CONTROL_AE_TARGET_FPS_RANGE);
        if (fr != null) b.append(L(", fps ", "・fps ")).append(fr);
        Integer sc = res.get(CaptureResult.CONTROL_SCENE_MODE);
        if (sc != null && sc != 0) b.append(L(", scene ", "・シーン ")).append(sc);
        if (Build.VERSION.SDK_INT >= 34) {
            Integer af = res.get(CaptureResult.CONTROL_AUTOFRAMING);
            if (af != null && af != 0) b.append(L(", autoframing ", "・自動フレーミング ")).append(af);
        }
        String sig = b.toString();
        if (sig.equals(lastResSig)) return;
        lastResSig = sig;
        Logbook.i("ウェブカメラ：カメラの今の値 " + sig, "Webcam: camera values " + sig);
    }

    private static long area(Rect r) {
        return (long) r.width() * r.height();
    }

    // 顔の枠（センサーの座標）→ 回したあとの絵の上の 0〜1
    private float[] faceToStage(Rect f) {
        CamCaps c = caps;
        Resolved r = cur;
        if (c == null || c.active == null || r == null) return null;
        Rect reg = c.ratioApi || lastCrop == null ? new Rect(0, 0, c.active.width(), c.active.height()) : lastCrop;
        if (reg.width() <= 0 || reg.height() <= 0) return null;
        float nx = (f.exactCenterX() - reg.left) / reg.width(), ny = (f.exactCenterY() - reg.top) / reg.height();
        float as = r.camW / (float) r.camH, ar = reg.width() / (float) reg.height();
        if (as > ar) { float hf = ar / as; ny = (ny - (1 - hf) / 2) / hf; } else { float wf = as / ar; nx = (nx - (1 - wf) / 2) / wf; }
        float x, y;
        switch (r.rot) {
            case 90: x = 1 - ny; y = nx; break;
            case 180: x = 1 - nx; y = 1 - ny; break;
            case 270: x = ny; y = 1 - nx; break;
            default: x = nx; y = ny; break;
        }
        if (r.mirror) x = 1 - x;
        return new float[] { Math.max(0, Math.min(1, x)), Math.max(0, Math.min(1, y)) };
    }

    // ---- 熱（h の上）----

    private void thermalChanged(int raw) {
        int lv = Thermal.level(raw);
        if (lv == thermalLevel) return;
        Logbook.i("ウェブカメラ：熱の状態が " + raw + "（4 段階で " + lv + "）になりました", "Webcam: thermal status " + raw + " (level " + lv + " of 0-3)");
        thermalLevel = lv;
        if (conn != null && ackSent) sendEvent(conn, 3, lv);
        applyThermal();
        ctlSendState(false, true, WHY_IOS);
    }

    // 熱の状態か熱の設定が変わった。作り直さずに、カメラのコマ数だけ変える。大きさは変わらないので、どの PC でもよい。
    // 途中の ACK を受けられる PC（LIVE）には、ACK（CHANGED と、下げているときは THERMAL）を先に送ってから EVENT 5（webcam_rx はこの順で読む）
    private void applyThermal() {
        Resolved r = cur;
        Conn c = conn;
        if (r == null || c == null || session == null || !ackSent) return;
        int target = thermalFps(r.baseFps);
        if (target == r.fps) return;
        Range<Integer> rr = target >= r.baseFps ? r.baseRange : pickRange(r.camId, target, true);
        if (rr == null && target < r.baseFps) return;
        int fr = target >= r.baseFps ? r.baseFps : Math.min(target, rr.getUpper());
        if (fr == r.fps) return;
        r.fps = fr;
        r.fpsRange = rr;
        boolean low = fr < r.baseFps;
        lowFps = low ? fr : 0;
        curFps = fr;
        liveApply();
        Logbook.i("ウェブカメラ：熱の状態 " + thermalLevel + " のため、コマ数を " + fr + " にしました", "Webcam: frame rate set to " + fr + " for thermal level " + thermalLevel);
        if (hello != null && (hello.flags & HF_LIVE) != 0) {
            c.send(ackMsg(0, r.outW, r.outH, fr, r.kbps, r.camera, r.ackFlags | AF_LIVE | AF_CHANGED | (low ? AF_THERMAL : 0) | (ctl ? AF_CTRL : 0)));
        }
        sendEvent(c, 5, fr); // 5：熱でコマ数を変えた（arg が新しいコマ数）。古い PC は記録に出すだけ
        if (ctl) {
            ctlSendCaps(); // シャッター速度の選択肢と送っているコマ数（108）が変わる
            ctlSendState(true, true, WHY_ALL);
        }
    }

    // ---- CTRL：PC から操作する（h の上。iPhone の ctl* と同じ形）----
    // CAPS：{u32 数}{項目 24 バイト × 数}{u32 倍率のボタンの数}{f32 × 数}{u32 選択肢の表の数}{u16 id・u16 数・f32 × 数 を表の数だけ}
    // STATE：{u16 id・u16 why・f32 値} × n。why は 0 全部・1 画面で変えた・2 端末が変えた・8 SET への返事

    private static final int K_TOGGLE = 0, K_CHOICE = 1, K_NUMBER = 2, K_READONLY = 3;
    private static final int F_REBUILD = 1, F_UNAVAILABLE = 2, F_LOG = 4;
    private static final int WHY_ALL = 0, WHY_APP = 1, WHY_IOS = 2, WHY_REPLY = 8;

    private static final class CtlItem {
        int id, kind, flags;
        float min, max, step;
        float[] choices = new float[0];
    }

    private static final class CtlCaps {
        final List<CtlItem> items = new ArrayList<>();
        float[] stops = new float[0];
        CtlItem find(int id) {
            for (CtlItem it : items) if (it.id == id) return it;
            return null;
        }
    }

    private void ctlReset(boolean on) {
        ctl = on;
        ctlNow = new CtlCaps();
        ctlSent.clear();
        ctlReplies.clear();
        ctlZoom = null;
        ctlPollGen++;
    }

    private void ctlStart() {
        if (!ctl) return;
        ctlSendCaps();
        ctlSendState(true, true, WHY_ALL);
        final int g = ++ctlPollGen;
        ctlPollLater(g);
    }

    // 1 秒ごと：ライト・温度・水平の角度など、端末が変える値を見て、変わった物を送る
    private void ctlPollLater(int g) {
        h.postDelayed(() -> {
            if (g != ctlPollGen || !ctl || conn == null) return;
            ctlSendState(false, true, WHY_IOS);
            ctlPollLater(g);
        }, 1000);
    }

    // 画面の変更・SET のあと。0.1 秒まとめてから送る
    private void ctlSchedule() {
        if (!ctl || ctlFlushQueued) return;
        ctlFlushQueued = true;
        h.postDelayed(() -> {
            ctlFlushQueued = false;
            ctlSendState(false, false, WHY_APP);
        }, 100);
    }

    private CtlCaps ctlBuild() {
        CtlCaps n = new CtlCaps();
        CamCaps c = caps;
        Resolved r = cur;
        if (c == null || r == null) return n;
        // ピント
        if (c.focusAuto || c.focusManual) {
            List<Float> fm = new ArrayList<>();
            fm.add(0f);
            if (c.focusLock) fm.add(1f);
            if (c.focusManual) fm.add(2f);
            choice(n, 1, fm, 0);
            if (c.focusManual) add(n, 2, K_NUMBER, 0, 1, 0.01f, 0);
            if (c.focusTarget()) choice(n, 3, Arrays.asList(0f, 1f), 0);
        }
        // 明るさ
        List<Float> em = new ArrayList<>();
        em.add(0f);
        if (c.expLock) em.add(1f);
        if (c.expManual) em.add(2f);
        choice(n, 4, em, 0);
        if (c.biasMax > c.biasMin) add(n, 5, K_NUMBER, c.biasMin, c.biasMax, c.biasStep, 0);
        if (c.expManual) {
            add(n, 6, K_NUMBER, Math.max(c.isoMin, 1), Math.max(c.isoMax, c.isoMin + 1), 1, F_LOG);
            int[] sh = c.shutters(r.fps);
            if (sh.length > 0) {
                List<Float> v = new ArrayList<>();
                for (int s : sh) v.add((float) s);
                choice(n, 7, v, 0);
            }
        }
        // 色
        List<Float> wm = new ArrayList<>();
        wm.add(0f);
        if (c.wbLock) wm.add(1f);
        if (c.wbTemp) wm.add(2f);
        choice(n, 8, wm, 0);
        if (c.wbTemp) add(n, 9, K_NUMBER, 2500, 8000, 100, 0);
        // ズームと倍率のボタン
        if (c.zoomMax > c.zoomMin + 0.01f) {
            add(n, 10, K_NUMBER, c.zoomMin, c.zoomMax, 0.01f, F_LOG);
            n.stops = c.zoomStops;
        }
        // ライト。前面カメラで背面のライトを点けられないときは「今は使えない」
        if (c.torch) {
            boolean ok = c.torchOwn || !Boolean.FALSE.equals(torchAvail.get(c.torchId));
            int tf = ok ? 0 : F_UNAVAILABLE;
            add(n, 11, K_TOGGLE, 0, 1, 1, tf);
            if (c.torchLevels) add(n, 12, K_NUMBER, 0.05f, 1, 0.05f, tf);
            add(n, 100, K_READONLY, 0, 1, 1, 0);
            add(n, 101, K_READONLY, 0, 1, 0.01f, 0);
        }
        // 手ぶれ補正（作り直す）・センターフレーム
        if (c.stabModes.length > 1) {
            List<Float> v = new ArrayList<>();
            for (int s : c.stabModes) v.add((float) s);
            choice(n, 13, v, F_REBUILD);
        }
        if (c.autoframing) add(n, 16, K_TOGGLE, 0, 1, 1, 0);
        // 向き・切り抜き・水平（描き手を作れなかった端末では出さない）
        if (!glFailed) {
            add(n, 17, K_TOGGLE, 0, 1, 1, F_REBUILD);
            add(n, 18, K_TOGGLE, 0, 1, 1, F_REBUILD);
            CtlItem rot = add(n, 19, K_CHOICE, 0, 270, 90, F_REBUILD);
            rot.choices = new float[] { 0, 90, 180, 270 };
            add(n, 20, K_TOGGLE, 0, 1, 1, F_REBUILD);
            List<Float> pos = new ArrayList<>(Arrays.asList(0f, 1f, 2f));
            if (c.face) pos.add(3f);
            pos.add(4f);
            choice(n, 21, pos, F_REBUILD);
            add(n, 22, K_NUMBER, 0.3f, 1, 0.01f, F_REBUILD);
            add(n, 23, K_NUMBER, 0, 1, 0.01f, 0);
            add(n, 24, K_NUMBER, 0, 1, 0.01f, 0);
            if (c.level) {
                add(n, 25, K_TOGGLE, 0, 1, 1, F_REBUILD);
                add(n, 107, K_READONLY, -LEVEL_MAX_DEG, LEVEL_MAX_DEG, 0.1f, 0);
            }
        }
        // 画面のライト・暖かい色・熱いときにコマ数を下げる・画面を暗くする
        add(n, 26, K_TOGGLE, 0, 1, 1, 0);
        add(n, 27, K_TOGGLE, 0, 1, 1, 0);
        add(n, 28, K_TOGGLE, 0, 1, 1, 0);
        add(n, 29, K_TOGGLE, 0, 1, 1, 0);
        // 読むだけ
        add(n, 102, K_READONLY, 0, 3, 1, 0);
        add(n, 108, K_READONLY, 1, 120, 1, 0);
        Collections.sort(n.items, (a, b) -> a.id - b.id);
        return n;
    }

    private static CtlItem add(CtlCaps n, int id, int kind, float lo, float hi, float step, int flags) {
        CtlItem it = new CtlItem();
        it.id = id;
        it.kind = kind;
        it.min = lo;
        it.max = hi;
        it.step = step;
        it.flags = flags;
        n.items.add(it);
        return it;
    }

    private static void choice(CtlCaps n, int id, List<Float> values, int flags) {
        float lo = Float.MAX_VALUE, hi = -Float.MAX_VALUE;
        for (float v : values) { lo = Math.min(lo, v); hi = Math.max(hi, v); }
        CtlItem it = add(n, id, K_CHOICE, values.isEmpty() ? 0 : lo, values.isEmpty() ? 0 : hi, 1, flags);
        it.choices = new float[values.size()];
        for (int i = 0; i < it.choices.length; i++) it.choices[i] = values.get(i);
    }

    // いまの値。画面に出ている値と同じにする（選べない値が入っていたら、画面と同じく初めの選択肢）。readOnly が false なら 100 からは入れない
    private TreeMap<Integer, Float> ctlValues(boolean readOnly) {
        TreeMap<Integer, Float> v = new TreeMap<>();
        CamCaps c = caps;
        Resolved r = cur;
        if (c == null || r == null) return v;
        SharedPreferences p = prefs;
        for (CtlItem it : ctlNow.items) {
            if (it.id >= 100 && !readOnly) continue;
            float x;
            switch (it.id) {
                case 1: x = Prefs.i(p, Prefs.FOCUS, 0); break;
                case 2: x = Prefs.focusPos(p); break;
                case 3: x = Prefs.i(p, Prefs.FOCUS_TARGET, 0) == 1 ? 1 : 0; break;
                case 4: x = Prefs.i(p, Prefs.EXPOSURE, 0); break;
                case 5: x = Prefs.f(p, Prefs.BIAS, 0); break;
                case 6: x = Prefs.f(p, Prefs.ISO, 0); break; // 0 は「今の値から」
                case 7: {
                    int s = Prefs.i(p, Prefs.SHUTTER, 0);
                    int[] sh = c.shutters(r.fps);
                    int fb = sh.length > 0 ? sh[0] : 0;
                    for (int q : sh) if (q >= Math.max(r.fps, 30) * 2) { fb = q; break; }
                    x = contains(sh, s) ? s : fb;
                    break;
                }
                case 8: x = Prefs.i(p, Prefs.WB, 0); break;
                case 9: x = Prefs.wbTemp(p); break;
                case 10: x = zoomView(r); break;
                case 11: x = Prefs.b(p, Prefs.TORCH, false) ? 1 : 0; break;
                case 12: x = Prefs.torchLevel(p); break;
                case 13: x = Prefs.i(p, Prefs.STAB, 0); break;
                case 16: x = Prefs.b(p, Prefs.CENTER_STAGE, false) ? 1 : 0; break;
                case 17: x = Prefs.b(p, Prefs.MIRROR, false) ? 1 : 0; break;
                case 18: x = Prefs.b(p, Prefs.FLIP_V, false) ? 1 : 0; break;
                case 19: x = Prefs.i(p, Prefs.ROTATION, 0); break;
                case 20: x = Prefs.b(p, Prefs.CROP, false) ? 1 : 0; break;
                case 21: x = Prefs.i(p, Prefs.CROP_POS, 0); break;
                case 22: x = Prefs.cropSize(p); break;
                case 23: x = Prefs.cropX(p); break;
                case 24: x = Prefs.cropY(p); break;
                case 25: x = Prefs.b(p, Prefs.LEVEL, false) ? 1 : 0; break;
                case 26: x = ScreenState.light ? 1 : 0; break;
                case 27: x = Prefs.b(p, Prefs.SCREEN_WARM, false) ? 1 : 0; break;
                case 28: x = Prefs.thermalFps(p) ? 1 : 0; break;
                case 29: x = ScreenState.dimmed ? 1 : 0; break;
                case 100: x = torchActive() ? 1 : 0; break;
                case 101: x = torchActive() ? (c.torchLevels ? torchLevelSteps(c) / (float) c.torchMax : 1) : 0; break;
                case 102: x = thermalLevel; break;
                case 107: x = gl != null ? Math.round(gl.levelShownDeg * 10) / 10f : 0; break;
                case 108: x = r.fps; break;
                default: continue;
            }
            if (Float.isNaN(x) || Float.isInfinite(x)) x = 0;
            // 範囲の外の値（古い設定など）は、範囲に収めて見せる。ISO の 0 は「今の値から」なのでそのまま
            if (it.kind == K_NUMBER && it.id != 6) x = Math.min(Math.max(x, it.min), it.max);
            if (it.kind == K_CHOICE && it.choices.length > 0) {
                boolean in = false;
                for (float q : it.choices) if (q == x) in = true;
                if (!in) x = it.choices[0];
            }
            v.put(it.id, x);
        }
        return v;
    }

    private boolean torchActive() {
        CamCaps c = caps;
        if (c == null || !c.torch) return false;
        if (c.torchOwn) return torchFired;
        return Boolean.TRUE.equals(torchOn.get(c.torchId));
    }

    private void ctlSendCaps() {
        if (!ctl || conn == null || cur == null) return;
        ctlNow = ctlBuild();
        TreeMap<Integer, Float> vals = ctlValues(true);
        List<CtlItem> lists = new ArrayList<>();
        int size = 4 + ctlNow.items.size() * 24 + 4 + ctlNow.stops.length * 4 + 4;
        for (CtlItem it : ctlNow.items) if (it.kind == K_CHOICE) { lists.add(it); size += 4 + it.choices.length * 4; }
        ByteBuffer o = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN);
        o.putInt(ctlNow.items.size());
        for (CtlItem it : ctlNow.items) {
            Float x = vals.get(it.id);
            o.putShort((short) it.id).putShort((short) it.kind).putFloat(it.min).putFloat(it.max).putFloat(it.step)
                    .putFloat(x == null ? 0 : x).putInt(it.flags);
        }
        o.putInt(ctlNow.stops.length);
        for (float s : ctlNow.stops) o.putFloat(s);
        // camctl2 で足した：選ぶ項目の選択肢（PROTOCOL.md の案の後ろ。案だけ読む PC は倍率のボタンまでで止める）
        o.putInt(lists.size());
        for (CtlItem it : lists) {
            o.putShort((short) it.id).putShort((short) it.choices.length);
            for (float x : it.choices) o.putFloat(x);
        }
        conn.send(msg(T_CAPS, o.array()));
    }

    // STATE を送る。all は全部（CAPS のあと・GET）。それ以外は、前に送った値と違う物と、SET への返事
    private void ctlSendState(boolean all, boolean readOnly, int why) {
        if (!ctl || conn == null || cur == null || ctlNow.items.isEmpty()) return;
        TreeMap<Integer, Float> vals = ctlValues(readOnly);
        ByteBuffer o = ByteBuffer.allocate(Math.max(8, vals.size() * 8)).order(ByteOrder.LITTLE_ENDIAN);
        int n = 0;
        for (Map.Entry<Integer, Float> e : vals.entrySet()) {
            int id = e.getKey();
            float x = e.getValue();
            boolean reply = ctlReplies.contains(id);
            Float sent = ctlSent.get(id);
            boolean changed = sent == null || Math.abs(sent - x) > 0.0001f;
            if (!(all || reply || changed)) continue;
            int w = why;
            if (all) w = WHY_ALL;
            else if (reply) w = WHY_REPLY;
            else if (id >= 100) w = WHY_IOS;
            o.putShort((short) id).putShort((short) w).putFloat(x);
            ctlSent.put(id, x);
            n++;
        }
        ctlReplies.removeAll(vals.keySet());
        if (n > 0) conn.send(msg(T_STATE, Arrays.copyOf(o.array(), n * 8)));
    }

    // PC が値を変えた。範囲に収めてから、画面で変えたときと同じ道（設定に書く）で当てる。
    // 使えない操作・範囲の外の値にも、STATE（why 8）で今の値を返す。CAPS に無い id は読み捨てる
    private void ctlSet(byte[] body) {
        Resolved r = cur;
        if (r == null) return; // 作り直しの途中など。このあとの CAPS と STATE で今の値が届く
        ByteBuffer bb = le(body);
        SharedPreferences.Editor e = prefs.edit();
        boolean wrote = false, zoomLive = false;
        for (int at = 0; at + 8 <= body.length; at += 8) {
            int id = bb.getShort(at) & 0xffff;
            float raw = bb.getFloat(at + 4);
            CtlItem it = ctlNow.find(id);
            if (it == null) continue;
            ctlReplies.add(id);
            if (it.kind == K_READONLY || (it.flags & F_UNAVAILABLE) != 0 || Float.isNaN(raw) || Float.isInfinite(raw)) continue;
            float v = raw;
            if (it.kind == K_TOGGLE) {
                v = v != 0 ? 1 : 0;
            } else if (it.kind == K_CHOICE) {
                float best = it.choices.length > 0 ? it.choices[0] : it.min;
                for (float x : it.choices) if (Math.abs(x - v) < Math.abs(best - v)) best = x;
                v = best;
            } else {
                if (it.step > 0) v = Math.round(v / it.step) * it.step;
                v = Math.min(Math.max(v, it.min), it.max);
            }
            boolean on = v != 0;
            switch (id) {
                case 1: e.putInt(Prefs.FOCUS, (int) v); break;
                case 2: e.putFloat(Prefs.FOCUS_POS, v); break;
                case 3: e.putInt(Prefs.FOCUS_TARGET, (int) v); break;
                case 4: e.putInt(Prefs.EXPOSURE, (int) v); break;
                case 5: e.putFloat(Prefs.BIAS, v); break;
                case 6: e.putFloat(Prefs.ISO, v); break;
                case 7: e.putInt(Prefs.SHUTTER, (int) v); break;
                case 8: e.putInt(Prefs.WB, (int) v); break;
                case 9: e.putFloat(Prefs.WB_TEMP, v); break;
                case 10:
                    if (!zoomStored(r)) { ctlZoom = v; zoomLive = true; continue; } // PC が決めたカメラの倍率は、このつなぎの間だけ
                    e.putFloat(Prefs.ZOOM, v);
                    break;
                case 11: e.putBoolean(Prefs.TORCH, on); break;
                case 12: e.putFloat(Prefs.TORCH_LEVEL, v); break;
                case 13: e.putInt(Prefs.STAB, (int) v); break;
                case 16: e.putBoolean(Prefs.CENTER_STAGE, on); break;
                case 17: e.putBoolean(Prefs.MIRROR, on); break;
                case 18: e.putBoolean(Prefs.FLIP_V, on); break;
                case 19: e.putInt(Prefs.ROTATION, (int) v); break;
                case 20: e.putBoolean(Prefs.CROP, on); break;
                case 21: e.putInt(Prefs.CROP_POS, (int) v); break;
                case 22: e.putFloat(Prefs.CROP_SIZE, v); break;
                case 23: e.putFloat(Prefs.CROP_X, v); break;
                case 24: e.putFloat(Prefs.CROP_Y, v); break;
                case 25: e.putBoolean(Prefs.LEVEL, on); break;
                case 26: ScreenState.setLight(ctx, on); continue; // アプリが前にいるときだけ点く（消すのはいつでも）
                case 27: e.putBoolean(Prefs.SCREEN_WARM, on); break;
                case 28: e.putBoolean(Prefs.THERMAL_FPS, on); break;
                case 29: ScreenState.setDim(ctx, on); continue;
                default: continue;
            }
            wrote = true;
        }
        if (wrote) e.apply(); // 設定の変化を見ている prefChanged が当てる（画面にもすぐ出る）
        if (zoomLive) requestLive();
        ctlSchedule();
    }

    // ---- 送る ----

    private static byte[] ackMsg(int status, int w, int hh, int fps, int kbps, int camera, int flags) {
        ByteBuffer o = ByteBuffer.allocate(32).order(ByteOrder.LITTLE_ENDIAN);
        o.putInt(VERSION).putInt(status).putInt(w).putInt(hh).putInt(fps).putInt(kbps).putInt(camera).putInt(flags);
        return msg(T_ACK, o.array());
    }

    private static void sendEvent(Conn c, int code, int arg) {
        ByteBuffer o = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
        o.putInt(code).putInt(arg);
        c.send(msg(T_EVENT, o.array()));
    }

    private static byte[] msg(int type, byte[] body) {
        ByteBuffer o = ByteBuffer.allocate(8 + body.length).order(ByteOrder.LITTLE_ENDIAN);
        o.putInt(type).putInt(body.length).put(body);
        return o.array();
    }

    private static ByteBuffer le(byte[] b) {
        return ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN);
    }

    // カメラの時刻と同じ時計（µs）。FRAME の撮った時刻・送る時刻と PONG に使う
    private long nowUs() {
        return realtimeClock ? SystemClock.elapsedRealtimeNanos() / 1000 : System.nanoTime() / 1000;
    }

    // 1 本のつなぎ。受ける糸と送る糸を持つ。送る物は 1 つの列に積み、送る糸が順に書く（頭と中身は 1 回の write）
    private final class Conn {
        final Link link;
        final LinkedBlockingQueue<byte[]> q = new LinkedBlockingQueue<>();
        final AtomicInteger frames = new AtomicInteger();
        volatile boolean closed;

        Conn(Link link) {
            this.link = link;
        }

        void start() {
            new Thread(this::readLoop, "camera-read").start();
            new Thread(this::writeLoop, "camera-write").start();
        }

        void send(byte[] m) {
            if (!closed) q.offer(m);
        }

        void sendFrame(byte[] m) {
            if (closed) return;
            frames.incrementAndGet();
            q.offer(m);
        }

        void close() {
            closed = true;
            link.close();
            q.offer(POISON);
        }

        private void readLoop() {
            try {
                DataInputStream in = new DataInputStream(new BufferedInputStream(link.in()));
                byte[] hd = new byte[8];
                for (;;) {
                    in.readFully(hd);
                    ByteBuffer bb = le(hd);
                    int type = bb.getInt(0);
                    long len = bb.getInt(4) & 0xffffffffL;
                    if (len > 65536) { Logbook.i("ウェブカメラ：長さが " + len + " バイトで上限を超えました。形が違うので切ります", "Webcam: length " + len + " bytes is over the limit. Wrong format, disconnecting"); break; }
                    byte[] body = new byte[(int) len];
                    in.readFully(body);
                    h.post(() -> handle(this, type, body));
                }
            } catch (IOException ignored) {
            }
            h.post(() -> dropAny(this));
        }

        private void writeLoop() {
            try {
                OutputStream out = link.out();
                for (;;) {
                    byte[] m = q.take();
                    if (m == POISON || closed) break;
                    boolean frame = m.length >= 40 && m[0] == T_FRAME && m[1] == 0 && m[2] == 0 && m[3] == 0;
                    if (frame) {
                        ByteBuffer.wrap(m).order(ByteOrder.LITTLE_ENDIAN).putLong(16, nowUs());
                    }
                    out.write(m);
                    if (frame) {
                        frames.decrementAndGet();
                        sentFrames++;
                    }
                }
            } catch (IOException | InterruptedException ignored) {
            }
            link.close();
        }
    }
}
