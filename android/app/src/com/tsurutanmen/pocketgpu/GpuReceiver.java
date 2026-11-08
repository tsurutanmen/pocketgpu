package com.tsurutanmen.pocketgpu;

import android.content.Context;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.SystemClock;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;

import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

// GPU の受信アプリ（app/vk/remote/server.c を build_android.sh で組んだ実行ファイル）を、このアプリの子のプロセスとして動かす。
// APK の lib/<abi>/libpgpu_server.so に入れておき、端末が取り出した nativeLibraryDir から起動する（Android 10 からは、
// アプリが書ける場所の実行ファイルは起動できないが、nativeLibraryDir の物は起動できる）。
// 別のプロセスにする理由：server.c の main をそのまま使える・落ちてもカメラと音は止まらない・止めるのはプロセスを終わらせるだけでよい。
// 0.1.3 までは 127.0.0.1:50060 だけで待った（PC は adb forward tcp:50300 tcp:50060 のあと pocketgpu_android.cmd）。0.1.4 からは下の「0.1.4 で直した」
//
// 0.1.2 で直した（テスターの報告：アプリと キットの android\start_android.cmd を両方使うと分かりにくい）：
//   キットの受け手（/data/local/tmp/pgpu_server。端末の 127.0.0.1:50300 で待つ）が動いているときは、アプリの受け手は起こさず、
//   画面に「キットの受け手を使っています」と出す。PC はキットの手順（start_android.cmd の forward tcp:50300 tcp:50300）のまま使える。
//   どちらも PC の 50300 番を使うので、両方を動かすと、あとから adb forward を打った方にだけつながって分かりにくかった。
//   キットの受け手が動いているかは、10 秒ごとに 127.0.0.1:50300 を bind できるかで調べる（つながないので、受け手の記録は増えない）。
//   受け手が終わったときは、終わりのコードと受け手の記録の最後の行を、画面の状態の行とログに出す
//
// 0.1.3 で足した：
//   受け手の記録を画面の言語で出す（起動のとき環境変数 PGPU_LANG=en|ja で渡す。server.c が英語と日本語の両方を持つ）
//   前にこのアプリが起動した受け手が残って口（50060）をつかんだままになることがある（テスターの報告：アプリの本体が落ちたあとなど）。
//   shell からは止められないので、起動の前に /proc から自分の uid の受け手（実行ファイル libpgpu_server.so）を探して止める
//   口がふさがっている理由が自分の古い受け手でもキットの受け手でもないときは、画面に分かりやすく出す
//
// 0.1.4 で直した（テスターの報告：Redmagic 9S Pro で、受け手が「127.0.0.1:50060 で待てない」ですぐ終わる。端末の再起動でも同じ）：
//   ・待つ口は、受け手ではなくアプリ（この Java）が開ける。開けた口の fd を、抽象名の UNIX ソケットで受け手に渡す（SCM_RIGHTS）。
//     受け手は PGPU_FD_SOCK の名前につないで受け取る。カメラと音の口はこのプロセスで開けられていたので、子のプロセスだけが
//     開けない場合（機種の独自の制限など）でも動く
//   ・50060 で待てなければ 50061・50062・50065〜50069 を順に試す（50063・50064 は音とカメラの古い形の TCP が使う）。
//     使えなかった番号と理由（errno の名前）と、使った番号を、ログと画面に出す。Android 14 からは、端末の特権を持つ部分が
//     特定の番号の bind を止められる（ConnectivityNative の blockPortForBind。止められた bind は EPERM）
//   ・USB 用に抽象名の UNIX ソケット（localabstract:pocketgpu-gpu）でも待つ。番号がぶつからない。PC は
//     adb forward tcp:50300 localabstract:pocketgpu-gpu。受け手が adb（uid 2000・0）と自分の uid だけ相手をする
//   ・fd の受け渡しが失敗したら（受け手が 4 で終わる・来ない）、次からは受け手に自分で開けさせる（PGPU_PORT_TRY・PGPU_UNIX）
//
// 0.1.5 で足した：
//   ・PC とつながっているか（受け手の記録の「つながった」「切れた」の行）と、この数秒で GPU の仕事が来ているか（受け手の CPU 時間の増え方）を調べる。
//     受け手は別のプロセスなので、アプリから見えるのは記録と /proc/<pid>/stat（同じ uid なので読める）だけ。つなぎ直したり探りを入れたりはしない
//   ・つながっていて仕事がある間（仕事が 2 分来なければやめる）、BridgeService が画面を点けたままにする（keepScreen）
//
// 0.1.6 で足した：受け手の pid を serverPid に出す（PerfBoost が使う）
final class GpuReceiver {
    static final int PORT = 50060;
    // 50060 で待てないときに試す番号（はじめは 50060）。50063・50064 は音とカメラ（古い PC のツール向けの TCP）が使うので飛ばす
    static final int[] PORTS = { 50060, 50061, 50062, 50065, 50066, 50067, 50068, 50069 };
    static final String USB_NAME = "pocketgpu-gpu"; // PC は adb forward tcp:50300 localabstract:pocketgpu-gpu
    static final int EXIT_NO_FDS = 4;               // 受け手が、アプリから口を受け取れなかったときの終わりのコード
    static final int KIT_PORT = 50300; // キットの start_android.cmd の受け手が端末で待つ番号（PGPU_PORT の初めの値）
    private static final String EXE = "libpgpu_server.so";

