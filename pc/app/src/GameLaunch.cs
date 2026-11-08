// ゲームやアプリを iPhone の GPU で起動する。Run-on-iPhone-GPU（run.ps1）と同じ順に見る：
// 管理者で動く設定か → iPhone のアプリが答えるか → DirectX 12・9 か、11 なら DXVK が横にあるか → Steam か。
// 環境変数はこの起動にだけ付ける。レジストリには書かない。DXVK を置くのは、聞いて「はい」のときだけ。
// 起動したゲームはジョブに入れない（このアプリを閉じてもゲームは止めない）。
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Windows;

namespace PocketGPU
{
    class PeInfo
    {
        public ushort Machine;
        public List<string> Imports = new List<string>();
        public string Level = "";

        // PE の見出しから、32/64 ビット・取り込む DLL・manifest の管理者の印を読む（pgpu_common.ps1 の Get-PeInfo）
        public static PeInfo Read(string path)
        {
            try
            {
                using (var fs = File.OpenRead(path))
                using (var br = new BinaryReader(fs))
                {
                    if (br.ReadUInt16() != 0x5A4D) return null;
                    fs.Position = 0x3C;
                    int pe = br.ReadInt32();
                    fs.Position = pe;
                    if (br.ReadUInt32() != 0x4550) return null;
                    var r = new PeInfo();
                    r.Machine = br.ReadUInt16();
                    int nsec = br.ReadUInt16();
                    fs.Position = pe + 20;
                    int optSize = br.ReadUInt16();
                    long opt = pe + 24;
                    fs.Position = opt;
                    ushort magic = br.ReadUInt16();
                    long dd = opt + (magic == 0x20b ? 112 : 96);
                    fs.Position = dd + 8; uint impRva = br.ReadUInt32();
                    fs.Position = dd + 16; uint rsrcRva = br.ReadUInt32(); uint rsrcSize = br.ReadUInt32();
                    var secs = new List<long[]>();
                    fs.Position = opt + optSize;
                    for (int i = 0; i < nsec; i++)
                    {
                        br.ReadBytes(8);
                        uint vs = br.ReadUInt32(), va = br.ReadUInt32(), rs = br.ReadUInt32(), rp = br.ReadUInt32();
                        br.ReadBytes(16);
                        secs.Add(new long[] { va, Math.Max(vs, rs), rp });
                    }
                    Func<long, long> toOff = rva =>
                    {
                        foreach (var s in secs) if (rva >= s[0] && rva < s[0] + s[1]) return rva - s[0] + s[2];
                        return -1;
                    };
                    long o = toOff(impRva);
                    if (impRva != 0 && o >= 0)
                        for (int i = 0; i < 512; i++)
                        {
                            fs.Position = o + 20 * i;
                            byte[] d = br.ReadBytes(20);
                            if (d.Length < 20 || BitConverter.ToUInt32(d, 12) == 0) break;
                            long no = toOff(BitConverter.ToUInt32(d, 12));
                            if (no < 0) continue;
                            fs.Position = no;
                            byte[] nb = br.ReadBytes(64);
                            int end = Array.IndexOf(nb, (byte) 0);
                            if (end < 0) end = nb.Length;
                            r.Imports.Add(Encoding.ASCII.GetString(nb, 0, end).ToLowerInvariant());
                        }
                    o = toOff(rsrcRva);
                    if (rsrcRva != 0 && o >= 0)
                    {
                        fs.Position = o;
                        byte[] raw = br.ReadBytes((int) Math.Min(rsrcSize, 32u << 20));
                        string txt = Encoding.GetEncoding(28591).GetString(raw) + Encoding.Unicode.GetString(raw);
                        if (txt.Contains("requireAdministrator")) r.Level = "requireAdministrator";
                        else if (txt.Contains("highestAvailable")) r.Level = "highestAvailable";
                    }
                    return r;
                }
            }
            catch { return null; }
        }
    }

