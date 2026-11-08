package com.tsurutanmen.pocketgpu;

import android.content.Context;
import android.graphics.SurfaceTexture;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLExt;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.opengl.Matrix;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

// カメラとエンコーダの間で、絵を回す・反転する・切り抜く・水平に保つ（iPhone の接続の回転と renderStage の役目）。
// カメラ → SurfaceTexture（外部テクスチャ）→ OpenGL ES 2 で 1 枚描く → エンコーダの入力の Surface（EGL の窓）。
// 回さず・反転せず・切り抜かず・水平も保たないときは使わない（0.1.1 と同じく、カメラから直接エンコーダへ流す）。
// 描くのは自分の糸（camera-gl）。カメラのコマが来るたびに 1 回描き、カメラの時刻をそのままエンコーダに渡す（PC が測る遅れが変わらないように）
final class GlStage {
    // 描き方。CameraBridge が作って渡す（描く糸は渡された物を読むだけ）
    static final class Stage {
        int rot;            // 時計回りに回す角度（上下反転を入れた後の値。0・90・180・270）
        boolean mirror;     // 回したあとに左右反転（上下反転を入れた後の値）
        float scale = 1f;   // 枠を縮める倍率（切り抜きと水平の余白）
        int pos;            // 0 中央・1 左寄り・2 右寄り・3 顔・4 手動
        float cx = 0.5f, cy = 0.5f; // 手動の枠の真ん中（回したあとの絵の上で 0〜1。左上が 0）
        boolean level;      // 水平を保つ
        float levelMax;     // 直す角度の上限（ラジアン）
        float ref;          // 水平のときの端末の傾き（ラジアン）。センサーの向き＋回す角度
        float sign = 1f;    // 反転していれば -1
    }

    private final int camW, camH, outW, outH;
    private final HandlerThread thread;
    private final Handler gh;
    private EGLDisplay dpy = EGL14.EGL_NO_DISPLAY;
    private EGLContext ectx = EGL14.EGL_NO_CONTEXT;
    private EGLSurface surf = EGL14.EGL_NO_SURFACE;
    private int prog, tex, uTex, aPos, aUV;
    private SurfaceTexture st;
    private Surface input;
    private final FloatBuffer posBuf, uvBuf;
    private final float[] stm = new float[16], mm = new float[16], um = new float[16];
    private volatile Stage stage = new Stage();
    private volatile float faceX = 0.5f, faceY = 0.5f;
    private volatile boolean faceSeen;
    private float faceNowX = 0.5f, faceNowY = 0.5f, levelNow;
    private long lastTs = -1;
    private volatile float gx, gy, gz;
    private volatile boolean released;
    volatile float levelShownDeg;
    volatile long drawn;
    private SensorManager sm;
    private SensorEventListener sl;

    private GlStage(int camW, int camH, int outW, int outH) {
        this.camW = camW;
        this.camH = camH;
        this.outW = outW;
        this.outH = outH;
        thread = new HandlerThread("camera-gl");
        thread.start();
        gh = new Handler(thread.getLooper());
        posBuf = fb(new float[] { -1, -1, 1, -1, -1, 1, 1, 1 });
        uvBuf = fb(new float[] { 0, 0, 1, 0, 0, 1, 1, 1 });
    }