    private final Context ctx;
    private final File exe, log;
    private java.lang.Process proc; // this で守る
    private volatile boolean enabled;
    private volatile int gen;
    private final List<Long> starts = new ArrayList<>();
    private boolean gaveUp;          // 1 分に 5 回より多く終わった。スイッチを入れ直すまで起こさない
    private volatile int staleStopped; // 今回の起動の前に止めた、前の受け手の数
    private boolean selfBind;          // fd の受け渡しが失敗した。受け手に自分で口を開けさせる（スイッチを入れ直すと戻す）
    private FileDescriptor tcpFd;      // アプリが開けた TCP の口。受け手が終わるまで持つ（this で守る）
    private LocalServerSocket usbSrv;  // アプリが開けた USB（UNIX）の口。同じく受け手が終わるまで持つ
    private volatile boolean handedOver;
    private int lastGoodPort = PORT;

    // 画面から読む：いま待っている口
    volatile int port;          // TCP の番号（0 は TCP なし）
    volatile boolean usb;       // USB（localabstract:pocketgpu-gpu）で待っている
    volatile String portNote = ""; // 50060 を使えなかったときの理由（画面の言語）

    // 画面から読む
    volatile String state = "off";   // off・starting・running・failed・kit
    volatile String reason = "";     // failed のときの理由（画面の言語）

    // 0.1.5：画面に出す・画面を点けたままにするための知らせ（アプリの受け手のとき）
    static final long WORK_WINDOW_MS = 5_000;      // この間に CPU 時間が増えていれば「仕事が来ている」
    static final long KEEP_AWAKE_IDLE_MS = 120_000; // つながっていても、仕事がこれだけ来なければ画面を点けたままにするのをやめる
    volatile boolean pcConnected;   // 受け手の記録の最後の「つながった／切れた」が「つながった」
    volatile long connectedAt;      // つながった時刻（SystemClock.elapsedRealtime）
    volatile long lastWorkAt;       // 最後に仕事が来た時刻。0 は無し
    volatile int serverPid = -1;    // 0.1.6：アプリが起動した受け手の pid（見つかっている間だけ。無ければ -1）

    GpuReceiver(Context ctx) {
        this.ctx = ctx.getApplicationContext();
        exe = new File(this.ctx.getApplicationInfo().nativeLibraryDir, EXE);
        log = new File(this.ctx.getFilesDir(), "pgpu_server.log");
    }

    boolean available() {
        return exe.exists();
    }

    File logFile() {
        return log;
    }

    synchronized void setEnabled(boolean on) {
        if (on == enabled) return;
        enabled = on;
        if (on) {
            starts.clear();
            gaveUp = false;
            selfBind = false;
            reason = "";
            final int g = ++gen;
            new Thread(() -> supervise(g), "gpu-supervise").start(); // つないで調べるので、画面の糸ではなく自分の糸で
        } else {
            gen++;
            stop();
            state = "off";
        }
    }

    // キットの受け手を見ながら、アプリの受け手を起こす・止める
    private void supervise(int g) {
        boolean kitNoted = false;
        while (enabled && g == gen) {
            boolean kit = kitRunning();
            if (kit) {
                if (!kitNoted) {
                    Logbook.i("GPU：キットの受け手（start_android.cmd）が端末で動いています（ポート " + KIT_PORT + "）。アプリの受け手は起動せず、キットの受け手を使います。PC はキットの手順のまま使えます",
                            "GPU: the kit receiver (start_android.cmd) is running on the phone (port " + KIT_PORT + "). The app receiver is not started and the kit receiver is used. Keep using the kit steps on the PC");
                    kitNoted = true;
                }
                boolean had;
                synchronized (this) { had = proc != null; }
                if (had) stop();
                state = "kit";
                reason = "";
                startKitMon(g);
            } else {
                if (kitNoted) {
                    Logbook.i("GPU：キットの受け手が止まりました。アプリの受け手を起動します（PC は adb forward tcp:50300 tcp:" + PORT + "）",
                            "GPU: the kit receiver stopped. Starting the app receiver (on the PC: adb forward tcp:50300 tcp:" + PORT + ")");
                    kitNoted = false;
                    if ("kit".equals(state)) state = "off";
                }
                start();
            }
            SystemClock.sleep(10_000);
        }
    }

