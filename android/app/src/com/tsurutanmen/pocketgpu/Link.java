package com.tsurutanmen.pocketgpu;

import android.net.Credentials;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.SystemClock;
import android.system.Os;
import android.system.OsConstants;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;

// 1 本のつなぎ（USB の adb か、TCP）。CameraBridge と AudioBridge が使う。
//
// 0.1.2 で直した（バグ探しの指摘 1）：前は USB も 127.0.0.1 の TCP で待っていたので、同じ端末のほかのアプリが 127.0.0.1:50064・50063 に
// つなげば、カメラとマイクの権限なしで映像と音を受けられた。Android 10 からアプリは /proc/net/tcp を読めず、TCP の相手の uid も分からない。
// そこで USB は抽象名の UNIX ソケット（localabstract）で待ち、相手の uid が adb（shell の 2000 か、root の 0）のときだけ受ける。
// PC では adb forward tcp:50064 localabstract:pocketgpu-webcam のように向ける。PC のツール（webcam_rx・ipaudio）はそのまま使える。
// 古い向け方（adb forward tcp:50064 tcp:50064）は、設定で「古い PC のツールも受け付ける」を入れたときだけ受ける（UsbPort ではなく TcpPort）
abstract class Link {
    static final int UID_ROOT = 0, UID_SHELL = 2000; // adbd は user ビルドで shell、エミュレーターや userdebug では root のことがある

    final boolean remote; // Wi-Fi の相手（合言葉が要る）
    final String desc;

    Link(boolean remote, String desc) {
        this.remote = remote;
        this.desc = desc;
    }

    abstract InputStream in() throws IOException;
    abstract OutputStream out() throws IOException;
    abstract void close();

    static final class Tcp extends Link {
        final Socket s;
        Tcp(Socket s, boolean remote) {
            super(remote, (remote ? "Wi-Fi " : "TCP ") + s.getInetAddress().getHostAddress());
            this.s = s;
        }
        @Override InputStream in() throws IOException { return s.getInputStream(); }
        @Override OutputStream out() throws IOException { return s.getOutputStream(); }
        @Override void close() { try { s.close(); } catch (IOException ignored) {} }
    }

    static final class Local extends Link {
        final LocalSocket s;
        Local(LocalSocket s, int uid) {
            super(false, Logbook.t("USB（adb・uid " + uid + "）", "USB (adb, uid " + uid + ")"));
            this.s = s;
        }
        @Override InputStream in() throws IOException { return s.getInputStream(); }
        @Override OutputStream out() throws IOException { return s.getOutputStream(); }
        @Override void close() {
            try { s.shutdownInput(); } catch (IOException | RuntimeException ignored) {}
            try { s.shutdownOutput(); } catch (IOException | RuntimeException ignored) {}
            try { s.close(); } catch (IOException ignored) {}
        }
    }

    interface Sink {
        void accepted(Link l);
    }

    // ---- USB：抽象名の UNIX ソケットで待ち、adb からの接続だけを受ける ----
    static final class UsbPort {
        private final String name, who, whoEn;
        private final Sink sink;
        private volatile boolean enabled;
        private volatile int gen;
        private LocalServerSocket server; // this で守る
        volatile long lastRejectAt;       // ほかのアプリを断った時刻（画面に出す）
        volatile int lastRejectUid = -1;

        UsbPort(String name, String who, String whoEn, Sink sink) {
            this.name = name;
            this.who = who;
            this.whoEn = whoEn;
            this.sink = sink;
        }

        synchronized void setEnabled(boolean on) {
            if (on == enabled) return;
            enabled = on;
            if (on) {
                final int g = ++gen;
                new Thread(() -> loop(g), who + "-usb-accept").start();
            } else {
                closeServer();
            }
        }

        private void closeServer() {
            LocalServerSocket s;
            synchronized (this) {
                s = server;
                server = null;
            }
            if (s == null) return;
            // close だけでは accept が戻らない版があるので、先に shutdown する
            try { Os.shutdown(s.getFileDescriptor(), OsConstants.SHUT_RDWR); } catch (Exception ignored) {}
            try { s.close(); } catch (IOException ignored) {}
        }

        private void loop(int g) {
            boolean failNoted = false;
            while (enabled && g == gen) {
                LocalServerSocket ss;
                try {
                    ss = new LocalServerSocket(name);
                } catch (IOException e) {
                    if (!failNoted) Logbook.i(who + "：USB の口 " + name + " を開けません（" + e.getMessage() + "）。1 秒ごとに開き直します", whoEn + ": cannot open the USB socket " + name + " (" + e.getMessage() + "). Retrying every second");
                    failNoted = true;
                    SystemClock.sleep(1000);
                    continue;
                }
                synchronized (this) {
                    if (!enabled || g != gen) { try { ss.close(); } catch (IOException ignored) {} break; }
                    server = ss;
                }
                failNoted = false;
                Logbook.i(who + "：USB（adb の localabstract:" + name + "）で待っています", whoEn + ": waiting on USB (adb localabstract:" + name + ")");
                try {
                    for (;;) {
                        LocalSocket s = ss.accept();
                        int uid = -1;
                        try {
                            Credentials cr = s.getPeerCredentials();
                            uid = cr.getUid();
                        } catch (IOException | RuntimeException ignored) {
                        }
                        if (uid != UID_SHELL && uid != UID_ROOT) {
                            lastRejectAt = System.currentTimeMillis();
                            lastRejectUid = uid;
                            Logbook.i(who + "：adb ではない接続（uid " + uid + "）を断りました。この端末のほかのアプリが接続しようとしました", whoEn + ": refused a connection that is not from adb (uid " + uid + "). Another app on this phone tried to connect");
                            try { s.close(); } catch (IOException ignored) {}
                            continue;
                        }
                        sink.accepted(new Local(s, uid));
                    }
                } catch (IOException e) {
                    // 止めたとき（closeServer）はここに来る
                } finally {
                    synchronized (this) { if (server == ss) server = null; }
                    try { ss.close(); } catch (IOException ignored) {}
                }
                if (enabled && g == gen) SystemClock.sleep(200);
            }
        }
    }

