// チェック：Start-PocketGPU（start.ps1）の 7 つの項目を C# に移した物。裏の糸で順に見て、NG の所で止まる。
// 試す人は使わない、試験用の差し替えは pgpu_common.ps1 と同じ名前：IPHONE_VK_MUX・PGPU_VKLIST・PGPU_TEST_ELEVATED・PGPU_TEST_APPLE_SW
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Management;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.ServiceProcess;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Threading;

namespace PocketGPU
{
    enum CheckState { Wait, Running, Ok, Warn, Ng, Skip }

    class CheckItem
    {
        public int N;
        public CheckState State;
        public string Detail = "";                       // 1 行目の下の文
        public List<string> More = new List<string>();   // NG・注意のときの案内
        public string ActionText;                        // 案内のボタン（Microsoft Store で開くなど）
        public Action Action;
        public bool CanRetry;
    }

    class AppleSoftware
    {
        public bool Service, ServiceRunning, Devices, StoreITunes;
        public string DevicesAumid, ITunesAumid;
    }

    class CheckRunner
    {
        public readonly CheckItem[] Items = new CheckItem[7];
        readonly Dispatcher ui;
        public event Action Updated;
        public bool Busy { get; private set; }
        public bool MediaOnly;
        public DateTime? LastRun;
        public MuxDevice Dev;
        public HelloResult LastHello;
        public int LastThermal = -1;
        AppleSoftware sw;
        readonly List<string> log = new List<string>();

        public CheckRunner(Dispatcher ui)
        {
            this.ui = ui;
            for (int i = 0; i < 7; i++) Items[i] = new CheckItem { N = i + 1, State = CheckState.Wait };
        }

        void Post() { ui.BeginInvoke(new Action(() => { if (Updated != null) Updated(); })); }

        public void Run(int from)
        {
            if (Busy) return;
            Busy = true;
            for (int i = from - 1; i < 7; i++)
            {
                Items[i].State = CheckState.Wait;
                Items[i].Detail = "";
                Items[i].More.Clear();
                Items[i].ActionText = null;
                Items[i].Action = null;
                Items[i].CanRetry = false;
            }
            if (from == 1) lock (log) log.Clear();
            Post();
            var t = new Thread(() =>
            {
                try { RunSteps(from); }
                catch (Exception ex) { Log("  error " + ex.Message); }
                Busy = false;
                LastRun = DateTime.Now;
                SaveLog();
                Post();
            });
            t.IsBackground = true;
            t.Start();
        }

        void RunSteps(int from)
        {
            Log("PocketGPU check " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));
            Func<CheckItem, bool>[] steps = { Step1, Step2, Step3, Step4, Step5, Step6, Step7 };
            for (int i = from - 1; i < 7; i++)
            {
                var it = Items[i];
                if (MediaOnly && (i == 4 || i == 6)) { it.State = CheckState.Skip; Post(); continue; }
                it.State = CheckState.Running;
                if (i == 5) it.Detail = L.T("c6_wait");
                Post();
                Log("[" + (i + 1) + "/7] " + L.T("c" + (i + 1)));
                bool go = steps[i](it);
                Log("  " + it.State + " " + it.Detail);
                foreach (string m in it.More) Log("      " + m);
                Post();
                if (!go) return;
            }
        }

        bool Ng(CheckItem it, string detail, params string[] more)
        {
            it.State = CheckState.Ng;
            it.Detail = detail;
            it.More.AddRange(more);
            it.CanRetry = true;
            return false;
        }

        // ==== 1. この PC
        [StructLayout(LayoutKind.Sequential)]
        struct OSV { public int Size, Major, Minor, Build, Platform; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Csd; }
        [DllImport("ntdll.dll", CharSet = CharSet.Unicode)] static extern int RtlGetVersion(ref OSV v);