    // 端末の 127.0.0.1:50300 で誰かが待っているか。つながずに、SO_REUSEADDR を付けた Socket で bind だけしてすぐ閉じる
    // （待っている物があれば bind が断られる）。受け手につながると、受け手の記録に「つながった・切れた」が並んでしまうため
    private static boolean kitRunning() {
        Socket s = new Socket();
        try {
            s.setReuseAddress(true);
            s.bind(new InetSocketAddress("127.0.0.1", KIT_PORT));
            return false;
        } catch (java.net.BindException e) {
            return true;
        } catch (IOException e) {
            return false;
        } finally {
            try { s.close(); } catch (IOException ignored) {}
        }
    }

    private synchronized void start() {
        if (!enabled || proc != null || gaveUp) return;
        if (!available()) {
            if (!"failed".equals(state)) Logbook.i("GPU：受信アプリがこの APK に入っていません（" + exe + "）", "GPU: the receiver is not in this APK (" + exe + ")");
            state = "failed";
            reason = Logbook.t("受信アプリがこの APK に入っていません", "The receiver is not in this APK");
            return;
        }
        // 1 分に 5 回より多く落ちるなら、起こし直すのをやめる
        long now = SystemClock.elapsedRealtime();
        starts.add(now);
        while (!starts.isEmpty() && now - starts.get(0) > 60_000) starts.remove(0);
        if (starts.size() > 5) {
            gaveUp = true;
            state = "failed";
            Logbook.i("GPU：受信アプリが 1 分に 5 回より多く終わったので、起動し直すのをやめます。スイッチを入れ直すと再開します",
                    "GPU: the receiver ended more than 5 times in a minute, so it is not restarted. Turn the switch off and on to try again");
            return;
        }
        staleStopped = stopStale();
        // 待つ口をアプリが開ける（fd の受け渡しが前に失敗していれば、受け手に任せる）
        Lst l = selfBind ? new Lst() : openListeners();
        LocalServerSocket hs = null;
        try {
            int argPort = l.port > 0 ? l.port : PORT;
            ProcessBuilder pb = new ProcessBuilder(exe.getAbsolutePath(), "127.0.0.1", String.valueOf(argPort));
            pb.directory(ctx.getFilesDir());
            pb.environment().put("TMPDIR", ctx.getCacheDir().getAbsolutePath());
            pb.environment().put("HOME", ctx.getFilesDir().getAbsolutePath());
            pb.environment().put("PGPU_LANG", Logbook.ja() ? "ja" : "en"); // 受け手の記録の言語（画面の言語に合わせる）
            l.given = l.any();
            if (l.given) {
                // 開けた口を渡すための名前。同じ端末のほかのアプリに当てられないよう乱数にし、相手の uid も確かめる
                String hname = "pocketgpu-gpu-fd-" + Long.toHexString(new java.security.SecureRandom().nextLong());
                hs = new LocalServerSocket(hname);
                pb.environment().put("PGPU_FD_SOCK", hname);
            } else {
                // 受け手が自分で開ける：50060 がだめなら次の番号、USB の抽象名でも待つ
                StringBuilder tryList = new StringBuilder();
                for (int q : PORTS) if (q != PORT) tryList.append(tryList.length() > 0 ? "," : "").append(q);
                pb.environment().put("PGPU_PORT_TRY", tryList.toString());
                pb.environment().put("PGPU_UNIX", USB_NAME);
            }
            pb.redirectErrorStream(true);
            pb.redirectOutput(ProcessBuilder.Redirect.to(log));
            final java.lang.Process p = pb.start();
            proc = p;
            state = "starting";
            port = 0;
            usb = false;
            handedOver = false;
            pcConnected = false;
            lastWorkAt = 0;
            if (l.any()) {
                Logbook.i("GPU：受信アプリを起動しました（" + l.desc(true) + "）", "GPU: started the receiver (" + l.desc(false) + ")");
                final LocalServerSocket h = hs;
                new Thread(() -> handover(p, h, l), "gpu-handover").start();
            } else {
                Logbook.i("GPU：受信アプリを起動しました（ポートは受信アプリが開けます：" + PORT + " から順に試す・USB は localabstract:" + USB_NAME + "）",
                        "GPU: started the receiver (the receiver opens the ports itself: trying " + PORT + " and the next ones, USB localabstract:" + USB_NAME + ")");
            }
            new Thread(() -> watch(p, l), "gpu-watch").start();
            new Thread(() -> monitor(p), "gpu-monitor").start();
        } catch (IOException | RuntimeException e) {
            if (hs != null) try { hs.close(); } catch (IOException ignored) {}
            l.close();
            state = "failed";
            reason = Logbook.t("起動できません：", "Could not start: ") + e.getMessage();
            Logbook.i("GPU：受信アプリを起動できません：" + e.getMessage(), "GPU: could not start the receiver: " + e.getMessage());
        }
    }