    class RunningGame
    {
        public Process Proc;
        public string Path, Name, LogPrefix;
        public DateTime Started;
        public double Fps = -1;
        public bool Alive { get { try { return !Proc.HasExited; } catch { return false; } } }
    }

    static class GameLaunch
    {
        static Window owner;
        public static void SetOwner(Window w) { owner = w; }

        static bool Ask(string text)
        {
            var r = owner != null ? MessageBox.Show(owner, text, "PocketGPU", MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No)
                                  : MessageBox.Show(text, "PocketGPU", MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No);
            return r == MessageBoxResult.Yes;
        }
        static void Tell(string text, MessageBoxImage icon)
        {
            if (owner != null) MessageBox.Show(owner, text, "PocketGPU", MessageBoxButton.OK, icon);
            else MessageBox.Show(text, "PocketGPU", MessageBoxButton.OK, icon);
        }

        public static string Pick()
        {
            var dlg = new Microsoft.Win32.OpenFileDialog
            {
                Title = L.T("launch_pick_title"),
                Filter = L.T("launch_filter") + "|*.exe;*.lnk;*.bat;*.cmd",
                DereferenceLinks = false
            };
            bool? ok = owner != null ? dlg.ShowDialog(owner) : dlg.ShowDialog();
            return ok == true ? dlg.FileName : null;
        }

        static string Sha(string p)
        {
            using (var s = File.OpenRead(p))
            using (var h = SHA256.Create()) return BitConverter.ToString(h.ComputeHash(s));
        }

        // .lnk をたどる（WScript.Shell）。戻り値は [先, 引数, 作業フォルダ]
        static string[] ResolveLnk(string lnk)
        {
            try
            {
                Type t = Type.GetTypeFromProgID("WScript.Shell");
                object sh = Activator.CreateInstance(t);
                object sc = t.InvokeMember("CreateShortcut", System.Reflection.BindingFlags.InvokeMethod, null, sh, new object[] { lnk });
                Type st = sc.GetType();
                Func<string, string> get = n => Convert.ToString(st.InvokeMember(n, System.Reflection.BindingFlags.GetProperty, null, sc, null));
                return new[] { get("TargetPath"), get("Arguments"), get("WorkingDirectory") };
            }
            catch { return null; }
        }

