package com.tsurutanmen.pocketgpu;

import android.Manifest;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import android.media.audiofx.AcousticEchoCanceler;
import android.media.audiofx.NoiseSuppressor;
import android.os.Build;
import android.os.Process;
import android.os.SystemClock;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.atomic.AtomicInteger;

// Android を PC のスピーカーとマイクにする。iPhone の AudioBridge.swift と同じ約束（work/fix1012/pc/proto.h）。
// PC は ipaudio.exe。USB は adb forward tcp:50063 localabstract:pocketgpu-audio のあと ipaudio --connect 127.0.0.1:50063 --speaker --mic。
// つなぎ方（0.1.2 で直した。Link.java）：USB は抽象名の UNIX ソケット pocketgpu-audio で待ち、相手の uid が adb のときだけ受ける。
//   古い PC のツール（adb forward tcp:50063 tcp:50063）は、設定で「古い PC のツールも受け付ける」を入れたときだけ 127.0.0.1:50063 で受ける。Wi-Fi では待たない。
// 1 本のつなぎで両方向を流す：
//   PC → 端末：HELLO（40 バイト）→ 以降 [16 バイトの頭][PCM]（SPK。s16le・PC の rate・ch）
//   端末 → PC：ACK（40 バイト）→ 以降 [16 バイトの頭][PCM]（MIC。rate・ch・形は ACK に書いた物）
//   PC からの PING には PONG、1 秒ごとに STATS、BYE で終わる
// マイクの形と音の種類（0.1.2 で足した。iPhone の mic2・fix1012 と同じ。古い PC は HELLO で 1ch・s16 を頼むので、前と同じ形で送る）：
//   ch：HELLO の mic_channels が 2 で、画面の「ステレオで送る」がオンで、端末が 2ch で取れたときだけ 2ch。それ以外は 1ch
//   形：HELLO の mic_format の 1 = s16le・2 = s24le（3 バイト）・3 = float32le。知らない値は 1。取るのは float（だめなら 16 ビット）
//   rate：HELLO の mic_rate（8000〜48000）
//   音の種類：HELLO の 28 バイト目。0 は画面の設定・1 加工なし（UNPROCESSED。無い機種は VOICE_RECOGNITION）・2 標準（MIC）・
//            3 雑音とエコーを消す（VOICE_COMMUNICATION）。0 で flags に 0x4 があれば 3（前の PC の --voiceproc）
//   ACK の 32〜35 バイト目：使った音の種類・入力の種類・ステレオの印・入力の機器の ch 数。前の PC は読まない
//   マイクの音量（-20〜+30 dB）は送る前に掛け、-1〜1 に収める。画面に入力レベル（音量を掛けた後のいちばん大きい値）を出す
// あとからマイクを始める（iPhone の 0.7.60 と同じ形）：マイクを始められなかったが、許可はあるとき、ACK の flags に 0x0008（aMicLater）を立て、
//   1・2・4…30 秒おいて試し直す。始まったら、ACK に書いた形で MIC の塊を送り出す。STATS の 33〜36 バイト目にアプリの状態
// スピーカーは「ため」（PlayoutRing。proto.h の IpaPlayout と同じ）を通して AudioTrack に書く
final class AudioBridge {
    static final int PORT = 50063;
    static final String USB_NAME = "pocketgpu-audio";
    private static final int T_SPK = 1, T_MIC = 2, T_PING = 3, T_PONG = 4, T_STATS = 5, T_BYE = 6;
    private static final int F_SPEAKER = 1, F_MIC = 2, F_VOICEPROC = 4;
    private static final int A_SPEAKER_OK = 1, A_MIC_OK = 2, A_MIC_LATER = 8;
    private static final int S_FRONT = 1, S_MIC_RUNNING = 2, S_MIC_WAITING = 4, S_MIC_DENIED = 8;
    static final int FMT_S16 = 1, FMT_S24 = 2, FMT_F32 = 3;
    private static final int A2_STEREO_CAPABLE = 1, A2_STEREO_ON = 2;
    private static final int FR_SILENT = 1;
    private static final int MAX_PAYLOAD = 1 << 20;
    private static final byte[] POISON = new byte[0];

    private final Context ctx;
    private final SharedPreferences prefs;

    // 画面から読む
    volatile boolean connected, speakerActive, micActive, micRetrying, micBlocked;
    volatile double fillMs;
    volatile int pcMicMode;        // PC が指定した音の種類（0 は指定なし）
    volatile String micDetail = "", inputName = "";

    private final Link.UsbPort usb;
    private final Link.TcpPort tcp;
    private volatile boolean enabled, legacy;
    private Session cur; // this で守る

    private final SharedPreferences.OnSharedPreferenceChangeListener prefListener = (p, k) -> {
        if (Prefs.MIC_GAIN.equals(k)) MicLevel.setDb(Prefs.micGain(p));
        else if (Prefs.MIC_MODE.equals(k) || Prefs.MIC_INPUT.equals(k) || Prefs.MIC_STEREO.equals(k)) {
            Session s;
            synchronized (this) { s = cur; }
            if (s != null) s.restartMic(Logbook.t("マイクの設定が変わった", "the mic settings changed"));
        }
    };