    // アプリが開けた口。受け手が終わったら閉じる
    private static final class Lst {
        FileDescriptor tcp;
        LocalServerSocket usb;
        int port;
        boolean given; // 受け手に渡す形で起動した
        synchronized boolean any() { return tcp != null || usb != null; }
        String desc(boolean ja) {
            StringBuilder b = new StringBuilder();
            if (usb != null) b.append("USB localabstract:").append(USB_NAME);
            if (tcp != null) b.append(b.length() > 0 ? (ja ? "・" : ", ") : "").append("TCP 127.0.0.1:").append(port);
            return b.toString();
        }
        synchronized void close() {
            if (tcp != null) { try { Os.close(tcp); } catch (ErrnoException ignored) {} tcp = null; }
            if (usb != null) { try { usb.close(); } catch (IOException ignored) {} usb = null; }
        }
    }

    // 待つ口を開ける。TCP は 50060 から順に試す（前に使えた番号を先に）。USB は抽象名の UNIX ソケット。
    // 使えなかった番号は理由（errno の名前）と一緒にログと画面に出す
    private Lst openListeners() {
        Lst l = new Lst();
        try {
            l.usb = new LocalServerSocket(USB_NAME);
        } catch (IOException e) {
            Logbook.i("GPU：USB のポート localabstract:" + USB_NAME + " を開けません（" + e.getMessage() + "）", "GPU: cannot open the USB socket localabstract:" + USB_NAME + " (" + e.getMessage() + ")");
        }
        List<Integer> order = new ArrayList<>();
        order.add(lastGoodPort);
        for (int q : PORTS) if (!order.contains(q)) order.add(q);
        StringBuilder bad = new StringBuilder();
        InetAddress lo;
        try {
            lo = InetAddress.getByName("127.0.0.1");
        } catch (IOException e) {
            return l;
        }
        for (int q : order) {
            FileDescriptor fd = null;
            try {
                fd = Os.socket(OsConstants.AF_INET, OsConstants.SOCK_STREAM, 0);
                Os.setsockoptInt(fd, OsConstants.SOL_SOCKET, OsConstants.SO_REUSEADDR, 1);
                Os.bind(fd, lo, q);
                Os.listen(fd, 4);
                l.tcp = fd;
                l.port = q;
                break;
            } catch (ErrnoException | java.net.SocketException e) {
                String why = e instanceof ErrnoException ? errnoName(((ErrnoException) e).errno) : e.getMessage();
                bad.append(bad.length() > 0 ? Logbook.t("・", ", ") : "").append(q).append(Logbook.t("（" + why + "）", " (" + why + ")"));
                if (fd != null) try { Os.close(fd); } catch (ErrnoException ignored) {}
            }
        }
        if (l.tcp != null) lastGoodPort = l.port;
        if (bad.length() > 0) {
            String sb = bad.toString();
            if (l.tcp != null) {
                portNote = Logbook.t("ポート " + sb + " は使えないので、" + l.port + " を使います", "Port " + sb + " could not be used, so " + l.port + " is used");
                Logbook.i("GPU：TCP のポート " + sb + " は使えません。" + l.port + " で待ちます", "GPU: TCP port " + sb + " could not be used. Using " + l.port);
            } else {
                portNote = Logbook.t("TCP のポートはどれも使えません（" + sb + "）。USB（localabstract）だけで待ちます", "No TCP port could be used (" + sb + "). Waiting on USB (localabstract) only");
                Logbook.i("GPU：TCP のポートはどれも使えません（" + sb + "）", "GPU: no TCP port could be used (" + sb + ")");
            }
            if (sb.contains("EPERM"))
                Logbook.i("GPU：EPERM は、端末がそのポートの bind を止めているときに出ます（Android 14 からの仕組み）", "GPU: EPERM means the phone blocks binding to that port (a mechanism since Android 14)");
        } else {
            portNote = "";
        }
        return l;
    }

    private static String errnoName(int e) {
        String n = OsConstants.errnoName(e);
        return n != null ? n : "errno " + e;
    }