        public static bool Elevated()
        {
            if (Environment.GetEnvironmentVariable("PGPU_TEST_ELEVATED") == "1") return true;
            try { return new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator); } catch { return false; }
        }

        bool Step1(CheckItem it)
        {
            var v = new OSV { Size = Marshal.SizeOf(typeof(OSV)) };
            RtlGetVersion(ref v);
            if (!Environment.Is64BitOperatingSystem || v.Major < 10) return Ng(it, L.T("c1_ng"));
            it.State = CheckState.Ok;
            it.Detail = L.F("c1_ok", v.Build >= 22000 ? "Windows 11" : "Windows 10", v.Build);
            if (Elevated()) { it.State = CheckState.Warn; it.More.Add(L.T("c1_admin")); }
            try
            {
                object lua = Microsoft.Win32.Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System", "EnableLUA", 1);
                if (lua is int && (int) lua == 0) { it.State = CheckState.Warn; it.More.Add(L.T("c1_uac")); }
            }
            catch { }
            string kit = ToolPaths.KitDir;
            var enc = Encoding.Default;
            if (enc.GetString(enc.GetBytes(kit)) != kit) { it.State = CheckState.Warn; it.More.Add(L.T("c1_path")); }
            return true;
        }

        // ==== 2. Apple のソフト
        public static AppleSoftware FindAppleSoftware()
        {
            var r = new AppleSoftware();
            string fake = Environment.GetEnvironmentVariable("PGPU_TEST_APPLE_SW");
            if (!string.IsNullOrEmpty(fake))
            {
                if (fake == "devices") { r.Devices = true; r.DevicesAumid = "AppleInc.AppleDevices_nzyj5cx40ttqa!App"; }
                if (fake == "itunes") { r.StoreITunes = true; r.ITunesAumid = "AppleInc.iTunes_nzyj5cx40ttqa!App"; }
                if (fake == "service") r.Service = true;
                return r;
            }
            try
            {
                using (var sc = new ServiceController("Apple Mobile Device Service"))
                {
                    r.ServiceRunning = sc.Status == ServiceControllerStatus.Running;
                    r.Service = true;
                }
            }
            catch { }
            // ストアのアプリ。表示名は言語で変わるので、manifest の Application の Id から開く名前（AUMID）を作る
            var c = new ToolCmd { Exe = Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe") };
            c.Args.AddRange(new[] { "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command",
                "Get-AppxPackage -Name 'AppleInc.*' | ForEach-Object { $id = ''; try { $id = @((Get-AppxPackageManifest $_).Package.Applications.Application)[0].Id } catch {}; $_.Name + '|' + $_.PackageFamilyName + '|' + $id }" });
            c.WorkDir = Environment.SystemDirectory;
            int code;
            string outp = ChildTool.RunOnce(c, 20000, out code) ?? "";
            foreach (string line in outp.Split('\n'))
            {
                string[] p = line.Trim().Split('|');
                if (p.Length < 3) continue;
                string aumid = p[2].Length > 0 ? p[1] + "!" + p[2] : null;
                if (p[0] == "AppleInc.AppleDevices") { r.Devices = true; r.DevicesAumid = aumid; }
                if (p[0] == "AppleInc.iTunes") { r.StoreITunes = true; r.ITunesAumid = aumid; }
            }
            return r;
        }

        bool Step2(CheckItem it)
        {
            sw = FindAppleSoftware();
            if (sw.Devices) { it.State = CheckState.Ok; it.Detail = L.T("c2_devices"); return true; }
            if (sw.Service) { it.State = CheckState.Ok; it.Detail = L.T("c2_service"); return true; }
            if (sw.StoreITunes) { it.State = CheckState.Ok; it.Detail = L.T("c2_store"); return true; }
            Ng(it, L.T("c2_ng"), L.T("c2_ng2"));
            it.ActionText = L.T("act_store");
            it.Action = () => Open("ms-windows-store://pdp/?ProductId=9NP83LWLPZ9K");
            return false;
        }

        static void Open(string what)
        {
            try { Process.Start(new ProcessStartInfo(what) { UseShellExecute = true }); } catch { }
        }

        // ==== 3. Apple のサービス
        bool Step3(CheckItem it)
        {
            if (Mux.Listening()) { it.State = CheckState.Ok; it.Detail = L.T("c3_ok"); return true; }
            if (sw == null) sw = FindAppleSoftware();
            string aumid = sw.Devices ? sw.DevicesAumid : sw.StoreITunes ? sw.ITunesAumid : null;
            if (sw.Devices || sw.StoreITunes)
            {
                string app = sw.Devices ? "Apple Devices" : "iTunes";
                Ng(it, L.T("c3_ng"), L.F("c3_app", app));
                if (aumid != null)
                {
                    it.ActionText = L.F("act_open_app", app);
                    it.Action = () =>
                    {
                        Open("shell:AppsFolder\\" + aumid);
                        // サービスが始まるまで最大 30 秒待って、この項目からやり直す
                        var t = new Thread(() =>
                        {
                            for (int i = 0; i < 30 && !Mux.Listening(); i++) Thread.Sleep(1000);
                            ui.BeginInvoke(new Action(() => Run(3)));
                        });
                        t.IsBackground = true;
                        t.Start();
                    };
                }
                return false;
            }
            if (sw.Service && !sw.ServiceRunning) return Ng(it, L.T("c3_ng"), L.T("c3_stopped"));
            return Ng(it, L.T("c3_ng"), L.T("c3_restart"));
        }

        // ==== 4. iPhone が USB で見えるか
        public static List<string[]> UsbApplePhones()
        {
            var r = new List<string[]>();
            try
            {
                using (var s = new ManagementObjectSearcher("SELECT Name, ConfigManagerErrorCode, PNPDeviceID FROM Win32_PnPEntity WHERE PNPDeviceID LIKE 'USB\\\\VID_05AC&PID_12%'"))
                    foreach (ManagementObject o in s.Get())
                        r.Add(new[] { Convert.ToString(o["Name"]), Convert.ToString(o["ConfigManagerErrorCode"]) });
            }
            catch { }
            return r;
        }

        bool Step4(CheckItem it)
        {
            var devs = Mux.ListDevices() ?? new List<MuxDevice>();
            var usb = devs.FindAll(d => d.Type == "USB");
            if (usb.Count > 0)
            {
                Dev = usb[0];
                it.State = CheckState.Ok;
                it.Detail = L.T("c4_ok");
                Log("      usbmuxd ConnectionSpeed=" + Dev.Speed + " ProductID=" + Dev.ProductId);
                if (usb.Count > 1) { it.State = CheckState.Warn; it.More.Add(L.T("c4_many")); }
                return true;
            }
            Dev = null;
            var more = new List<string>();
            if (devs.Exists(d => d.Type == "Network")) more.Add(L.T("c4_wifi"));
            var pnp = UsbApplePhones();
            if (pnp.Count > 0)
            {
                if (pnp.Exists(p => p[1] != "0")) more.Add(L.T("c4_driver"));
                else more.Add(L.T("c4_trust"));
            }
            else more.Add(L.T("c4_plug"));
            return Ng(it, L.T("c4_ng"), more.ToArray());
        }

        // ==== 5. iPhone のアプリが答えるか
        bool Step5(CheckItem it)
        {
            if (Dev == null) return Ng(it, L.T("c4_ng"));
            int thermal;
            var h = Mux.TestReceiver(Dev.Id, 3000, true, out thermal);
            if (h.Ok)
            {
                LastHello = h;
                LastThermal = thermal;
                it.State = CheckState.Ok;
                it.Detail = L.F("c5_ok", h.MemMB > 0 ? L.F("c5_mem", h.Name, h.MemMB / 1024.0) : h.Name);
                return true;
            }
            if (h.Why == "timeout") return Ng(it, L.T("c5_busy"), L.T("c5_busy2"));
            if (h.Why == "result" && h.Result == -9)
            {
                string ver = Mux.AppVersion;
                return Ng(it, L.T("c5_ver"), L.F("c5_ver2", ver.Length > 0 ? "RpcServer-" + ver + ".ipa" : "RpcServer-*.ipa"));
            }
            if (h.Why == "result") return Ng(it, L.F("c5_refused", h.Result));
            return Ng(it, L.T("c5_none"), L.T("c5_none2"), L.T("c5_none3"));
        }

        // ==== 6. USB の速さ。遅くても止めずに次へ（「もう一度」で測り直せる）
        bool Step6(CheckItem it)
        {
            if (Dev == null) return Ng(it, L.T("c4_ng"));
            string err;
            var bw = Mux.MeasureUsb(Dev.Id, 32, out err);
            if (bw == null) { it.State = CheckState.Warn; it.Detail = L.T("c6_none"); it.CanRetry = true; return true; }
            if (err != null) { it.State = CheckState.Warn; it.Detail = L.F("c6_err", err); it.CanRetry = true; return true; }
            string line = L.F("c6_line", bw[0], bw[1], bw[2]);
            if (bw[0] < 45 || bw[1] < 35)
            {
                it.State = CheckState.Warn;
                it.Detail = line;
                it.More.Add(L.T("c6_slow"));
                it.More.Add(L.T("c6_slow2"));
                it.CanRetry = true;
                return true;
            }
            it.State = CheckState.Ok;
            it.Detail = line;
            if (bw[0] < 100) it.More.Add(L.T("c6_mid"));
            return true;
        }

        // ==== 7. ドライバで iPhone の GPU が見えるか
        bool Step7(CheckItem it)
        {
            string dll = Path.Combine(Environment.SystemDirectory, "vulkan-1.dll");
            if (!File.Exists(dll)) return Ng(it, L.T("c7_novk"));
            var fv = FileVersionInfo.GetVersionInfo(dll);
            var lv = new Version(fv.FileMajorPart, fv.FileMinorPart, fv.FileBuildPart, fv.FilePrivatePart);
            bool oldVk = lv < new Version(1, 3, 234);
            if (Elevated()) return Ng(it, L.T("c7_admin"));
            var c = ToolArgs.VkList();
            if (c.Exe == null) return Ng(it, L.T("c7_ng"), L.T("c7_missing"));
            if (c.Exe.EndsWith(".cmd", StringComparison.OrdinalIgnoreCase))
            {
                c.Args.Insert(0, c.Exe);
                c.Args.Insert(0, "/c");
                c.Exe = Environment.GetEnvironmentVariable("ComSpec") ?? "cmd.exe";
            }
            int code;
            string outp = ChildTool.RunOnce(c, 30000, out code);
            if (outp == null) return Ng(it, L.T("c7_ng"), L.T("c7_timeout"));
            var gpus = new List<string>();
            foreach (string l in outp.Split('\n'))
            {
                var m = Regex.Match(l.Trim(), @"^GPU\d+: (.*?)(\s+\(vendor .*)?$");
                if (m.Success) gpus.Add(m.Groups[1].Value);
            }
            if (gpus.Count == 0)
            {
                var more = new List<string>();
                var lines = new List<string>();
                foreach (string l in outp.Split('\n')) if (l.Trim().Length > 0) lines.Add(l.Trim());
                for (int i = Math.Max(0, lines.Count - 5); i < lines.Count; i++) more.Add(lines[i]);
                more.Add(L.T("c7_av"));
                return Ng(it, L.T("c7_ng"), more.ToArray());
            }
            it.State = oldVk ? CheckState.Warn : CheckState.Ok;
            it.Detail = L.F("c7_ok", string.Join("・", gpus.ToArray()));
            if (oldVk) it.More.Add(L.F("c7_oldvk", lv));
            return true;
        }

        // ---- 記録。ユーザー名と PC 名は伏せる
        void Log(string s) { lock (log) log.Add(s); }
        public string Text()
        {
            string body;
            lock (log) body = string.Join("\r\n", log.ToArray());
            string u = Environment.UserName, pc = Environment.MachineName;
            if (!string.IsNullOrEmpty(u)) body = Regex.Replace(body, Regex.Escape(u), "(user)", RegexOptions.IgnoreCase);
            if (!string.IsNullOrEmpty(pc)) body = Regex.Replace(body, Regex.Escape(pc), "(pc)", RegexOptions.IgnoreCase);
            return body;
        }
        void SaveLog()
        {
            try { File.WriteAllText(Path.Combine(AppPaths.Ensure(AppPaths.DataDir), "check-log.txt"), Text() + "\r\n", new UTF8Encoding(true)); } catch { }
        }
    }
}