        // 起動できたら RunningGame、やめたら null。Busy は「このアプリから起動したゲームが動いている」（受け手にあいさつしない）
        public static RunningGame Launch(string target, Settings s, bool busy)
        {
            if (!ToolPaths.DriverPresent) { Tell(L.F("launch_nodriver", ToolPaths.KitDir), MessageBoxImage.Error); return null; }
            if (CheckRunner.Elevated()) { Tell(L.T("launch_self_admin"), MessageBoxImage.Warning); return null; }
            string lnkArgs = "", workDir = null;
            if (target.EndsWith(".url", StringComparison.OrdinalIgnoreCase)) { Tell(L.T("launch_url"), MessageBoxImage.Information); return null; }
            if (target.EndsWith(".lnk", StringComparison.OrdinalIgnoreCase))
            {
                var l = ResolveLnk(target);
                if (l == null || string.IsNullOrEmpty(l[0]) || !File.Exists(l[0])) { Tell(L.T("launch_lnk_bad"), MessageBoxImage.Warning); return null; }
                lnkArgs = l[1] ?? "";
                if (!string.IsNullOrEmpty(l[2])) workDir = l[2];
                target = l[0];
            }
            if (!File.Exists(target)) { Tell(L.F("launch_notfound", target), MessageBoxImage.Warning); return null; }
            target = Path.GetFullPath(target);
            string exeDir = Path.GetDirectoryName(target);
            if (workDir == null) workDir = exeDir;

            // 1. 管理者
            PeInfo pe = target.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? PeInfo.Read(target) : null;
            string adminWhy = "";
            if (pe != null && pe.Level == "requireAdministrator") adminWhy = L.T("launch_admin_always");
            else if (CompatRunAsAdmin(target)) adminWhy = L.T("launch_admin_compat");
            if (adminWhy.Length > 0) { if (!Ask(adminWhy + "\n\n" + L.T("launch_admin_tail"))) return null; }
            else if (pe != null && pe.Level == "highestAvailable") Tell(L.T("launch_admin_maybe"), MessageBoxImage.Information);

            // 2. iPhone の様子（数秒）。このアプリから起動したゲームが動いている間は、受け手にあいさつしない
            if (!busy)
            {
                string problem = "";
                if (!Mux.Listening()) problem = L.T("launch_mux_down");
                else
                {
                    var devs = Mux.ListDevices() ?? new List<MuxDevice>();
                    var usb = devs.FindAll(d => d.Type == "USB");
                    if (usb.Count == 0) problem = L.T("launch_no_usb");
                    else
                    {
                        int th;
                        var h = Mux.TestReceiver(usb[0].Id, 2500, false, out th);
                        if (!h.Ok)
                            problem = h.Why == "timeout" ? L.T("launch_busy") : (h.Why == "result" && h.Result == -9) ? L.T("launch_mismatch") : L.T("launch_noanswer");
                    }
                }
                if (problem.Length > 0 && !Ask(L.F("launch_anyway", problem))) return null;
            }

            // 3. DirectX と Steam
            if (pe != null)
            {
                string dxTarget = target;
                PeInfo dxPe = pe;
                // Unreal のゲームは、外の .exe が中の *-Shipping.exe を起動する。DXVK は中の方の横に置く
                try
                {
                    var ship = new List<string>();
                    foreach (string d in Directory.GetDirectories(exeDir))
                    {
                        string bin = Path.Combine(Path.Combine(d, "Binaries"), "Win64");
                        if (Directory.Exists(bin)) ship.AddRange(Directory.GetFiles(bin, "*-Shipping.exe"));
                    }
                    if (ship.Count == 1 && !pe.Imports.Contains("d3d11.dll"))
                    {
                        dxTarget = ship[0];
                        dxPe = PeInfo.Read(dxTarget) ?? pe;
                    }
                }
                catch { }
                var imp = dxPe.Imports;
                if (imp.Contains("d3d12.dll")) Tell(L.T("launch_dx12"), MessageBoxImage.Information);
                else if (imp.Contains("d3d9.dll")) Tell(L.T("launch_dx9"), MessageBoxImage.Information);
                else if ((imp.Contains("d3d11.dll") || imp.Contains("dxgi.dll")) && !imp.Contains("vulkan-1.dll"))
                {
                    string arch = dxPe.Machine == 0x8664 ? "x64" : dxPe.Machine == 0x14c ? "x32" : "";
                    string dir = Path.GetDirectoryName(dxTarget);
                    string here = Path.Combine(dir, "d3d11.dll");
                    string kitDll = arch.Length > 0 ? Path.Combine(Path.Combine(Path.Combine(ToolPaths.KitDir, "dxvk"), arch), "d3d11.dll") : "";
                    if (arch.Length == 0) Tell(L.T("launch_dxvk_arch"), MessageBoxImage.Information);
                    else if (File.Exists(here))
                    {
                        bool same = false;
                        try { same = File.Exists(kitDll) && Sha(here) == Sha(kitDll); } catch { }
                        if (!same) Tell(L.T("launch_dxvk_other"), MessageBoxImage.Information);
                    }
                    else if (!File.Exists(kitDll)) Tell(L.F("launch_dxvk_missing", kitDll), MessageBoxImage.Warning);
                    else
                    {
                        string ask = L.F("launch_dxvk_ask", dir);
                        if (dxTarget != target) ask = L.F("launch_unreal", dxTarget) + "\n\n" + ask;
                        if (Ask(ask))
                        {
                            if (File.Exists(Path.Combine(dir, "dxgi.dll"))) Tell(L.T("launch_dxvk_dxgi"), MessageBoxImage.Information);
                            else
                            {
                                try
                                {
                                    File.Copy(kitDll, here);
                                    File.Copy(Path.Combine(Path.GetDirectoryName(kitDll), "dxgi.dll"), Path.Combine(dir, "dxgi.dll"));
                                }
                                catch (Exception ex) { Tell(L.F("launch_dxvk_fail", ex.Message), MessageBoxImage.Warning); }
                            }
                        }
                    }
                }
                if (File.Exists(Path.Combine(exeDir, "steam_api64.dll")) || File.Exists(Path.Combine(exeDir, "steam_api.dll")))
                    Tell(L.F("launch_steam", ToolArgs.SteamLine), MessageBoxImage.Information);
            }

            // 4. 起動。.bat と .cmd は cmd.exe で
            string logPrefix = null;
            if (s.Bool("gpu.diag", false))
                logPrefix = Path.Combine(AppPaths.Ensure(AppPaths.LogDir), "game-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
            var env = ToolArgs.GameEnv(s, logPrefix);
            string fetch;
            if (env.TryGetValue("IPHONE_VK_RECORD_FETCH", out fetch)) AppPaths.Ensure(fetch);
            ProcessStartInfo psi;
            bool script = target.EndsWith(".bat", StringComparison.OrdinalIgnoreCase) || target.EndsWith(".cmd", StringComparison.OrdinalIgnoreCase);
            if (script) psi = new ProcessStartInfo(Environment.GetEnvironmentVariable("ComSpec") ?? "cmd.exe", "/c \"\"" + target + "\" " + lnkArgs + "\"");
            else psi = new ProcessStartInfo(target, lnkArgs);
            psi.UseShellExecute = false;
            psi.WorkingDirectory = workDir;
            foreach (var kv in env) psi.EnvironmentVariables[kv.Key] = kv.Value;
            try
            {
                var p = Process.Start(psi);
                return new RunningGame { Proc = p, Path = target, Name = Path.GetFileNameWithoutExtension(target), Started = DateTime.Now, LogPrefix = logPrefix };
            }
            catch (Exception ex)
            {
                Tell(L.F("launch_fail", ex.Message), MessageBoxImage.Error);
                return null;
            }
        }

        // 互換性の設定の「管理者としてこのプログラムを実行する」（読むだけ）
        static bool CompatRunAsAdmin(string exe)
        {
            foreach (string root in new[] { "HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE" })
            {
                try
                {
                    object v = Microsoft.Win32.Registry.GetValue(root + @"\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers", exe, null);
                    if (v != null && v.ToString().Contains("RUNASADMIN")) return true;
                }
                catch { }
            }
            return false;
        }

        // 「コマ数を記録」のとき：ドライバのログ（IPHONE_VK_ERR_FILE.プロセス番号.txt）の最後の「直近 X 秒：Y コマ/秒」を読む
        public static double ReadFps(RunningGame g)
        {
            if (g.LogPrefix == null) return -1;
            try
            {
                string dir = Path.GetDirectoryName(g.LogPrefix), name = Path.GetFileName(g.LogPrefix);
                double best = -1;
                DateTime newest = DateTime.MinValue;
                foreach (string f in Directory.GetFiles(dir, name + ".*.txt"))
                {
                    var fi = new FileInfo(f);
                    if (fi.LastWriteTime < newest) continue;
                    string tail = Tail(f, 8192);
                    int k = tail.LastIndexOf("コマ/秒", StringComparison.Ordinal);
                    if (k < 0) continue;
                    int a = tail.LastIndexOf('：', k);
                    double v;
                    if (a >= 0 && double.TryParse(tail.Substring(a + 1, k - a - 1).Trim(), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out v))
                    {
                        best = v;
                        newest = fi.LastWriteTime;
                    }
                }
                return best;
            }
            catch { return -1; }
        }
        static string Tail(string path, int bytes)
        {
            using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            {
                long start = Math.Max(0, fs.Length - bytes);
                fs.Position = start;
                var b = new byte[fs.Length - start];
                int n = fs.Read(b, 0, b.Length);
                return Encoding.UTF8.GetString(b, 0, n);
            }
        }
    }
}