    // 開けた口を受け手に渡す。受け手が PGPU_FD_SOCK の名前につないでくるので、uid を確かめて fd を送り、「OK」を待つ。
    // 10 秒で終わらなければ失敗にして、次からは受け手に自分で開けさせる
    private void handover(java.lang.Process p, LocalServerSocket hs, Lst l) {
        final boolean[] done = { false };
        Thread guard = new Thread(() -> {
            long until = SystemClock.elapsedRealtime() + 10_000;
            while (SystemClock.elapsedRealtime() < until && p.isAlive()) {
                synchronized (done) { if (done[0]) return; }
                SystemClock.sleep(100);
            }
            synchronized (done) { if (done[0]) return; }
            // accept を戻す（close だけでは戻らない版がある）
            try { Os.shutdown(hs.getFileDescriptor(), OsConstants.SHUT_RDWR); } catch (Exception ignored) {}
            try { hs.close(); } catch (IOException ignored) {}
        }, "gpu-handover-guard");
        guard.start();
        String fail = null;
        try {
            for (;;) {
                LocalSocket s = hs.accept();
                try {
                    int uid = s.getPeerCredentials().getUid();
                    if (uid != android.os.Process.myUid()) {
                        Logbook.i("GPU：受け渡しのソケットに、ほかの uid（" + uid + "）が接続してきたので断りました", "GPU: refused uid " + uid + " on the handover socket");
                        continue;
                    }
                    List<FileDescriptor> fds = new ArrayList<>();
                    String msg;
                    synchronized (l) {
                        if (l.tcp != null) fds.add(l.tcp);
                        if (l.usb != null) fds.add(l.usb.getFileDescriptor());
                        msg = "PGPU-FDS tcp=" + (l.tcp != null ? l.port : 0) + " unix=" + (l.usb != null ? USB_NAME : "-") + "\n";
                    }
                    s.setFileDescriptorsForSend(fds.toArray(new FileDescriptor[0]));
                    s.getOutputStream().write(msg.getBytes(StandardCharsets.US_ASCII));
                    s.setSoTimeout(5000);
                    InputStream in = s.getInputStream();
                    StringBuilder r = new StringBuilder();
                    int c;
                    while (r.length() < 16 && (c = in.read()) >= 0 && c != '\n') r.append((char) c);
                    if (!"OK".equals(r.toString())) fail = "reply '" + r + "'";
                    break;
                } finally {
                    try { s.close(); } catch (IOException ignored) {}
                }
            }
        } catch (IOException | RuntimeException e) {
            fail = p.isAlive() ? Logbook.t("10 秒待っても受信アプリが接続してきません", "the receiver did not connect within 10 seconds") : Logbook.t("受信アプリが先に終わりました", "the receiver ended first");
        } finally {
            synchronized (done) { done[0] = true; }
            try { hs.close(); } catch (IOException ignored) {}
        }
        if (fail == null) {
            handedOver = true;
            return;
        }
        synchronized (this) { selfBind = true; }
        Logbook.i("GPU：受信アプリにポートを渡せませんでした（" + fail + "）。次は受信アプリが自分でポートを開けます",
                "GPU: could not hand the sockets to the receiver (" + fail + "). Next time the receiver opens them itself");
        p.destroy();
    }

    // 終わるのを待つ。待っている間は記録を見て「待っている」が出たら実行中にする
    private void watch(java.lang.Process p, Lst l) {
        Thread poll = new Thread(() -> {
            boolean noted = false;
            while (p.isAlive()) {
                String t = tail(4096);
                if ("starting".equals(state) && isListening(t)) {
                    readListening(t, l);
                    state = "running";
                    if (!noted) {
                        String ja = (usb ? "USB localabstract:" + USB_NAME : "") + (usb && port > 0 ? "・" : "") + (port > 0 ? "TCP 127.0.0.1:" + port : "");
                        String en = (usb ? "USB localabstract:" + USB_NAME : "") + (usb && port > 0 ? ", " : "") + (port > 0 ? "TCP 127.0.0.1:" + port : "");
                        Logbook.i("GPU：受信アプリが接続を待っています（" + ja + "）。PC は adb forward tcp:50300 " + forwardTarget(),
                                "GPU: the receiver is waiting for the PC (" + en + "). On the PC: adb forward tcp:50300 " + forwardTarget());
                        noted = true;
                    }
                }
                SystemClock.sleep(500);
            }
        }, "gpu-poll");
        poll.start();
        int code;
        try {
            code = p.waitFor();
        } catch (InterruptedException e) {
            return;
        }
        l.close(); // 受け手が終わったので、アプリが持っていた口も閉じる
        synchronized (this) {
            if (proc != p) return; // 止めた（stop）
            proc = null;
            state = enabled ? "failed" : "off";
            port = 0;
            usb = false;
            pcConnected = false;
        }
        String raw = lastLine();
        String last = serverLine(raw);
        reason = Logbook.t("受信アプリがコード " + code + " で終わりました", "The receiver ended with code " + code) + (last.isEmpty() ? "" : Logbook.t("：", ": ") + last);
        Logbook.i("GPU：受信アプリが終わりました（" + code + "）。最後の記録：" + last, "GPU: the receiver ended (" + code + "). Last log line: " + last);
        if (code == EXIT_NO_FDS || (l.given && !handedOver)) {
            // 受け渡しの前に終わった（fd の受け渡しを知らない受け手など）。このとき記録の最後の行は、アプリが持っている口で待とうとした失敗なので、そのまま出さない
            synchronized (this) { selfBind = true; }
            reason = Logbook.t("受信アプリがアプリからポートを受け取れませんでした。次は受信アプリが自分でポートを開けます", "The receiver could not take the sockets from the app. Next time it opens them itself");
        } else if (raw.contains("EADDRINUSE") && staleStopped > 0) {
            // 前の受け手を止めた直後なので、口が空くのを待って起動し直せばよい
            reason = Logbook.t("前の受信アプリを止めました。ポートが空くのを待って、もう一度起動します", "Stopped an old receiver. Waiting for the port to be released and starting again");
        } else if (raw.contains("で待てない") || raw.contains("Cannot listen")) {
            reason = Logbook.t("ポートを開けませんでした（50060〜50069 と USB の localabstract:" + USB_NAME + " のどれも）：", "Could not open any port (50060 to 50069 and USB localabstract:" + USB_NAME + "): ") + last;
            final String why = reason;
            Logbook.i("GPU：" + why, "GPU: " + why);
        }
    }

