// 試験用の偽の adb.exe。Android のスマートフォンと adb forward のふりをする（本物の端末も adb も使わない）。
// run-tests.ps1 が Add-Type で adb.exe に組む。状態は環境変数 FAKE_ADB_STATE のファイル（key=value の行）に持つ。
//   serial=…      端末の番号（空なら端末なし）
//   dstate=device|unauthorized
//   pids=1111 2222     スマートフォンで動いている pgpu_server（キットの受け手）
//   hash=…          スマートフォンに置いた pgpu_server の SHA-256
//   file=1
//   app=0.1.2       スマートフォンに入っている PocketGPU アプリの版（空なら入っていない）
//   apprecv=1       アプリの中の GPU の受け手が動いている（TCP は端末の 50060。0.1.4 以上は localabstract:pocketgpu-gpu でも待つ）
//   appport=50061   アプリの受け手の TCP の番号（既定 50060）。50060 が使えず次の番号で待った形（0.1.4。tcp:50060 は答えない）
//   legacy=1        アプリの設定「古い PC のツールも受け付ける」がオン
//   refuse_abstract=1   localabstract: への adb forward を断る（古い adb のふり）
//   crlf=1          shell の出力の行末を CR CR LF にする（古い adb のふり）
//   fwds=口>先>pid;…   今の adb forward の一覧（先は tcp:番号 か localabstract:名前。pid は口を開けている偽の adb の PID）
//   next=7001       次に使う pid
// 環境変数 FAKE_ADB_LOG に書くと、呼ばれた adb の引数を 1 行ずつそのファイルに足す。
// adb forward のふり：口を開けて待つ「adb.exe __serve 口 状態ファイル」を別に起こす。つながると、先と状態に応じて答える：
//   tcp:番号（GPU）             キットの受け手が動いていれば GPU のあいさつに答える。先が 50060 なら、アプリの受け手が 50060 で動いていれば答える
//   localabstract:pocketgpu-gpu  アプリの版が 0.1.4 以上で、アプリの受け手が動いていれば GPU のあいさつに答える
//   tcp:50064・tcp:50063        アプリの版が 0.1.1 以前か、legacy=1 のときだけ、アプリの口として答える（0.1.2 は既定で断る）
//   localabstract:pocketgpu-…   アプリの版が 0.1.2 以上のときだけ、アプリの口として答える
// アプリの口の答えは「PGFAKE-APP webcam」「PGFAKE-APP audio」の 1 行。断るときは、つないですぐ切る
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