    // ---- TCP：Wi-Fi（0.0.0.0）か、古い PC のツール向けの 127.0.0.1 ----
    // 127.0.0.1 から来た接続は、loopback のときだけ渡す（古い形の adb forward と、同じ端末のほかのアプリを区別できないため）。
    // loopback が切れていても 127.0.0.1 では待ち、来た接続は何も読まずにすぐ切る。画面に「古い形のコマンドで接続された」と出すため
    static final class TcpPort {
        private final int port;
        private final String who, whoEn;
        private final Sink sink;
        private volatile boolean enabled, all, loopback, rebinding;
        private volatile int gen;
        private ServerSocket server; // this で守る
        volatile long lastLoopbackRejectAt;

        TcpPort(int port, String who, String whoEn, Sink sink) {
            this.port = port;
            this.who = who;
            this.whoEn = whoEn;
            this.sink = sink;
        }

        // on：待つ。all：0.0.0.0 で待つ（Wi-Fi。偽なら 127.0.0.1 だけ）。loopback：127.0.0.1 から来た接続を受ける
        synchronized void configure(boolean on, boolean all, boolean loopback) {
            boolean rebind = on && enabled && all != this.all;
            this.all = all;
            this.loopback = loopback;
            if (on == enabled && !rebind) return;
            if (rebind) {
                rebinding = true;
                closeServer();
                return;
            }
            enabled = on;
            if (on) {
                final int g = ++gen;
                new Thread(() -> loop(g), who + "-tcp-accept").start();
            } else {
                closeServer();
            }
        }

        // Wi-Fi の受け付けをやめたあと、もう一度開く（合言葉の間違いが続いたとき）
        void rebind() {
            rebinding = true;
            closeServer();
        }

        private void closeServer() {
            ServerSocket s;
            synchronized (this) {
                s = server;
                server = null;
            }
            if (s != null) try { s.close(); } catch (IOException ignored) {}
        }

        private void loop(int g) {
            boolean failNoted = false;
            while (enabled && g == gen) {
                ServerSocket ss = null;
                try {
                    ss = new ServerSocket();
                    ss.setReuseAddress(true);
                    boolean w = all;
                    ss.bind(new InetSocketAddress(InetAddress.getByName(w ? "0.0.0.0" : "127.0.0.1"), port), 4);
                    synchronized (this) {
                        if (!enabled || g != gen) { ss.close(); break; }
                        if (all != w) continue; // 開いている間に設定が変わった。開き直す
                        server = ss;
                    }
                    rebinding = false;
                    failNoted = false;
                    Logbook.i(who + "：" + (w ? "Wi-Fi と 127.0.0.1" : "127.0.0.1") + " のポート " + port + " で待っています" + (loopback ? "（古い PC のツールも受け付ける）" : "（127.0.0.1 は古い形の接続を断るだけ）"), whoEn + ": waiting on " + (w ? "Wi-Fi and 127.0.0.1" : "127.0.0.1") + " port " + port + (loopback ? " (old PC tools accepted)" : " (old-style connections on 127.0.0.1 are refused)"));
                    for (;;) {
                        Socket s = ss.accept();
                        s.setTcpNoDelay(true);
                        boolean remote = !s.getInetAddress().isLoopbackAddress();
                        if (!remote && !loopback) {
                            lastLoopbackRejectAt = System.currentTimeMillis();
                            Logbook.i(who + "：127.0.0.1 の TCP への接続を断りました。PC は adb forward tcp:" + port + " localabstract:… の形で接続してください"
                                    + "（古い PC のツールを使うときは、設定で「古い PC のツールも受け付ける」をオンにします）", whoEn + ": refused a TCP connection to 127.0.0.1. On the PC, use adb forward tcp:" + port + " localabstract:... (to use old PC tools, turn on Also accept old PC tools in Settings)");
                            try { s.close(); } catch (IOException ignored) {}
                            continue;
                        }
                        sink.accepted(new Tcp(s, remote));
                    }
                } catch (IOException e) {
                    if (!enabled || g != gen || rebinding) continue;
                    if (!failNoted) Logbook.i(who + "：ポート " + port + " を開けません（" + e.getMessage() + "）。1 秒ごとに開き直します", whoEn + ": cannot open port " + port + " (" + e.getMessage() + "). Retrying every second");
                    failNoted = true;
                    SystemClock.sleep(1000);
                } finally {
                    if (ss != null) try { ss.close(); } catch (IOException ignored) {}
                }
            }
        }
    }
}