    AudioBridge(Context c) {
        ctx = c.getApplicationContext();
        prefs = Prefs.get(ctx);
        MicLevel.setDb(Prefs.micGain(prefs));
        usb = new Link.UsbPort(USB_NAME, "音", "Audio", this::accepted);
        tcp = new Link.TcpPort(PORT, "音", "Audio", this::accepted);
        prefs.registerOnSharedPreferenceChangeListener(prefListener);
    }

    synchronized void setEnabled(boolean on) {
        if (on == enabled) return;
        enabled = on;
        usb.setEnabled(on);
        tcp.configure(on, false, legacy);
        if (!on) {
            if (cur != null) cur.close(Logbook.t("止めた", "stopped"));
            cur = null;
            Logbook.i("音の受け付けを止めました", "Stopped accepting audio connections");
        }
    }

    synchronized void setLegacy(boolean on) {
        legacy = on;
        tcp.configure(enabled, false, legacy);
    }

    // 前面サービスがマイクの種類なしで動いているとき（Android 14 から、後ろからマイクの種類では始められない）。マイクを使わず、スピーカーだけにする
    void setMicBlocked(boolean b) {
        micBlocked = b;
    }

    long foreignRejectedAt() { return usb.lastRejectAt; }
    long legacyRejectedAt() { return tcp.lastLoopbackRejectAt; }

    private void accepted(Link l) {
        Session n = new Session(l);
        synchronized (this) {
            if (!enabled) { l.close(); return; }
            if (cur != null) cur.close(Logbook.t("新しい接続が来た", "a new connection came")); // 一度に 1 本。新しい方を残す
            cur = n;
        }
        n.start();
    }

    private synchronized void ended(Session s) {
        if (cur != s) return;
        cur = null;
        connected = false;
        speakerActive = false;
        micActive = false;
        micRetrying = false;
        fillMs = 0;
        pcMicMode = 0;
        micDetail = "";
        inputName = "";
    }

    // マイクの音量と、画面に出す入力レベル（iPhone の MicGain と同じ）
    static final class MicLevel {
        private static volatile float factor = 1f;
        private static float peak;
        static void setDb(float db) { factor = (float) Math.pow(10, db / 20.0); }
        static float factor() { return factor; }
        static synchronized void note(float p) { if (p > peak) peak = p; }
        // 前に読んでからのいちばん大きい値（音量を掛けた後）を返して 0 に戻す
        static synchronized float takePeak() { float p = peak; peak = 0; return p; }
    }

    // ACK で PC に伝えたマイクの形。あとで始めても、作り直しても変えない
    private static final class Promise {
        int rate = 48000, channels = 1, format = FMT_S16;
    }

    // ---- 1 本のつなぎ ----

    private final class Session {
        final Link link;
        final LinkedBlockingQueue<byte[]> q = new LinkedBlockingQueue<>();
        final AtomicInteger pendingMic = new AtomicInteger();
        volatile boolean closed;
        PlayoutRing ring;
        AudioTrack track;
        // マイク（micLock で守る）
        private final Object micLock = new Object();
        private AudioRecord rec;
        private Thread micThread;
        private AcousticEchoCanceler aec;
        private NoiseSuppressor ns;
        private boolean commDevSet;
        private int recEnc, recCh, inputKind, mode;
        private boolean stereoCapable;
        int spkRate = 48000, spkCh = 2, helloMode, helloFlags;
        final Promise promise = new Promise();
        boolean wantMic, micDenied;
        long micRetryAt, micRetryDelay = 1000;
        long spkExpected, micIndex;
        volatile long micSent, micDropped, spkFrames;
        volatile float spkPeak;

        Session(Link l) {
            link = l;
        }

        void start() {
            new Thread(this::run, "audio-read").start();
        }

        void send(byte[] m) {
            if (!closed) q.offer(m);
        }

        void close(String why) {
            if (closed) return;
            closed = true;
            link.close();
            q.offer(POISON);
            Logbook.i("音：切れました（" + why + "）", "Audio: disconnected (" + why + ")");
        }