    // 受け手の記録の「待っている」の行から、待っている口を読む（受け手が自分で開けたときの番号）
    private void readListening(String t, Lst l) {
        if (handedOver && l.any()) {
            synchronized (l) {
                port = l.tcp != null ? l.port : 0;
                usb = l.usb != null;
            }
            return;
        }
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("(?:受け手|listening on) ?：?127\\.0\\.0\\.1:(\\d+)").matcher(t);
        int found = 0;
        while (m.find()) {
            try { found = Integer.parseInt(m.group(1)); } catch (NumberFormatException ignored) {}
        }
        port = found;
        usb = t.contains("localabstract:" + USB_NAME + ")");
        if (found > 0) lastGoodPort = found;
        if (found > 0 && found != PORT) portNote = Logbook.t("ポート " + PORT + " が使えないので、" + found + " を使います", "Port " + PORT + " could not be used, so " + found + " is used");
        else if (found == 0 && usb) portNote = Logbook.t("TCP のポートはどれも使えません。USB（localabstract）だけで待ちます", "No TCP port could be used. Waiting on USB (localabstract) only");
        else portNote = "";
    }

    // 画面を点けたままにするか：つながっていて、つながってから（または最後の仕事から）KEEP_AWAKE_IDLE_MS がたっていない
    boolean keepScreen() {
        if (!pcConnected) return false;
        long since = Math.max(connectedAt, lastWorkAt);
        return SystemClock.elapsedRealtime() - since < KEEP_AWAKE_IDLE_MS;
    }

    // キットの受け手（別のプロセス・uid は shell）のとき：アプリから見えるのは、127.0.0.1:50300 で誰かが待っていること（kitRunning）だけ。
    // 受け手の CPU 時間（/proc は uid が違うと見えない）も接続（/proc/net は Android 10 から読めない）も見えない。
    // 台本が書く記録（/data/local/tmp/pgpu_server.log）が読める端末なら、そこの「つながった／切れた」の行を使う。
    // 読めないとき（shell が作ったファイルは ふつう自分以外に読めない）は、受け手が動いていて USB か充電器につながっている間、画面を点けたままにする
    // （受け手は stop_android.cmd で止めるまで動くため。ケーブルを抜けば電池を使い続けない）
    static final File KIT_LOG = new File("/data/local/tmp/pgpu_server.log");
    volatile boolean kitLogOk;      // キットの記録を読める
    volatile boolean kitConnected;  // キットの記録の最後が「つながった」（kitLogOk のときだけ意味がある）
    private Thread kitMon;

    boolean keepScreenKit(boolean plugged) {
        return "kit".equals(state) && plugged && (!kitLogOk || kitConnected);
    }

    private synchronized void startKitMon(int g) {
        if (kitMon != null && kitMon.isAlive()) return;
        kitMon = new Thread(() -> {
            long off = 0;
            final int[] ev = new int[1];
            int noted = 0; // 最後に記録に出した状態（0 まだ・1 読める・2 読めない）
            while (enabled && g == gen && "kit".equals(state)) {
                long o = scanLog(KIT_LOG, off, ev);
                if (o < 0) {
                    off = 0;
                    kitLogOk = false;
                    kitConnected = false;
                    if (noted != 2) {
                        noted = 2;
                        Logbook.i("GPU：キットの受け手の記録（" + KIT_LOG + "）は読めません。PC とつながっているかは分かりません", "GPU: cannot read the kit receiver log (" + KIT_LOG + "). Whether the PC is connected is unknown");
                    }
                } else {
                    off = o;
                    if (noted != 1) {
                        noted = 1;
                        Logbook.i("GPU：キットの受け手の記録（" + KIT_LOG + "）を読めます", "GPU: the kit receiver log (" + KIT_LOG + ") is readable");
                    }
                    kitLogOk = true;
                    if (ev[0] == 1) kitConnected = true;
                    else if (ev[0] == 2) kitConnected = false;
                }
                SystemClock.sleep(1000);
            }
            kitLogOk = false;
            kitConnected = false;
        }, "gpu-kitmon");
        kitMon.start();
    }

    // この数秒で GPU の仕事が来ているか
    boolean working() {
        return pcConnected && lastWorkAt != 0 && SystemClock.elapsedRealtime() - lastWorkAt < WORK_WINDOW_MS;
    }