public static class FakeAdb
{
    static string StatePath { get { return Environment.GetEnvironmentVariable("FAKE_ADB_STATE") ?? Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "state.txt"); } }
    static string Eol = "\n";

    static Dictionary<string, string> Load(string path)
    {
        var d = new Dictionary<string, string>();
        if (File.Exists(path))
            foreach (var l in File.ReadAllLines(path)) { int i = l.IndexOf('='); if (i > 0) d[l.Substring(0, i)] = l.Substring(i + 1); }
        return d;
    }
    static void Save(string path, Dictionary<string, string> d)
    {
        var lines = new List<string>();
        foreach (var kv in d) lines.Add(kv.Key + "=" + kv.Value);
        File.WriteAllLines(path, lines.ToArray());
    }
    static string Get(Dictionary<string, string> d, string k) { string v; return d.TryGetValue(k, out v) ? v : ""; }

    // fwds の読み書き。1 つ = 口>先>pid
    class Fwd { public int Port; public string Target; public string Pid; }
    static List<Fwd> LoadFwds(Dictionary<string, string> s)
    {
        var l = new List<Fwd>();
        foreach (var e in Get(s, "fwds").Split(new[] { ';' }, StringSplitOptions.RemoveEmptyEntries))
        {
            var p = e.Split('>');
            if (p.Length >= 2) l.Add(new Fwd { Port = int.Parse(p[0]), Target = p[1], Pid = p.Length > 2 ? p[2] : "" });
        }
        return l;
    }
    static void SaveFwds(Dictionary<string, string> s, List<Fwd> l)
    {
        var parts = new List<string>();
        foreach (var f in l) parts.Add(f.Port + ">" + f.Target + ">" + f.Pid);
        s["fwds"] = string.Join(";", parts.ToArray());
    }
    static void KillPid(string pid)
    {
        int n;
        if (int.TryParse(pid, out n)) { try { Process.GetProcessById(n).Kill(); } catch { } }
    }
    static void Line(string text) { Console.Write(text + Eol); }

    public static int Main(string[] args)
    {
        if (args.Length >= 3 && args[0] == "__serve") return Serve(int.Parse(args[1]), args[2]);
        var st = StatePath;
        var s = Load(st);
        Eol = Get(s, "crlf") == "1" ? "\r\r\n" : "\n";
        var log = Environment.GetEnvironmentVariable("FAKE_ADB_LOG");
        if (!string.IsNullOrEmpty(log)) { try { File.AppendAllText(log, string.Join(" ", args) + "\n"); } catch { } }
        var a = new List<string>(args);
        if (a.Count >= 2 && a[0] == "-s") a.RemoveRange(0, 2);
        if (a.Count == 0) return 1;
        string serial = Get(s, "serial");
        switch (a[0])
        {
            case "start-server": return 0;
            case "get-state": Line("device"); return 0;
            case "devices":
                Line("List of devices attached");
                if (serial != "") Line(serial + "\t" + (Get(s, "dstate") == "" ? "device" : Get(s, "dstate")));
                Line("");
                return 0;
            case "push":
                if (a.Count >= 3)
                {
                    using (var sha = SHA256.Create())
                        s["hash"] = BitConverter.ToString(sha.ComputeHash(File.ReadAllBytes(a[1]))).Replace("-", "").ToLowerInvariant();
                    s["file"] = "1";
                    Save(st, s);
                    Line("1 file pushed.");
                }
                return 0;
            case "forward":
                return Forward(a, s, st, serial);
            case "shell":
                return Shell(string.Join(" ", a.GetRange(1, a.Count - 1).ToArray()), s, st);
        }
        return 0;
    }

    static int Shell(string cmd, Dictionary<string, string> s, string st)
    {
        if (cmd.StartsWith("getprop ro.product.cpu.abi")) { Line("arm64-v8a"); return 0; }
        if (cmd.StartsWith("getprop ro.product.model")) { Line("FakePhone"); return 0; }
        if (cmd.StartsWith("pidof pgpu_server"))
        {
            var p = Get(s, "pids");
            if (p == "") return 1;
            Line(p);
            return 0;
        }
        if (cmd.StartsWith("pm path "))
        {
            if (Get(s, "app") == "") return 1;
            Line("package:/data/app/~~fake/com.tsurutanmen.pocketgpu-1/base.apk");
            return 0;
        }
        if (cmd.StartsWith("dumpsys package "))
        {
            if (Get(s, "app") == "") { Line("Unable to find package: " + cmd.Substring(16)); return 0; }
            Line("Packages:");
            Line("  Package [com.tsurutanmen.pocketgpu] (abc123):");
            Line("    userId=10200");
            Line("    versionCode=3 minSdk=29 targetSdk=34");
            Line("    versionName=" + Get(s, "app"));
            Line("    splits=[base]");
            return 0;
        }
        if (cmd.StartsWith("am start "))
        {
            // --ez gpu true|false だけ見る（アプリの受け手が動くかどうか）。キットの受け手が動いていれば、アプリは自分の受け手を起こさない
            if (cmd.Contains("--ez gpu true")) s["apprecv"] = Get(s, "pids") == "" ? "1" : "0";
            if (cmd.Contains("--ez gpu false")) s["apprecv"] = "0";
            Save(st, s);
            Line("Starting: Intent { cmp=com.tsurutanmen.pocketgpu/.MainActivity }");
            return 0;
        }
        if (cmd.StartsWith("sha256sum"))
        {
            if (Get(s, "file") != "1") { Console.Error.WriteLine("sha256sum: /data/local/tmp/pgpu_server: No such file or directory"); return 1; }
            Line(Get(s, "hash") + "  /data/local/tmp/pgpu_server");
            return 0;
        }
        if (cmd.Contains("kill $(pidof pgpu_server)"))
        {
            s["pids"] = ""; if (cmd.Contains("rm -f")) s["file"] = "0";
            Save(st, s);
            return 0;
        }
        if (cmd.Contains("nohup ./pgpu_server"))
        {
            int next = int.Parse(Get(s, "next") == "" ? "7001" : Get(s, "next"));
            s["pids"] = next.ToString(); s["next"] = (next + 1).ToString();
            Save(st, s);
            return 0;
        }
        if (cmd.StartsWith("kill "))
        {
            var left = new List<string>(Get(s, "pids").Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries));
            foreach (var t in cmd.Substring(5).Split(' ')) left.Remove(t);
            s["pids"] = string.Join(" ", left.ToArray());
            Save(st, s);
            return 0;
        }
        if (cmd.StartsWith("cat /data/local/tmp/pgpu_server.log")) { Line("fake receiver log"); return 0; }
        return 0;
    }

    static int Forward(List<string> a, Dictionary<string, string> s, string st, string serial)
    {
        var fw = LoadFwds(s);
        if (a.Count >= 2 && a[1] == "--list")
        {
            foreach (var f in fw) Line((serial == "" ? "FAKE" : serial) + " tcp:" + f.Port + " " + f.Target);
            return 0;
        }
        if (a.Count >= 3 && a[1] == "--remove")
        {
            int rp;
            if (!a[2].StartsWith("tcp:") || !int.TryParse(a[2].Substring(4), out rp)) return 0;
            var hit = fw.Find(f => f.Port == rp);
            if (hit == null) { Console.Error.WriteLine("adb.exe: error: listener '" + a[2] + "' not found"); return 1; }
            KillPid(hit.Pid);
            fw.Remove(hit);
            SaveFwds(s, fw);
            Save(st, s);
            return 0;
        }
        if (a.Count >= 3 && a[1].StartsWith("tcp:") && (a[2].StartsWith("tcp:") || a[2].StartsWith("localabstract:")))
        {
            int port = int.Parse(a[1].Substring(4));
            if (a[2].StartsWith("localabstract:") && Get(s, "refuse_abstract") == "1") { Console.Error.WriteLine("adb.exe: error: unknown host service"); return 1; }
            // 本物の adb は、同じ口がもう forward されていれば、置き換える（--no-rebind を付けない限り）
            var old = fw.Find(f => f.Port == port);
            if (old != null) { KillPid(old.Pid); fw.Remove(old); SaveFwds(s, fw); Save(st, s); Thread.Sleep(300); }
            try { var l = new TcpListener(IPAddress.Loopback, port); l.Start(); l.Stop(); }
            catch (SocketException) { Console.Error.WriteLine("error: cannot bind listener: Address already in use"); return 1; }
            var psi = new ProcessStartInfo(System.Reflection.Assembly.GetExecutingAssembly().Location, "__serve " + port + " \"" + st + "\"");
            psi.UseShellExecute = true;   // 親の出力の管を引き継がないため
            psi.WindowStyle = ProcessWindowStyle.Hidden;
            var pr = Process.Start(psi);
            fw.Add(new Fwd { Port = port, Target = a[2], Pid = pr.Id.ToString() });
            SaveFwds(s, fw);
            Save(st, s);
            Thread.Sleep(400);
            Line(port.ToString());
            return 0;
        }
        return 0;
    }

    static int Serve(int port, string st)
    {
        var l = new TcpListener(IPAddress.Loopback, port);
        l.Start();
        while (true)
        {
            var c = l.AcceptTcpClient();
            var t = new Thread(() => Handle(c, st, port));
            t.IsBackground = true;
            t.Start();
        }
    }

    static bool NewApp(string v) { return AppAtLeast(v, new Version(0, 1, 2)); }
    // GPU の受け手が USB 用の口（localabstract:pocketgpu-gpu）でも待つ版
    static bool GpuUsbApp(string v) { return AppAtLeast(v, new Version(0, 1, 4)); }
    static bool AppAtLeast(string v, Version min)
    {
        var m = System.Text.RegularExpressions.Regex.Match(v ?? "", @"^(\d+)\.(\d+)(?:\.(\d+))?");
        if (!m.Success) return false;
        var ver = new Version(int.Parse(m.Groups[1].Value), int.Parse(m.Groups[2].Value), m.Groups[3].Success ? int.Parse(m.Groups[3].Value) : 0);
        return ver >= min;
    }

    static void Handle(TcpClient c, string st, int port)
    {
        try
        {
            var s = Load(st);
            var fw = LoadFwds(s);
            var me = fw.Find(f => f.Port == port);
            string target = me == null ? "" : me.Target;
            string app = Get(s, "app");
            // アプリの口：向け方が、入っている版に合っているときだけ答える。合っていなければ、つないですぐ切る
            string kind = target == "localabstract:pocketgpu-webcam" || target == "tcp:50064" ? "webcam"
                        : target == "localabstract:pocketgpu-audio" || target == "tcp:50063" ? "audio" : "";
            if (kind != "")
            {
                bool abstractForm = target.StartsWith("localabstract:");
                bool ok = app != "" && (abstractForm ? NewApp(app) : (!NewApp(app) || Get(s, "legacy") == "1"));
                if (!ok) { c.Close(); return; }
                var msg = Encoding.ASCII.GetBytes("PGFAKE-APP " + kind + "\n");
                c.GetStream().Write(msg, 0, msg.Length);
                var hold = new byte[1];
                while (c.GetStream().Read(hold, 0, 1) > 0) { }
                return;
            }
            // GPU：先が 50060 か localabstract:pocketgpu-gpu ならアプリの受け手、それ以外はキットの受け手。動いていなければ、本物の adb と同じように、つないですぐ切る
            string appPort = Get(s, "appport") == "" ? "50060" : Get(s, "appport");
            if (target == "tcp:50060") { if (Get(s, "apprecv") != "1" || appPort != "50060") { c.Close(); return; } }
            else if (target == "localabstract:pocketgpu-gpu") { if (Get(s, "apprecv") != "1" || !GpuUsbApp(app)) { c.Close(); return; } }
            else if (target.StartsWith("localabstract:")) { c.Close(); return; }
            else if (Get(s, "pids") == "") { c.Close(); return; }
            var ns = c.GetStream();
            var req = new byte[12];
            int got = 0;
            while (got < 12) { int k = ns.Read(req, got, 12 - got); if (k <= 0) { c.Close(); return; } got += k; }
            var payload = new byte[1400];
            var nm = Encoding.UTF8.GetBytes("PocketGPU phone (fake)");
            Array.Copy(nm, 0, payload, 20, nm.Length);
            BitConverter.GetBytes((uint)1).CopyTo(payload, 1304);
            BitConverter.GetBytes((ulong)8L * 1024 * 1024 * 1024).CopyTo(payload, 1308);
            BitConverter.GetBytes((uint)1).CopyTo(payload, 1316);
            var head = new byte[8];
            BitConverter.GetBytes(0).CopyTo(head, 0);
            BitConverter.GetBytes((uint)payload.Length).CopyTo(head, 4);
            ns.Write(head, 0, 8);
            ns.Write(payload, 0, payload.Length);
            var one = new byte[1];
            while (ns.Read(one, 0, 1) > 0) { }
        }
        catch { }
        finally { try { c.Close(); } catch { } }
    }
}