        private void run() {
            Logbook.i("音：PC が接続しました（" + link.desc + "）", "Audio: the PC connected (" + link.desc + ")");
            try {
                DataInputStream in = new DataInputStream(new BufferedInputStream(link.in()));
                byte[] hb = new byte[40];
                readWithTimeout(in, hb);
                ByteBuffer hl = ByteBuffer.wrap(hb).order(ByteOrder.LITTLE_ENDIAN);
                if (hb[0] != 'I' || hb[1] != 'P' || hb[2] != 'A' || hb[3] != 'U') { close(Logbook.t("あいさつの形が違う", "wrong hello")); return; }
                int flags = hl.getShort(6) & 0xffff;
                spkRate = hl.getInt(8);
                spkCh = hl.getShort(12) & 0xffff;
                int spkFmt = hl.getShort(14) & 0xffff;
                int mr = hl.getInt(16);
                int mch = hl.getShort(20) & 0xffff;
                int mfmt = hl.getShort(22) & 0xffff;
                int targetMs = hl.getShort(26) & 0xffff;
                helloMode = hb[28] & 0xff;
                helloFlags = flags;
                promise.rate = mr == 0 ? 48000 : Math.max(8000, Math.min(48000, mr));
                promise.format = mfmt == FMT_S24 || mfmt == FMT_F32 ? mfmt : FMT_S16;
                promise.channels = mch >= 2 && Prefs.b(prefs, Prefs.MIC_STEREO, false) ? 2 : 1;
                if (spkRate < 8000 || spkRate > 192000) spkRate = 48000;
                if (spkCh < 1 || spkCh > 2) spkCh = 2;
                pcMicMode = helloMode >= 1 && helloMode <= 3 ? helloMode : 0;
                Logbook.i("音：あいさつ flags=0x" + Integer.toHexString(flags) + " スピーカー " + spkRate + " Hz・" + spkCh + " ch マイク " + mr + " Hz・" + mch
                        + " ch・形 " + mfmt + "・種類 " + helloMode + " ため " + targetMs + " ms", "Audio: hello flags=0x" + Integer.toHexString(flags) + " speaker " + spkRate + " Hz, " + spkCh + " ch, mic " + mr + " Hz, " + mch + " ch, format " + mfmt + ", mode " + helloMode + ", buffer " + targetMs + " ms");

                new Thread(this::writeLoop, "audio-write").start();
                boolean spkOk = false, micOk = false;
                if ((flags & F_SPEAKER) != 0 && spkFmt == 1) {
                    double t = targetMs == 0 ? 40 : Math.max(10, Math.min(300, targetMs));
                    ring = new PlayoutRing(spkCh, spkRate, t);
                    spkOk = openTrack();
                    if (!spkOk) ring = null;
                }
                wantMic = (flags & F_MIC) != 0;
                boolean later = false;
                if (wantMic) {
                    micDenied = ctx.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED;
                    if (micDenied) {
                        Logbook.i("音：マイクの使用が許可されていないので、スピーカーだけ使います", "Audio: microphone access is not allowed, so only the speaker is used");
                    } else if (micBlocked) {
                        Logbook.i("音：前面サービスがマイクの種類なしで動いているので、マイクを使いません（アプリを開いて、機能を入れ直してください）", "Audio: the foreground service runs without the microphone type, so the mic is not used. Open the app and turn the feature on again");
                    } else {
                        micOk = openMic(true);
                        later = !micOk;
                        if (later) micRetryAt = SystemClock.elapsedRealtime() + micRetryDelay;
                    }
                }
                sendAck(spkOk, micOk, later);
                speakerActive = spkOk;
                micActive = micOk;
                micRetrying = later;
                connected = true;
                if (spkOk) new Thread(this::playLoop, "audio-play").start();
                if (micOk) startMicLoop();
                new Thread(this::statsLoop, "audio-stats").start();
                readLoop(in);
            } catch (IOException e) {
                close(Logbook.t("読めない：", "read error: ") + e.getMessage());
            } finally {
                close(Logbook.t("終わり", "ended"));
                stopDevices();
                ended(this);
            }
        }

        // HELLO は 5 秒まで待つ（LocalSocket は読む時間の上限を付けられないので、待つ糸で切る）
        private void readWithTimeout(DataInputStream in, byte[] hb) throws IOException {
            final boolean[] got = { false };
            Thread guard = new Thread(() -> {
                SystemClock.sleep(5000);
                synchronized (got) { if (!got[0]) link.close(); }
            }, "audio-hello-wait");
            guard.start();
            in.readFully(hb);
            synchronized (got) { got[0] = true; }
            guard.interrupt();
        }

        // ---- 音の機器 ----

        private boolean openTrack() {
            try {
                int mask = spkCh == 1 ? AudioFormat.CHANNEL_OUT_MONO : AudioFormat.CHANNEL_OUT_STEREO;
                int min = AudioTrack.getMinBufferSize(spkRate, mask, AudioFormat.ENCODING_PCM_FLOAT);
                int want = Math.max(min, spkRate / 50 * spkCh * 4); // 20 ms 以上
                track = new AudioTrack.Builder()
                        .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                        .setAudioFormat(new AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_FLOAT).setSampleRate(spkRate).setChannelMask(mask).build())
                        .setBufferSizeInBytes(want)
                        .setTransferMode(AudioTrack.MODE_STREAM)
                        .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                        .build();
                if (track.getState() != AudioTrack.STATE_INITIALIZED) throw new IllegalStateException(Logbook.t("AudioTrack を作れない", "cannot create AudioTrack"));
                track.play();
                Logbook.i("音：スピーカーを始めました（バッファ " + track.getBufferSizeInFrames() + " フレーム）", "Audio: speaker started (buffer " + track.getBufferSizeInFrames() + " frames)");
                return true;
            } catch (RuntimeException e) {
                Logbook.i("音：スピーカーを始められません：" + e, "Audio: could not start the speaker: " + e);
                if (track != null) { track.release(); track = null; }
                return false;
            }
        }

        // 音の種類：PC が決めた物（HELLO の 28 バイト目）。0 なら --voiceproc（flags の 0x4）か画面の設定
        private int micMode() {
            if (helloMode >= 1 && helloMode <= 3) return helloMode;
            if ((helloFlags & F_VOICEPROC) != 0) return 3;
            return Prefs.micMode(prefs);
        }

