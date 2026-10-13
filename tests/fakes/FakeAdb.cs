// 試験用の偽の adb.exe。Android のスマートフォンと adb forward のふりをする（本物の端末も adb も使わない）。
// run-tests.ps1 が Add-Type で adb.exe に組む。状態は環境変数 FAKE_ADB_STATE のファイル（key=value の行）に持つ。
//   serial=…      端末の番号（空なら端末なし）
//   dstate=device|unauthorized
//   pids=1111 2222     スマートフォンで動いている pgpu_server
//   hash=…          スマートフォンに置いた pgpu_server の SHA-256
//   file=1
//   forward=50300:50300
//   fwdpid=…        forward の口を開けている偽の adb の PID
//   next=7001       次に使う pid
// adb forward のふり：口を開けて待つ「adb.exe __serve 口 状態ファイル」を別に起こす。受け手が動いていれば、あいさつに答える
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

    public static int Main(string[] args)
    {
        if (args.Length >= 3 && args[0] == "__serve") return Serve(int.Parse(args[1]), args[2]);
        var st = StatePath;
        var s = Load(st);
        var a = new List<string>(args);
        if (a.Count >= 2 && a[0] == "-s") a.RemoveRange(0, 2);
        if (a.Count == 0) return 1;
        string serial = Get(s, "serial");
        switch (a[0])
        {
            case "start-server": return 0;
            case "get-state": Console.WriteLine("device"); return 0;
            case "devices":
                Console.WriteLine("List of devices attached");
                if (serial != "") Console.WriteLine(serial + "\t" + (Get(s, "dstate") == "" ? "device" : Get(s, "dstate")));
                Console.WriteLine();
                return 0;
            case "push":
                if (a.Count >= 3)
                {
                    using (var sha = SHA256.Create())
                        s["hash"] = BitConverter.ToString(sha.ComputeHash(File.ReadAllBytes(a[1]))).Replace("-", "").ToLowerInvariant();
                    s["file"] = "1";
                    Save(st, s);
                    Console.WriteLine("1 file pushed.");
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
        if (cmd.StartsWith("getprop ro.product.cpu.abi")) { Console.WriteLine("arm64-v8a"); return 0; }
        if (cmd.StartsWith("getprop ro.product.model")) { Console.WriteLine("FakePhone"); return 0; }
        if (cmd.StartsWith("pidof pgpu_server"))
        {
            var p = Get(s, "pids");
            if (p == "") return 1;
            Console.WriteLine(p);
            return 0;
        }
        if (cmd.StartsWith("sha256sum"))
        {
            if (Get(s, "file") != "1") { Console.Error.WriteLine("sha256sum: /data/local/tmp/pgpu_server: No such file or directory"); return 1; }
            Console.WriteLine(Get(s, "hash") + "  /data/local/tmp/pgpu_server");
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
        if (cmd.StartsWith("cat /data/local/tmp/pgpu_server.log")) { Console.WriteLine("fake receiver log"); return 0; }
        return 0;
    }

    static int Forward(List<string> a, Dictionary<string, string> s, string st, string serial)
    {
        if (a.Count >= 2 && a[1] == "--list")
        {
            var f = Get(s, "forward");
            if (f != "") { var p = f.Split(':'); Console.WriteLine((serial == "" ? "FAKE" : serial) + " tcp:" + p[0] + " tcp:" + p[1]); }
            return 0;
        }
        if (a.Count >= 3 && a[1] == "--remove")
        {
            KillServe(s);
            s["forward"] = ""; s["fwdpid"] = "";
            Save(st, s);
            return 0;
        }
        if (a.Count >= 3 && a[1].StartsWith("tcp:") && a[2].StartsWith("tcp:"))
        {
            int port = int.Parse(a[1].Substring(4));
            // 本物の adb は、同じ口がもう forward されていれば、置き換える（--no-rebind を付けない限り）
            if (Get(s, "forward").StartsWith(port + ":")) { KillServe(s); Thread.Sleep(300); }
            try { var l = new TcpListener(IPAddress.Loopback, port); l.Start(); l.Stop(); }
            catch (SocketException) { Console.Error.WriteLine("error: cannot bind listener: Address already in use"); return 1; }
            KillServe(s);
            var psi = new ProcessStartInfo(System.Reflection.Assembly.GetExecutingAssembly().Location, "__serve " + port + " \"" + st + "\"");
            psi.UseShellExecute = true;   // 親の出力の管を引き継がないため
            psi.WindowStyle = ProcessWindowStyle.Hidden;
            var pr = Process.Start(psi);
            s["forward"] = a[1].Substring(4) + ":" + a[2].Substring(4);
            s["fwdpid"] = pr.Id.ToString();
            Save(st, s);
            Thread.Sleep(400);
            Console.WriteLine(port);
            return 0;
        }
        return 0;
    }

    static void KillServe(Dictionary<string, string> s)
    {
        int pid;
        if (int.TryParse(Get(s, "fwdpid"), out pid)) { try { Process.GetProcessById(pid).Kill(); } catch { } }
    }

    static int Serve(int port, string st)
    {
        var l = new TcpListener(IPAddress.Loopback, port);
        l.Start();
        while (true)
        {
            var c = l.AcceptTcpClient();
            var t = new Thread(() => Handle(c, st));
            t.IsBackground = true;
            t.Start();
        }
    }

    static void Handle(TcpClient c, string st)
    {
        try
        {
            var s = Load(st);
            // スマートフォンで受け手が動いていないなら、本物の adb と同じように、つないですぐ切る
            if (Get(s, "pids") == "") { c.Close(); return; }
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