    // 受け手が動いている間、1 秒ごとに「つながっているか」と「仕事が来ているか」を調べる。
    // つながっているか：受け手の記録に増えた行を読み、「つながった」「切れた」の行（日本語でも英語でも）で決める。
    // 仕事が来ているか：受け手の CPU 時間（/proc/<pid>/stat の utime と stime）が 1 秒で WORK_TICKS 以上増えたか。
    // 待っているだけの受け手は CPU を使わない。pid は /proc から実行ファイルの名前で探す（java.lang.Process から pid を取る公開の方法が API 29 に無い）
    private static final int WORK_TICKS = 1; // 待っているだけなら 10 秒たっても増えない・仕事があれば 1 秒に 30 前後増える（エミュレーターで測った）

    private void monitor(java.lang.Process p) {
        long off = 0;
        final int[] ev = new int[1];
        int pid = -1;
        long lastTicks = -1;
        long tickHz = 100;
        try { tickHz = Math.max(1, Os.sysconf(OsConstants._SC_CLK_TCK)); } catch (RuntimeException ignored) {}
        int need = (int) Math.max(1, Math.round(WORK_TICKS * tickHz / 100.0));
        while (p.isAlive()) {
            synchronized (this) { if (proc != p) return; }
            long o2 = scanLog(log, off, ev);
            if (o2 >= 0) off = o2;
            if (ev[0] == 2) pcConnected = false;
            else if (ev[0] == 1) {
                connectedAt = SystemClock.elapsedRealtime();
                lastWorkAt = 0;
                pcConnected = true;
            }
            if (pid < 0) pid = findPid(p);
            serverPid = pid;
            if (pid >= 0) {
                long t = cpuTicks(pid);
                if (t < 0) {
                    pid = -1;
                    serverPid = -1;
                    lastTicks = -1;
                } else {
                    if (lastTicks >= 0 && pcConnected && t - lastTicks >= need) lastWorkAt = SystemClock.elapsedRealtime();
                    lastTicks = t;
                }
            }
            SystemClock.sleep(1000);
        }
        serverPid = -1;
    }

    // 記録の off より後ろの、改行まで読めた行から「つながった」「切れた」を拾い、読み終えた位置を返す。
    // ev[0] に、増えた行のうち最後の出来事を入れる（0 なし・1 つながった・2 切れた）。読めなければ -1
    private static long scanLog(File file, long off, int[] ev) {
        ev[0] = 0;
        try (RandomAccessFile f = new RandomAccessFile(file, "r")) {
            long len = f.length();
            if (len < off) off = 0; // 記録が作り直された
            if (len == off) return off;
            long n = Math.min(len - off, 1 << 20);
            byte[] b = new byte[(int) n];
            f.seek(off);
            f.readFully(b);
            int end = (int) n;
            while (end > 0 && b[end - 1] != '\n') end--;
            if (end == 0) return n == 1 << 20 ? off + n : off; // 改行がまだ来ていない（巨大な 1 行なら飛ばす）
            String[] lines = new String(b, 0, end, StandardCharsets.UTF_8).split("\n");
            for (String ln : lines) {
                String x = ln.trim();
                if (x.equals("Disconnected") || x.equals("切れた")) ev[0] = 2;
                else if (x.equals("Connected") || x.startsWith("Connected (") || x.equals("つながった") || x.startsWith("つながった（")) ev[0] = 1;
            }
            return off + end;
        } catch (IOException | RuntimeException e) {
            return -1;
        }
    }

    private int findPid(java.lang.Process p) {
        int me = android.os.Process.myPid();
        File[] procs = new File("/proc").listFiles();
        if (procs == null) return -1;
        int found = -1;
        for (File f : procs) {
            String nm = f.getName();
            if (nm.isEmpty() || nm.charAt(0) < '0' || nm.charAt(0) > '9') continue;
            int pid;
            try { pid = Integer.parseInt(nm); } catch (NumberFormatException e) { continue; }
            if (pid == me) continue;
            String arg0 = cmdline0(pid);
            if (arg0.equals(exe.getAbsolutePath()) || arg0.endsWith("/" + EXE)) found = Math.max(found, pid); // 新しい方（pid が大きい方）
        }
        return found;
    }

    // /proc/<pid>/stat の utime + stime（クロック刻み）。読めなければ -1。コマンド名に空白や ) が入るので、最後の ) より後ろを読む
    static long cpuTicks(int pid) {
        try (RandomAccessFile f = new RandomAccessFile("/proc/" + pid + "/stat", "r")) {
            byte[] b = new byte[1024];
            int n = f.read(b);
            if (n <= 0) return -1;
            return parseStatTicks(new String(b, 0, n, StandardCharsets.UTF_8));
        } catch (IOException | RuntimeException e) {
            return -1;
        }
    }

    static long parseStatTicks(String s) {
        try {
            int i = s.lastIndexOf(')');
            if (i < 0) return -1;
            String[] a = s.substring(i + 1).trim().split(" ");
            // a[0] は state（全体の 3 番目）。utime は 14 番目 → a[11]、stime は 15 番目 → a[12]
            return Long.parseLong(a[11]) + Long.parseLong(a[12]);
        } catch (RuntimeException e) {
            return -1;
        }
    }