        // マイクを始める。first は ACK の前（ステレオで取れなければ、伝える ch を 1 にしてよい）
        private boolean openMic(boolean first) {
            synchronized (micLock) {
                if (closed) return false;
                if (ctx.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) return false;
                mode = micMode();
                AudioManager am = ctx.getSystemService(AudioManager.class);
                int source;
                if (mode == 1) {
                    boolean un = "true".equals(am.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED));
                    source = un ? MediaRecorder.AudioSource.UNPROCESSED : MediaRecorder.AudioSource.VOICE_RECOGNITION;
                } else if (mode == 3) {
                    source = MediaRecorder.AudioSource.VOICE_COMMUNICATION;
                } else {
                    source = MediaRecorder.AudioSource.MIC;
                }
                AudioDeviceInfo dev = pickInput(am);
                stereoCapable = builtinStereo(am);
                boolean wantStereo = promise.channels == 2 || (first && promise.channels == 2);
                int[] encs = { AudioFormat.ENCODING_PCM_FLOAT, AudioFormat.ENCODING_PCM_16BIT };
                int[] chs = wantStereo ? new int[] { 2, 1 } : new int[] { 1 };
                for (int ch : chs) {
                    for (int enc : encs) {
                        AudioRecord r = null;
                        try {
                            int mask = ch == 2 ? AudioFormat.CHANNEL_IN_STEREO : AudioFormat.CHANNEL_IN_MONO;
                            int bps = enc == AudioFormat.ENCODING_PCM_FLOAT ? 4 : 2;
                            int min = AudioRecord.getMinBufferSize(promise.rate, mask, enc);
                            if (min <= 0) continue;
                            r = new AudioRecord.Builder()
                                    .setAudioSource(source)
                                    .setAudioFormat(new AudioFormat.Builder().setEncoding(enc).setSampleRate(promise.rate).setChannelMask(mask).build())
                                    .setBufferSizeInBytes(Math.max(min, promise.rate / 10 * ch * bps))
                                    .build();
                            if (r.getState() != AudioRecord.STATE_INITIALIZED || r.getChannelCount() != ch) throw new IllegalStateException(Logbook.t("AudioRecord を作れない", "cannot create AudioRecord"));
                            if (dev != null) r.setPreferredDevice(dev);
                            r.startRecording();
                            if (r.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) throw new IllegalStateException(Logbook.t("録音を始められない", "cannot start recording"));
                        } catch (RuntimeException e) {
                            if (r != null) r.release();
                            continue;
                        }
                        rec = r;
                        recEnc = enc;
                        recCh = ch;
                        if (ch == 2) stereoCapable = true;
                        if (first && promise.channels == 2 && ch == 1) promise.channels = 1; // ステレオで取れない。ACK で 1ch と伝える
                        if (mode == 3) effectsOn(r.getAudioSessionId());
                        AudioDeviceInfo routed = r.getRoutedDevice();
                        inputKind = kindOf(routed);
                        inputName = routed == null ? "" : deviceName(routed);
                        micDetail = detailText();
                        Logbook.i("音：マイクを始めました（" + promise.rate + " Hz・取る " + ch + " ch・" + (enc == AudioFormat.ENCODING_PCM_FLOAT ? "float" : "16 ビット")
                                + "・送る " + promise.channels + " ch・" + fmtName(promise.format) + "・種類 " + mode + "・入力 " + (routed == null ? "?" : routed.getType() + " " + routed.getProductName()) + "）", "Audio: mic started (" + promise.rate + " Hz, capture " + ch + " ch " + (enc == AudioFormat.ENCODING_PCM_FLOAT ? "float" : "16-bit") + ", send " + promise.channels + " ch " + fmtName(promise.format) + ", mode " + mode + ", input " + (routed == null ? "?" : routed.getType() + " " + routed.getProductName()) + ")");
                        return true;
                    }
                }
                clearCommDevice(am);
                Logbook.i("音：マイクを始められません（" + promise.rate + " Hz・種類 " + mode + "）", "Audio: could not start the mic (" + promise.rate + " Hz, mode " + mode + ")");
                return false;
            }
        }

        // 入力の選び方（iPhone と同じ 5 つ）。選んだ機器が無いときは本体のマイク
        private AudioDeviceInfo pickInput(AudioManager am) {
            String in = Prefs.micInput(prefs);
            clearCommDevice(am);
            if ("auto".equals(in)) return null;
            AudioDeviceInfo builtin = null, hit = null;
            for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
                int t = d.getType();
                if (t == AudioDeviceInfo.TYPE_BUILTIN_MIC && builtin == null) builtin = d;
                if (hit != null) continue;
                switch (in) {
                    case "wired": if (t == AudioDeviceInfo.TYPE_WIRED_HEADSET) hit = d; break;
                    case "usb": if (t == AudioDeviceInfo.TYPE_USB_HEADSET || t == AudioDeviceInfo.TYPE_USB_DEVICE) hit = d; break;
                    case "bluetooth": if (t == AudioDeviceInfo.TYPE_BLUETOOTH_SCO) hit = d; break;
                    default: break;
                }
            }
            if ("bluetooth".equals(in) && Build.VERSION.SDK_INT >= 31) {
                // Bluetooth のマイクは通話の経路でしか使えない。通話の機器を Bluetooth にする（止めるときに戻す）
                for (AudioDeviceInfo d : am.getAvailableCommunicationDevices()) {
                    if (d.getType() == AudioDeviceInfo.TYPE_BLUETOOTH_SCO) {
                        commDevSet = am.setCommunicationDevice(d);
                        break;
                    }
                }
                if (hit == null) for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_INPUTS)) if (d.getType() == AudioDeviceInfo.TYPE_BLUETOOTH_SCO) hit = d;
            }
            return hit != null ? hit : builtin;
        }

        private void clearCommDevice(AudioManager am) {
            if (commDevSet && Build.VERSION.SDK_INT >= 31) {
                try { am.clearCommunicationDevice(); } catch (RuntimeException ignored) {}
            }
            commDevSet = false;
        }

        private boolean builtinStereo(AudioManager am) {
            for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
                if (d.getType() != AudioDeviceInfo.TYPE_BUILTIN_MIC) continue;
                for (int c : d.getChannelCounts()) if (c >= 2) return true;
            }
            return false;
        }

        private void effectsOn(int sid) {
            try {
                if (AcousticEchoCanceler.isAvailable()) { aec = AcousticEchoCanceler.create(sid); if (aec != null) aec.setEnabled(true); }
                if (NoiseSuppressor.isAvailable()) { ns = NoiseSuppressor.create(sid); if (ns != null) ns.setEnabled(true); }
            } catch (RuntimeException ignored) {
            }
        }

        private String detailText() {
            String khz = (promise.rate % 1000 == 0 ? String.valueOf(promise.rate / 1000) : String.format(java.util.Locale.US, "%.1f", promise.rate / 1000.0)) + " kHz";
            String ch = ctx.getString(promise.channels >= 2 ? R.string.mic_stereo_short : R.string.mic_mono_short);
            String bits = ctx.getString(promise.format == FMT_F32 ? R.string.mic_bits32 : promise.format == FMT_S24 ? R.string.mic_bits24 : R.string.mic_bits16);
            String sep = ctx.getString(R.string.sep);
            return khz + sep + ch + sep + bits;
        }

        private String deviceName(AudioDeviceInfo d) {
            switch (d.getType()) {
                case AudioDeviceInfo.TYPE_BUILTIN_MIC: return ctx.getString(R.string.mic_in_builtin);
                case AudioDeviceInfo.TYPE_WIRED_HEADSET: return ctx.getString(R.string.mic_in_wired);
                case AudioDeviceInfo.TYPE_USB_DEVICE: case AudioDeviceInfo.TYPE_USB_HEADSET: return ctx.getString(R.string.mic_usb_name, String.valueOf(d.getProductName()));
                case AudioDeviceInfo.TYPE_BLUETOOTH_SCO: return ctx.getString(R.string.mic_bt_name, String.valueOf(d.getProductName()));
                default: return String.valueOf(d.getProductName());
            }
        }

        private int kindOf(AudioDeviceInfo d) {
            if (d == null) return 0;
            switch (d.getType()) {
                case AudioDeviceInfo.TYPE_BUILTIN_MIC: return 1;
                case AudioDeviceInfo.TYPE_WIRED_HEADSET: return 2;
                case AudioDeviceInfo.TYPE_USB_DEVICE: case AudioDeviceInfo.TYPE_USB_HEADSET: return 3;
                case AudioDeviceInfo.TYPE_BLUETOOTH_SCO: return 4;
                default: return 5;
            }
        }

        private void startMicLoop() {
            Thread t = new Thread(this::micLoop, "audio-mic");
            synchronized (micLock) { micThread = t; }
            t.start();
        }

        // マイクだけ止める（つなぎと、伝えた形はそのまま）
        private void stopMic() {
            AudioRecord r;
            Thread t;
            synchronized (micLock) {
                r = rec;
                rec = null;
                t = micThread;
                micThread = null;
                if (aec != null) { aec.release(); aec = null; }
                if (ns != null) { ns.release(); ns = null; }
                clearCommDevice(ctx.getSystemService(AudioManager.class));
            }
            if (r != null) { try { r.stop(); } catch (RuntimeException ignored) {} }
            if (t != null) try { t.join(500); } catch (InterruptedException ignored) {}
            if (r != null) r.release();
        }

        // 画面でマイクの設定を変えた：同じつなぎのまま、伝えた形でマイクを作り直す
        void restartMic(String why) {
            if (closed || !wantMic) return;
            new Thread(() -> {
                boolean was;
                synchronized (micLock) { was = rec != null; }
                if (!was) return;
                Logbook.i("音：" + why + "。マイクを作り直します", "Audio: " + why + ". Restarting the mic");
                stopMic();
                if (openMic(false)) startMicLoop();
                else { micActive = false; micRetrying = true; micRetryAt = SystemClock.elapsedRealtime() + 1000; }
            }, "audio-mic-restart").start();
        }

        private void stopDevices() {
            AudioTrack t = track;
            track = null;
            if (t != null) { try { t.pause(); t.flush(); t.stop(); } catch (RuntimeException ignored) {} t.release(); }
            stopMic();
        }

        // ---- 糸 ----

        // スピーカー：ためから読み、AudioTrack に書く。書き込みが機器の時計で止まる
        private void playLoop() {
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
            AudioTrack t = track;
            PlayoutRing r = ring;
            int frames = Math.max(64, spkRate / 200); // 5 ms
            float[] buf = new float[frames * spkCh];
            while (!closed && t != null) {
                r.read(buf, frames);
                int n = t.write(buf, 0, buf.length, AudioTrack.WRITE_BLOCKING);
                if (n < 0) { Logbook.i("音：スピーカーに書けません（" + n + "）", "Audio: cannot write to the speaker (" + n + ")"); break; }
                spkFrames += frames;
            }
        }

        // マイク：10 ms ずつ読み、音量を掛けて、伝えた ch と形にして送る。PC が受け切れていなければ、捨てて番号だけ進める（PC は抜けを無音で埋める）
        private void micLoop() {
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
            AudioRecord r;
            int enc, rch;
            synchronized (micLock) { r = rec; enc = recEnc; rch = recCh; }
            if (r == null) return;
            micActive = true;
            micRetrying = false;
            int chunk = promise.rate / 100, och = promise.channels, fmt = promise.format;
            int bps = fmt == FMT_F32 ? 4 : fmt == FMT_S24 ? 3 : 2;
            float[] fin = new float[chunk * rch];
            short[] sin = enc == AudioFormat.ENCODING_PCM_16BIT ? new short[chunk * rch] : null;
            float[] out = new float[chunk * och];
            while (!closed) {
                synchronized (micLock) { if (rec != r) return; }
                int got = 0, need = chunk * rch;
                while (got < need && !closed) {
                    int n = sin != null ? r.read(sin, got, need - got) : r.read(fin, got, need - got, AudioRecord.READ_BLOCKING);
                    if (n < 0) {
                        synchronized (micLock) { if (rec != r) return; }
                        Logbook.i("音：マイクを読めません（" + n + "）", "Audio: cannot read the mic (" + n + ")");
                        micActive = false;
                        return;
                    }
                    got += n;
                }
                if (closed) break;
                if (sin != null) for (int i = 0; i < need; i++) fin[i] = sin[i] * (1f / 32768f);
                // 音量を掛けて -1〜1 に収め、伝えた ch にする（足りない ch は増やし、多ければ混ぜる）
                float g = MicLevel.factor(), top = 0;
                for (int i = 0; i < chunk; i++) {
                    float l = fin[i * rch], rr = rch == 2 ? fin[i * rch + 1] : l;
                    l = Math.max(-1f, Math.min(1f, l * g));
                    rr = Math.max(-1f, Math.min(1f, rr * g));
                    if (och == 2) { out[i * 2] = l; out[i * 2 + 1] = rr; }
                    else out[i] = rch == 2 ? (l + rr) * 0.5f : l;
                    top = Math.max(top, Math.max(Math.abs(l), Math.abs(rr)));
                }
                MicLevel.note(top);
                if (pendingMic.get() > 20) {
                    micDropped += chunk;
                    micIndex += chunk;
                    continue;
                }
                int len = chunk * och * bps;
                ByteBuffer o = ByteBuffer.allocate(16 + len).order(ByteOrder.LITTLE_ENDIAN);
                o.put((byte) T_MIC).put((byte) 0).putShort((short) chunk).putInt(len).putLong(micIndex);
                for (float v : out) {
                    if (fmt == FMT_F32) o.putFloat(v);
                    else if (fmt == FMT_S24) {
                        int x = Math.round(v * 8388607f);
                        o.put((byte) x).put((byte) (x >> 8)).put((byte) (x >> 16));
                    } else o.putShort((short) Math.round(v * 32767f));
                }
                pendingMic.incrementAndGet();
                send(o.array());
                micIndex += chunk;
                micSent += chunk;
            }
        }

        // 1 秒ごとに状態を PC に送る（proto.h の IpaStatsExt）。マイクを待っていれば試し直す。5 秒ごとに記録にも残す
        private void statsLoop() {
            int n = 0;
            while (!closed) {
                SystemClock.sleep(1000);
                if (closed) break;
                boolean running;
                synchronized (micLock) { running = rec != null; }
                micDenied = wantMic && ctx.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED;
                if (wantMic && !running && !micDenied && !micBlocked && SystemClock.elapsedRealtime() >= micRetryAt) {
                    if (openMic(false)) {
                        Logbook.i("音：マイクを始めました。PC にはこのつなぎのまま送ります", "Audio: mic started. Sending to the PC on the same connection");
                        startMicLoop();
                        micRetryDelay = 1000;
                        running = true;
                    } else {
                        micRetryAt = SystemClock.elapsedRealtime() + micRetryDelay;
                        Logbook.i("音：マイクを始められませんでした。" + micRetryDelay / 1000 + " 秒後にもう一度試します", "Audio: could not start the mic. Trying again in " + micRetryDelay / 1000 + " s");
                        micRetryDelay = Math.min(micRetryDelay * 2, 30_000);
                        micRetrying = true;
                    }
                }
                ByteBuffer o = ByteBuffer.allocate(16 + 36).order(ByteOrder.LITTLE_ENDIAN);
                o.put((byte) T_STATS).put((byte) 0).putShort((short) 0).putInt(36).putLong(0);
                PlayoutRing r = ring;
                if (r != null) {
                    PlayoutRing.Snap sn = r.snapshot();
                    o.putInt((int) Math.max(0, sn.fill)).putInt((int) sn.target).putInt(sn.under).putInt(sn.over).putInt(sn.gap).putInt(sn.ratioPpm);
                    fillMs = sn.fill * 1000.0 / spkRate;
                } else {
                    for (int i = 0; i < 6; i++) o.putInt(0);
                }
                o.putInt((int) micSent).putInt((int) micDropped);
                int st = 0;
                if (ScreenState.foreground > 0) st |= S_FRONT;
                if (running) st |= S_MIC_RUNNING;
                if (wantMic && !running && !micDenied && !micBlocked) st |= S_MIC_WAITING;
                if (wantMic && (micDenied || micBlocked)) st |= S_MIC_DENIED;
                o.put((byte) st).put((byte) (running ? inputKind : 0)).put((byte) 0).put((byte) 0);
                send(o.array());
                if (++n % 5 == 0) {
                    float pk = spkPeak;
                    spkPeak = 0;
                    Logbook.i(String.format(java.util.Locale.US, "音：スピーカー %d フレーム再生・ピーク %.3f・ため %.0f ms／マイク %d フレーム送信・%d 捨て",
                            spkFrames, pk, fillMs, micSent, micDropped), String.format(java.util.Locale.US, "Audio: speaker %d frames played, peak %.3f, buffer %.0f ms / mic %d frames sent, %d dropped", spkFrames, pk, fillMs, micSent, micDropped));
                }
            }
        }

        private void readLoop(DataInputStream in) throws IOException {
            byte[] hd = new byte[16];
            byte[] body = new byte[4096];
            while (!closed) {
                in.readFully(hd);
                ByteBuffer bb = ByteBuffer.wrap(hd).order(ByteOrder.LITTLE_ENDIAN);
                int type = hd[0] & 0xff;
                int flags = hd[1] & 0xff;
                int frames = bb.getShort(2) & 0xffff;
                long length = bb.getInt(4) & 0xffffffffL;
                long index = bb.getLong(8);
                if (length > MAX_PAYLOAD) { close(Logbook.t("長さ " + length + " が上限を超えた", "length " + length + " is over the limit")); return; }
                if (body.length < length) body = new byte[(int) length];
                in.readFully(body, 0, (int) length);
                switch (type) {
                    case T_SPK: {
                        PlayoutRing r = ring;
                        if (r == null) break;
                        if (index > spkExpected) { // 抜け。無音で埋めて時間の流れを保つ
                            r.writeSilence((int) Math.min(index - spkExpected, 96_000));
                            spkExpected = index;
                        } else if (index < spkExpected) {
                            break; // 古い塊
                        }
                        if ((flags & FR_SILENT) != 0) r.writeSilence(frames);
                        else {
                            int n = Math.min(frames, (int) (length / (2 * spkCh)));
                            float pk = r.writeS16(body, n);
                            if (pk > spkPeak) spkPeak = pk;
                        }
                        spkExpected += frames;
                        break;
                    }
                    case T_PING: {
                        ByteBuffer o = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
                        o.put((byte) T_PONG).put((byte) 0).putShort((short) 0).putInt(0).putLong(index);
                        send(o.array());
                        break;
                    }
                    case T_BYE:
                        close(Logbook.t("PC が終えた", "the PC ended"));
                        return;
                    default:
                        break;
                }
            }
        }

        private void writeLoop() {
            try {
                OutputStream out = link.out();
                for (;;) {
                    byte[] m = q.take();
                    if (m == POISON || closed) break;
                    out.write(m);
                    if (m[0] == T_MIC) pendingMic.decrementAndGet();
                }
            } catch (IOException | InterruptedException ignored) {
            }
            close(Logbook.t("送れない", "send error"));
        }

        private void sendAck(boolean spkOk, boolean micOk, boolean later) {
            AudioManager am = ctx.getSystemService(AudioManager.class);
            int hwRate = parse(am.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE), 48000);
            int fpb = parse(am.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER), 256);
            long ioUs = hwRate > 0 ? fpb * 1_000_000L / hwRate : 0;
            long outUs = track != null ? track.getBufferSizeInFrames() * 1_000_000L / spkRate : 0;
            int inKind, flags2 = 0, hwIn;
            long inUs;
            synchronized (micLock) {
                inUs = rec != null ? rec.getBufferSizeInFrames() * 1_000_000L / promise.rate / 2 : 0;
                inKind = rec != null ? inputKind : 0;
                if (stereoCapable) flags2 |= A2_STEREO_CAPABLE;
                if (rec != null && recCh == 2 && promise.channels == 2) flags2 |= A2_STEREO_ON;
                hwIn = rec != null ? recCh : 0;
            }
            int usedMode = wantMic ? micMode() : 0;
            ByteBuffer o = ByteBuffer.allocate(40).order(ByteOrder.LITTLE_ENDIAN);
            o.put(new byte[] { 'I', 'P', 'A', 'K' });
            o.putShort((short) 1);
            o.putShort((short) ((spkOk ? A_SPEAKER_OK : 0) | (micOk ? A_MIC_OK : 0) | (later ? A_MIC_LATER : 0)));
            // 前に出ていなくても、あとで始めたら必ずこの rate・ch・形で送る
            o.putInt(promise.rate).putShort((short) promise.channels).putShort((short) promise.format);
            o.putInt(hwRate).putInt((int) ioUs).putInt((int) outUs).putInt((int) inUs);
            o.put((byte) usedMode).put((byte) inKind).put((byte) flags2).put((byte) hwIn);
            send(o.array());
            Logbook.i("音：始めました スピーカー=" + spkOk + " マイク=" + micOk + (later ? "（あとで始める）" : "") + " 形 " + promise.rate + " Hz・" + promise.channels + " ch・"
                    + fmtName(promise.format) + " 種類 " + usedMode + " hw=" + hwRate + " Hz・" + fpb + " フレーム", "Audio: started speaker=" + spkOk + " mic=" + micOk + (later ? " (later)" : "") + " format " + promise.rate + " Hz, " + promise.channels + " ch, " + fmtName(promise.format) + " mode " + usedMode + " hw=" + hwRate + " Hz, " + fpb + " frames");
        }
    }

    private static String fmtName(int f) {
        return f == FMT_F32 ? "float32" : f == FMT_S24 ? Logbook.t("24 ビット", "24-bit") : Logbook.t("16 ビット", "16-bit");
    }

    private static int parse(String s, int def) {
        try { return s == null ? def : Integer.parseInt(s); } catch (NumberFormatException e) { return def; }
    }

    // ---- 鳴らす側の「ため」（proto.h の IpaPlayout・AudioBridge.swift の PlayoutRing と同じ中身）----
    static final class PlayoutRing {
        private final int ch, cap;
        private final float[] buf;
        private final double rate, target;
        private long wr;
        private double rd, fs, integ, ratio = 1.0;
        private boolean running;
        private int under, over, gap;

        PlayoutRing(int ch, double rate, double targetMs) {
            this.ch = ch;
            this.rate = rate;
            this.cap = (int) (rate * 2);
            this.buf = new float[cap * ch];
            this.target = rate * targetMs / 1000.0;
        }

        // s16le の PCM を frames フレーム積む。いちばん大きい振れ幅（0〜1）を返す
        synchronized float writeS16(byte[] pcm, int frames) {
            float pk = 0;
            for (int i = 0; i < frames; i++) {
                int at = (int) (wr % cap) * ch;
                for (int c = 0; c < ch; c++) {
                    int o = (i * ch + c) * 2;
                    float v = (short) ((pcm[o] & 0xff) | (pcm[o + 1] << 8)) * (1.0f / 32768.0f);
                    buf[at + c] = v;
                    float a = Math.abs(v);
                    if (a > pk) pk = a;
                }
                wr++;
            }
            trim();
            return pk;
        }

        synchronized void writeSilence(int frames) {
            gap += frames;
            int n = Math.min(frames, cap);
            for (int i = 0; i < n; i++) {
                int at = (int) (wr % cap) * ch;
                for (int c = 0; c < ch; c++) buf[at + c] = 0;
                wr++;
            }
            trim();
        }

        // 多すぎる（止まりのあとの追いつき）なら、目標まで古い方を捨てる
        private void trim() {
            double fill = wr - rd;
            if (fill > target * 2 + 0.05 * rate) {
                rd = wr - target;
                over++;
            }
        }

        // 機器の時計で frames フレームを読む。足りない所は無音
        synchronized void read(float[] out, int frames) {
            double fill = wr - rd;
            if (!running) {
                if (fill >= target) { running = true; fs = fill; }
                else { java.util.Arrays.fill(out, 0, frames * ch, 0f); return; }
            }
            fs += 0.02 * (fill - fs);
            double err = fs - target;
            double dt = frames / rate;
            double adjP = err / (4.0 * rate);
            integ = Math.max(-0.005, Math.min(0.005, integ + adjP * dt / 20.0));
            double adj = Math.max(-0.005, Math.min(0.005, adjP + integ));
            ratio = 1.0 + adj;
            for (int i = 0; i < frames; i++) {
                long i0 = (long) Math.floor(rd);
                if (i0 + 1 >= wr) { // 足りない。残りは無音にして、ためなおしを待つ
                    java.util.Arrays.fill(out, i * ch, frames * ch, 0f);
                    running = false;
                    under++;
                    return;
                }
                float fr = (float) (rd - i0);
                int a = (int) (i0 % cap) * ch, b = (int) ((i0 + 1) % cap) * ch;
                for (int c = 0; c < ch; c++) out[i * ch + c] = buf[a + c] + (buf[b + c] - buf[a + c]) * fr;
                rd += ratio;
            }
        }

        static final class Snap {
            double fill, target;
            int under, over, gap, ratioPpm;
        }

        synchronized Snap snapshot() {
            Snap s = new Snap();
            s.fill = wr - rd;
            s.target = target;
            s.under = under;
            s.over = over;
            s.gap = gap;
            s.ratioPpm = (int) ((ratio - 1.0) * 1e6);
            return s;
        }
    }
}