    private static FloatBuffer fb(float[] a) {
        FloatBuffer b = ByteBuffer.allocateDirect(a.length * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
        b.put(a).position(0);
        return b;
    }

    // 作る。エンコーダの Surface に描く。カメラが書く Surface は input()。作れなければ null（呼ぶ側は変形なしに戻す）
    static GlStage create(Context ctx, Surface out, int camW, int camH, int outW, int outH) {
        GlStage g = new GlStage(camW, camH, outW, outH);
        final String[] err = { null };
        CountDownLatch done = new CountDownLatch(1);
        g.gh.post(() -> {
            try {
                g.init(out);
            } catch (RuntimeException e) {
                err[0] = e.getMessage() == null ? e.toString() : e.getMessage();
            }
            done.countDown();
        });
        try {
            if (!done.await(3, TimeUnit.SECONDS)) err[0] = Logbook.t("3 秒たっても OpenGL の準備が終わらない", "OpenGL setup did not finish in 3 s");
        } catch (InterruptedException e) {
            err[0] = Logbook.t("待つのを止められた", "interrupted");
        }
        if (err[0] != null) {
            Logbook.i("ウェブカメラ：回転・切り抜きの描き手を作れません（" + err[0] + "）", "Webcam: cannot create the renderer for rotation and cropping (" + err[0] + ")");
            g.release();
            return null;
        }
        g.sm = ctx.getSystemService(SensorManager.class);
        return g;
    }

    Surface input() {
        return input;
    }

    void setStage(Stage s) {
        stage = s;
        gh.post(this::updateSensor);
    }

    // 顔の真ん中（回したあとの絵の上で 0〜1）
    void setFace(float x, float y) {
        faceX = x;
        faceY = y;
        faceSeen = true;
    }

    private void init(Surface out) {
        dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] ver = new int[2];
        if (dpy == EGL14.EGL_NO_DISPLAY || !EGL14.eglInitialize(dpy, ver, 0, ver, 1)) throw new RuntimeException(Logbook.t("EGL を始められない", "cannot initialize EGL"));
        EGLConfig cfg = choose(true);
        if (cfg == null) cfg = choose(false);
        if (cfg == null) throw new RuntimeException(Logbook.t("EGL の形を選べない", "cannot choose an EGL config"));
        ectx = EGL14.eglCreateContext(dpy, cfg, EGL14.EGL_NO_CONTEXT, new int[] { EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE }, 0);
        if (ectx == null || ectx == EGL14.EGL_NO_CONTEXT) throw new RuntimeException(Logbook.t("EGL の文脈を作れない（0x", "cannot create the EGL context (0x") + Integer.toHexString(EGL14.eglGetError()) + Logbook.t("）", ")"));
        surf = EGL14.eglCreateWindowSurface(dpy, cfg, out, new int[] { EGL14.EGL_NONE }, 0);
        if (surf == null || surf == EGL14.EGL_NO_SURFACE) throw new RuntimeException(Logbook.t("エンコーダの Surface に描けない（0x", "cannot draw to the encoder surface (0x") + Integer.toHexString(EGL14.eglGetError()) + Logbook.t("）", ")"));
        if (!EGL14.eglMakeCurrent(dpy, surf, surf, ectx)) throw new RuntimeException(Logbook.t("eglMakeCurrent に失敗（0x", "eglMakeCurrent failed (0x") + Integer.toHexString(EGL14.eglGetError()) + Logbook.t("）", ")"));
        prog = program(
                "uniform mat4 uTex;\nattribute vec4 aPos;\nattribute vec4 aUV;\nvarying vec2 vUV;\n"
                        + "void main() { gl_Position = aPos; vUV = (uTex * aUV).xy; }\n",
                "#extension GL_OES_EGL_image_external : require\n"
                        + "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
                        + "varying vec2 vUV;\nuniform samplerExternalOES sTex;\n"
                        + "void main() { gl_FragColor = texture2D(sTex, vUV); }\n");
        uTex = GLES20.glGetUniformLocation(prog, "uTex");
        aPos = GLES20.glGetAttribLocation(prog, "aPos");
        aUV = GLES20.glGetAttribLocation(prog, "aUV");
        int[] t = new int[1];
        GLES20.glGenTextures(1, t, 0);
        tex = t[0];
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, tex);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
        st = new SurfaceTexture(tex);
        st.setDefaultBufferSize(camW, camH);
        st.setOnFrameAvailableListener(s -> draw(), gh);
        input = new Surface(st);
        int e = GLES20.glGetError();
        if (e != GLES20.GL_NO_ERROR) throw new RuntimeException(Logbook.t("OpenGL の準備で失敗（0x", "OpenGL setup failed (0x") + Integer.toHexString(e) + Logbook.t("）", ")"));
        Logbook.i("ウェブカメラ：回転・切り抜きの描き手を作りました（" + camW + "x" + camH + " → " + outW + "x" + outH + "・GL " + GLES20.glGetString(GLES20.GL_RENDERER) + "）", "Webcam: created the renderer for rotation and cropping (" + camW + "x" + camH + " to " + outW + "x" + outH + ", GL " + GLES20.glGetString(GLES20.GL_RENDERER) + ")");
    }

    private EGLConfig choose(boolean recordable) {
        int[] a = recordable
                ? new int[] { EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                        EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, 0x3142 /* EGL_RECORDABLE_ANDROID */, 1, EGL14.EGL_NONE }
                : new int[] { EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                        EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_NONE };
        EGLConfig[] cs = new EGLConfig[1];
        int[] n = new int[1];
        if (!EGL14.eglChooseConfig(dpy, a, 0, cs, 0, 1, n, 0) || n[0] < 1) return null;
        return cs[0];
    }

    private static int shader(int type, String src) {
        int s = GLES20.glCreateShader(type);
        GLES20.glShaderSource(s, src);
        GLES20.glCompileShader(s);
        int[] ok = new int[1];
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0);
        if (ok[0] == 0) {
            String log = GLES20.glGetShaderInfoLog(s);
            GLES20.glDeleteShader(s);
            throw new RuntimeException(Logbook.t("シェーダを組めない：", "cannot compile the shader: ") + log);
        }
        return s;
    }

    private static int program(String vs, String fs) {
        int p = GLES20.glCreateProgram();
        GLES20.glAttachShader(p, shader(GLES20.GL_VERTEX_SHADER, vs));
        GLES20.glAttachShader(p, shader(GLES20.GL_FRAGMENT_SHADER, fs));
        GLES20.glLinkProgram(p);
        int[] ok = new int[1];
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0);
        if (ok[0] == 0) throw new RuntimeException(Logbook.t("シェーダをつなげない：", "cannot link the shader: ") + GLES20.glGetProgramInfoLog(p));
        return p;
    }

    // 水平を保つときだけ、重さの向きを読む（描く糸で受ける）
    private void updateSensor() {
        if (released || sm == null) return;
        boolean want = stage.level;
        if (want && sl == null) {
            Sensor s = sm.getDefaultSensor(Sensor.TYPE_GRAVITY);
            final boolean raw = s == null;
            if (s == null) s = sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
            if (s == null) return;
            sl = new SensorEventListener() {
                @Override public void onSensorChanged(SensorEvent ev) {
                    if (raw) { // 加速度だけの機種は、なめらかにして重さの向きにする
                        gx += 0.1f * (ev.values[0] - gx);
                        gy += 0.1f * (ev.values[1] - gy);
                        gz += 0.1f * (ev.values[2] - gz);
                    } else {
                        gx = ev.values[0];
                        gy = ev.values[1];
                        gz = ev.values[2];
                    }
                }
                @Override public void onAccuracyChanged(Sensor sensor, int accuracy) {}
            };
            sm.registerListener(sl, s, SensorManager.SENSOR_DELAY_GAME, gh);
        } else if (!want && sl != null) {
            sm.unregisterListener(sl);
            sl = null;
            levelShownDeg = 0;
        }
    }

    // 1 コマ描く（描く糸）
    private void draw() {
        if (released || st == null) return;
        try {
            st.updateTexImage();
        } catch (RuntimeException e) {
            return;
        }
        long ts = st.getTimestamp();
        double dt = lastTs < 0 ? 0 : Math.min(Math.max((ts - lastTs) / 1e9, 0), 0.1);
        lastTs = ts;
        st.getTransformMatrix(stm);
        Stage s = stage;
        affine(s, dt, mm);
        Matrix.multiplyMM(um, 0, stm, 0, mm, 0);
        GLES20.glViewport(0, 0, outW, outH);
        GLES20.glUseProgram(prog);
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, tex);
        GLES20.glUniformMatrix4fv(uTex, 1, false, um, 0);
        GLES20.glEnableVertexAttribArray(aPos);
        GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 8, posBuf);
        GLES20.glEnableVertexAttribArray(aUV);
        GLES20.glVertexAttribPointer(aUV, 2, GLES20.GL_FLOAT, false, 8, uvBuf);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
        EGLExt.eglPresentationTimeANDROID(dpy, surf, ts);
        EGL14.eglSwapBuffers(dpy, surf);
        drawn++;
    }

    // 送る絵の座標（0〜1・下が 0）→ カメラの絵の座標（0〜1・下が 0）の 1 次の写し。iPhone の renderStage と同じ枠の決め方
    private void affine(Stage s, double dt, float[] out) {
        boolean portrait = s.rot % 180 != 0;
        float ow = outW, oh = outH;
        float W = portrait ? camH : camW, H = portrait ? camW : camH; // 回したあとの絵の大きさ
        // 傾き：重さの向きから端末の傾きを出し、水平のときの傾きとの差を直す（平らに置いているときは直さない）
        float target = 0;
        if (s.level) {
            float x = gx, y = gy;
            if (Math.hypot(x, y) > 0.5 * SensorManager.GRAVITY_EARTH) {
                // iPhone の重さ（下向き・g 単位）にそろえると (-x, -y)。iPhone と同じく atan2(g.x, -g.y)
                double e = Math.atan2(-x, y) - s.ref;
                while (e > Math.PI) e -= 2 * Math.PI;
                while (e < -Math.PI) e += 2 * Math.PI;
                if (Math.abs(e) < Math.PI / 4) target = (float) (Math.max(-s.levelMax, Math.min(s.levelMax, e)) * s.sign);
            }
        }
        levelNow = smooth(levelNow, target, dt, 0.5, (float) Math.toRadians(0.3));
        levelShownDeg = (float) Math.toDegrees(levelNow);
        float fcx = s.cx, fcy = s.cy;
        if (s.pos == 3) {
            if (faceSeen) {
                faceNowX = smooth(faceNowX, faceX, dt, 0.4, 0.02f);
                faceNowY = smooth(faceNowY, faceY, dt, 0.4, 0.02f);
            }
            fcx = faceNowX;
            fcy = faceNowY;
        }
        float bw = W, bh = W * oh / ow;
        if (bh > H) { bh = H; bw = H * ow / oh; }
        float sc = Math.max(1f, s.scale);
        float rw = bw / sc, rh = bh / sc;
        double a = levelNow, ca = Math.cos(a), sa = Math.sin(a);
        double hx = (rw * Math.abs(ca) + rh * Math.abs(sa)) / 2, hy = (rw * Math.abs(sa) + rh * Math.abs(ca)) / 2;
        double cx, cy;
        switch (s.pos) {
            case 1: cx = hx; cy = H / 2.0; break;
            case 2: cx = W - hx; cy = H / 2.0; break;
            case 3: case 4: cx = fcx * W; cy = fcy * H; break;
            default: cx = W / 2.0; cy = H / 2.0; break;
        }
        cx = hx * 2 >= W ? W / 2.0 : Math.min(Math.max(cx, hx), W - hx);
        cy = hy * 2 >= H ? H / 2.0 : Math.min(Math.max(cy, hy), H - hy);
        double k = rw / ow; // 送る絵の 1 画素が、回したあとの絵の何画素か
        double[] p0 = map(s, 0, 0, ow, oh, W, H, cx, cy, ca, sa, k);
        double[] p1 = map(s, 1, 0, ow, oh, W, H, cx, cy, ca, sa, k);
        double[] p2 = map(s, 0, 1, ow, oh, W, H, cx, cy, ca, sa, k);
        Matrix.setIdentityM(out, 0);
        out[0] = (float) (p1[0] - p0[0]);
        out[1] = (float) (p1[1] - p0[1]);
        out[4] = (float) (p2[0] - p0[0]);
        out[5] = (float) (p2[1] - p0[1]);
        out[12] = (float) p0[0];
        out[13] = (float) p0[1];
    }

    private static double[] map(Stage s, double u, double v, float ow, float oh, float W, float H, double cx, double cy, double ca, double sa, double k) {
        double xo = u * ow, yo = (1 - v) * oh;          // 送る絵の画素（上が 0）
        double dx = (xo - ow / 2) * k, dy = (yo - oh / 2) * k;
        double ox = cx + dx * ca + dy * sa, oy = cy - dx * sa + dy * ca; // 回したあとの絵の画素（枠を傾きの分だけ回す）
        double nx = ox / W, ny = oy / H;
        if (s.mirror) nx = 1 - nx;
        double x, y; // カメラの絵（上が 0）
        switch (s.rot) {
            case 90: x = ny; y = 1 - nx; break;
            case 180: x = 1 - nx; y = 1 - ny; break;
            case 270: x = 1 - ny; y = nx; break;
            default: x = nx; y = ny; break;
        }
        return new double[] { x, 1 - y };
    }

    // なめらかにする：dead より小さい差は動かさない。それより大きい差は、tau 秒でなじむように少しずつ寄せる（iPhone と同じ）
    private static float smooth(float cur, float target, double dt, double tau, float dead) {
        float diff = target - cur;
        if (Math.abs(diff) <= dead) return cur;
        if (dt <= 0) return target;
        float k = (float) (1 - Math.exp(-dt / tau));
        return cur + (diff - (diff > 0 ? dead : -dead)) * k;
    }

    // 片づける。カメラを閉じたあと、エンコーダを止める前に呼ぶ
    void release() {
        if (released) return;
        released = true;
        CountDownLatch done = new CountDownLatch(1);
        gh.post(() -> {
            if (sl != null && sm != null) { sm.unregisterListener(sl); sl = null; }
            try {
                if (input != null) input.release();
                if (st != null) st.release();
                if (dpy != EGL14.EGL_NO_DISPLAY) {
                    EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                    if (surf != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(dpy, surf);
                    if (ectx != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(dpy, ectx);
                    EGL14.eglReleaseThread();
                    EGL14.eglTerminate(dpy);
                }
            } catch (RuntimeException ignored) {
            }
            input = null;
            st = null;
            done.countDown();
        });
        try { done.await(2, TimeUnit.SECONDS); } catch (InterruptedException ignored) {}
        thread.quitSafely();
    }
}