    // PC の adb forward の行き先
    String forwardTarget() {
        if (usb) return "localabstract:" + USB_NAME;
        if (port > 0) return "tcp:" + port;
        return "localabstract:" + USB_NAME; // まだ待っていないときは、ふだんの形を出す
    }

    private void stop() {
        java.lang.Process p;
        synchronized (this) {
            p = proc;
            proc = null;
            port = 0;
            usb = false;
            pcConnected = false;
        }
        if (p == null) return;
        p.destroy(); // SIGTERM。アプリが持っていた口は watch が閉じる
        new Thread(() -> {
            try {
                if (!p.waitFor(2, java.util.concurrent.TimeUnit.SECONDS)) p.destroyForcibly();
            } catch (InterruptedException ignored) {
            }
            Logbook.i("GPU：受信アプリを止めました", "GPU: stopped the receiver");
        }, "gpu-stop").start();
    }

    // 受け手の記録の行。0.1.3 の受け手は PGPU_LANG で英語を書くので、そのまま使う。
    // 古い受け手（キットの物など。日本語しか書けない）の行だけ、英語の画面でよく出る物を英語にする
    private static String serverLine(String s) {
        if (Logbook.ja() || s.isEmpty() || !hasJapanese(s)) return s;
        if (s.contains("Vulkan を起動できない")) return "Vulkan could not start: " + s;
        if (s.contains("で待てない")) return "Could not listen on the port. Another receiver may be using it: " + s;
        if (s.contains("で待っている")) return "Waiting for the PC: " + s;
        return s;
    }

    private static boolean hasJapanese(String s) {
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if ((c >= 0x3040 && c <= 0x30ff) || (c >= 0x4e00 && c <= 0x9fff)) return true;
        }
        return false;
    }

    // 受け手が「待っている」と記録に書いたか（日本語でも英語でも）
    private static boolean isListening(String t) {
        return t.contains("待っている") || t.contains("listening on");
    }

    // 前にこのアプリが起動した受け手が残っていれば止める。アプリの本体が落ちると、子の受け手が残って口をつかんだままになることがある。
    // 自分の uid の子なので止められる（shell の uid からは止められない）。/proc は同じ uid のプロセスだけ見える。
    // 実行ファイルの名前（libpgpu_server.so）で探す。止めた数を返す
    private int stopStale() {
        int me = android.os.Process.myPid();
        List<Integer> hit = new ArrayList<>();
        File[] procs = new File("/proc").listFiles();
        if (procs == null) return 0;
        for (File f : procs) {
            String nm = f.getName();
            if (nm.isEmpty() || nm.charAt(0) < '0' || nm.charAt(0) > '9') continue;
            int pid;
            try { pid = Integer.parseInt(nm); } catch (NumberFormatException e) { continue; }
            if (pid == me) continue;
            String arg0 = cmdline0(pid);
            if (arg0.equals(exe.getAbsolutePath()) || arg0.endsWith("/" + EXE)) hit.add(pid);
        }
        if (hit.isEmpty()) return 0;
        for (int pid : hit) android.os.Process.killProcess(pid);
        // 終わって口が空くのを待つ（最長 3 秒）
        long until = SystemClock.elapsedRealtime() + 3000;
        for (int pid : hit) {
            while (SystemClock.elapsedRealtime() < until && cmdline0(pid).endsWith(EXE)) SystemClock.sleep(50);
        }
        SystemClock.sleep(200);
        Logbook.i("GPU：前に起動した受信アプリが残っていたので止めました（" + hit.size() + " 個・pid " + hit + "）",
                "GPU: stopped " + hit.size() + " old receiver process(es) left from an earlier run (pid " + hit + ")");
        return hit.size();
    }

    // /proc/<pid>/cmdline の初めの 1 つ（実行ファイルの名前）。読めなければ空
    private static String cmdline0(int pid) {
        try (RandomAccessFile f = new RandomAccessFile("/proc/" + pid + "/cmdline", "r")) {
            byte[] b = new byte[512];
            int n = f.read(b);
            if (n <= 0) return "";
            int e = 0;
            while (e < n && b[e] != 0) e++;
            return new String(b, 0, e, StandardCharsets.UTF_8);
        } catch (IOException | RuntimeException e) {
            return "";
        }
    }

    // 記録の最後の n バイト
    String tail(int n) {
        try (RandomAccessFile f = new RandomAccessFile(log, "r")) {
            long len = f.length();
            long from = Math.max(0, len - n);
            byte[] b = new byte[(int) (len - from)];
            f.seek(from);
            f.readFully(b);
            return new String(b, StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "";
        }
    }

    private String lastLine() {
        String t = tail(2048).trim();
        int i = t.lastIndexOf('\n');
        return i >= 0 ? t.substring(i + 1) : t;
    }
}
