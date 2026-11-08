// 道具の置き場（ToolPaths）と、道具に渡す引数・環境変数の組み立て（ToolArgs）。
// camset の係の作業で webcam_rx・camera_setup の引数が変わったら、ここだけ直す。
//
// 道具を探す順（先に見つかった物）：
//   1. 環境変数 PGPU_TOOL_名前（例 PGPU_TOOL_WEBCAM_RX=D:\...\webcam_rx.exe）
//   2. PocketGPU.exe の横の tools.ini（1 行に 1 つ：webcam_rx=D:\...\webcam_rx.exe。# で始まる行は無視）
//   3. PocketGPU.exe の横・tools・extras のフォルダ
// 試す人は使わない、試験用の差し替え：
//   PGPU_TEST_CAM_ADDR=host:port    webcam_rx に --usb の代わりに --addr（偽の iPhone）
//   PGPU_TEST_AUDIO_ADDR=host:port  ipaudio に --usb の代わりに --connect
//   PGPU_TEST_CAP_ADDR=host:port    livecap に USB の代わりに --addr
//   PGPU_TEST_SWDEC=1               webcam_rx はソフトでほどく（GPU を使わない）
//   PGPU_TEST_MON_ASSUME=1          ipaudio が --mic-monitor に対応していると決めてかかる（前の ipaudio に断られたときの戻り方の試験）
//   PGPU_TEST_MUTE_ONLY=名前の一部   PC の音を消すとき、その名前を含む機器しかミュートしない（AudioDevices.cs）
//   IPHONE_VK_MUX=番号              usbmuxd の口（ドライバと同じ名前）。道具にも渡す
//   PGPU_TEST_CTL_PIPE=名前          カメラの操作のパイプを \\.\pipe\pocketgpu-camctl の代わりにこの名前で開く（CamCtl.cs。偽のパイプ test\fakectl）
//   PGPU_TEST_SHM=名前               プレビューが読む共有メモリを Local\pocketgpu-webcam-v1 の代わりにこの名前にする（CamPreview.cs）
//   PGPU_TEST_BATTERY=温度,残り,充電中  仮の電池の部品が返す値（IphoneBattery.cs。本物に差し替えたら効かない）
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace PocketGPU
{
    static class ToolPaths
    {
        public static readonly string[] Names = { "webcam_rx", "camera_setup", "ipaudio", "livecap", "vklist" };
        static Dictionary<string, string> ini;

        static Dictionary<string, string> Ini()
        {
            if (ini != null) return ini;
            ini = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            try
            {
                string p = Path.Combine(AppPaths.ExeDir, "tools.ini");
                if (File.Exists(p))
                    foreach (string raw in File.ReadAllLines(p, Encoding.UTF8))
                    {
                        string line = raw.Trim();
                        if (line.Length == 0 || line.StartsWith("#")) continue;
                        int eq = line.IndexOf('=');
                        if (eq > 0) ini[line.Substring(0, eq).Trim()] = Environment.ExpandEnvironmentVariables(line.Substring(eq + 1).Trim().Trim('"'));
                    }
            }
            catch { }
            return ini;
        }

        // 道具のフルパス。無ければ null
        public static string Find(string name)
        {
            string env = Environment.GetEnvironmentVariable("PGPU_TOOL_" + name.ToUpperInvariant());
            if (!string.IsNullOrEmpty(env) && File.Exists(env)) return env;
            string p;
            if (Ini().TryGetValue(name, out p) && File.Exists(p)) return p;
            if (name == "vklist")
            {
                string vk = Environment.GetEnvironmentVariable("PGPU_VKLIST"); // キットの台本と同じ名前
                if (!string.IsNullOrEmpty(vk) && File.Exists(vk)) return vk;
                string k = Path.Combine(KitDir, "vklist.exe");
                if (File.Exists(k)) return k;
            }
            foreach (string sub in new[] { "", "tools", "extras" })
            {
                string c = Path.Combine(Path.Combine(AppPaths.ExeDir, sub), name + ".exe");
                if (File.Exists(c)) return c;
            }
            return null;
        }

        // キットのフォルダ（iphone_icd.json・x86\・dxvk\・pocketgpu.cmd）。ふつうは PocketGPU.exe のある所
        public static string KitDir
        {
            get
            {
                string d = Environment.GetEnvironmentVariable("PGPU_KIT_DIR");
                if (!string.IsNullOrEmpty(d)) return d.TrimEnd('\\');
                string p;
                if (Ini().TryGetValue("kit", out p) && Directory.Exists(p)) return p.TrimEnd('\\');
                return AppPaths.ExeDir;
            }
        }
        public static bool DriverPresent { get { return File.Exists(Path.Combine(KitDir, "iphone_icd.json")); } }
    }

    // 1 回の起動：exe・引数の並び・足す環境変数
    class ToolCmd
    {
        public string Exe;
        public List<string> Args = new List<string>();
        public Dictionary<string, string> Env = new Dictionary<string, string>();
        public string WorkDir;
        public string Line { get { return ToolArgs.Join(Args); } }
    }

    static class ToolArgs
    {
        static string Test(string name) { string v = Environment.GetEnvironmentVariable(name); return string.IsNullOrEmpty(v) ? null : v; }
        static string MuxPort { get { return Test("IPHONE_VK_MUX"); } }

        // ---- カメラ：webcam_rx がほどいた NV12 を共有メモリ Local\pocketgpu-webcam-v1 に書く（仮想カメラとプレビューが読む）
        public static ToolCmd Webcam(Settings s)
        {
            var c = new ToolCmd { Exe = ToolPaths.Find("webcam_rx") };
            string test = Test("PGPU_TEST_CAM_ADDR");
            if (test != null) { c.Args.Add("--addr"); c.Args.Add(test); }
            else if (s.Bool("cam.wifi", false))
            {
                c.Args.Add("--addr"); c.Args.Add(WifiAddr(s.Str("cam.addr", "")));
                c.Args.Add("--code"); c.Args.Add(Digits(s.Str("cam.code", "")));
            }
            else
            {
                c.Args.Add("--usb");
                if (MuxPort != null) { c.Args.Add("--mux"); c.Args.Add(MuxPort); }
            }
            c.Args.Add("--shm");
            c.Args.Add("--nowin");
            c.Args.Add("--reconnect");
            string cam = s.Str("cam.camera", "auto");
            if (cam != "auto") { c.Args.Add("--camera"); c.Args.Add(cam); }
            // cam4k：解像度は 720p・1080p・4K。仮想カメラ（DirectShow）は ふだん 1080p までで、4K を選んだときだけ 3840x2160 も名乗る
            // （webcam_rx が環境変数 POCKETGPU_VCAM_MAX=2160 を見て 4K の口を作り、レジストリに書く。古い webcam_rx は環境変数を見ないので、引数ではなく環境変数で渡す）。
            // 4K 以外のときは何も渡さない（webcam_rx が 1080p までに戻す）
            int res = s.Int("cam.res", 1080);
            bool uhd = res >= 2160;
            int w = res == 720 ? 1280 : uhd ? 3840 : 1920, h = res == 720 ? 720 : uhd ? 2160 : 1080;
            if (uhd) c.Env["POCKETGPU_VCAM_MAX"] = "2160";
            c.Args.Add("--w"); c.Args.Add(w.ToString());
            c.Args.Add("--h"); c.Args.Add(h.ToString());
            c.Args.Add("--fps"); c.Args.Add(s.Int("cam.fps", 30) == 60 ? "60" : "30");
            string[] q = { "auto", "standard", "high", "max" };
            int qi = Math.Max(0, Math.Min(3, s.Int("cam.quality", 1)));
            c.Args.Add("--quality"); c.Args.Add(q[qi]);
            if (Test("PGPU_TEST_SWDEC") == "1") c.Args.Add("--swdec");
            // camgpu1015：デコードを CPU にすると、3060 の Video Decode を使わない（webcam_rx の --decode cpu。糸は 4 本）。古い webcam_rx は知らない引数で終わるので、設定が cpu のときだけ渡す
            else if (s.Str("cam.decode", "gpu") == "cpu") { c.Args.Add("--decode"); c.Args.Add("cpu"); c.Env["POCKETGPU_DEC_THREADS"] = "4"; }
            // カメラの操作のパイプ：ふだんは webcam_rx の既定の名前（pocketgpu-camctl）。試験のときだけ別の名前を渡す（--ctl-pipe は camctl2 の webcam_rx から）
            if (Test("PGPU_TEST_CTL_PIPE") != null) { c.Args.Add("--ctl-pipe"); c.Args.Add(Test("PGPU_TEST_CTL_PIPE")); }
            return c;
        }
        // アドレスは番号なしで入力してよい。iPhone のカメラの口は 50064
        public static string WifiAddr(string a)
        {
            a = (a ?? "").Trim();
            if (a.Length == 0) return a;
            return a.IndexOf(':') >= 0 ? a : a + ":50064";
        }
        public static string Digits(string a)
        {
            var sb = new StringBuilder();
            foreach (char ch in a ?? "") if (ch >= '0' && ch <= '9') sb.Append(ch);
            return sb.ToString();
        }

        // ---- 仮想カメラ：install・uninstall・status。データの置き場の camera に写した物から動かす（登録は DLL の場所を覚えるため）
        public static ToolCmd CameraSetup(string exe, string verb)
        {
            var c = new ToolCmd { Exe = exe };
            c.Args.Add(verb);
            return c;
        }

        // ---- マイクとスピーカー
        public static ToolCmd Audio(Settings s)
        {
            var c = new ToolCmd { Exe = ToolPaths.Find("ipaudio") };
            string test = Test("PGPU_TEST_AUDIO_ADDR");
            if (test != null) { c.Args.Add("--connect"); c.Args.Add(test); }
            else
            {
                c.Args.Add("--usb");
                if (MuxPort != null) { c.Args.Add("--mux"); c.Args.Add(MuxPort); }
            }
            if (s.Bool("audio.spk", true))
            {
                c.Args.Add("--speaker");
                string dev = s.Str("audio.spkDev", "");
                if (dev.Length > 0 && !dev.StartsWith("-")) c.Args.Add(dev);
            }
            if (s.Bool("audio.mic", false) && s.Str("audio.micDev", "").Length > 0)
            {
                c.Args.Add("--mic-render"); c.Args.Add(s.Str("audio.micDev", ""));
                if (s.Bool("audio.voice", false)) c.Args.Add("--voiceproc");
                // マイクの音を聞く：ipaudio が --mic-monitor に対応しているとき（MonitorSupported）だけ渡す
                if (s.Bool("audio.monitor", false) && MonitorSupported == true)
                {
                    string md = s.Str("audio.monitorDev", "");
                    c.Args.Add("--mic-monitor"); c.Args.Add(md.Length > 0 ? md : "default");
                    c.Args.Add("--mic-monitor-gain"); c.Args.Add(MonitorGainDb(s.Int("audio.monitorVol", 100)).ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
                }
            }
            c.Args.Add("--target-ms"); c.Args.Add(SpeakerDelayMs(s).ToString());
            return c;
        }
        // スピーカーの遅延：iPhone がためておく長さ。短い 20・標準 40・長い 80。前の版の 120 は長いにまとめる
        public static int SpeakerDelayMs(Settings s)
        {
            int buf = s.Int("audio.buf", 40);
            return buf <= 20 ? 20 : buf >= 80 ? 80 : 40;
        }
        // マイクの音を聞く音量：画面は 5〜100%、ipaudio には dB で渡す（100% が 0 dB）
        public static double MonitorGainDb(int percent)
        {
            int p = Math.Max(5, Math.Min(100, percent));
            return Math.Round(20.0 * Math.Log10(p / 100.0), 1);
        }

        // ipaudio が --mic-monitor と --mic-monitor-gain に対応しているか。null はまだ分からない
        // 見分け方：--list にこの 2 つを付けて起動する。対応していれば一覧を出して 0 で終わる。
        // 前の ipaudio は「知らない引数」と出して 2 で終わる（引数を読む所で止まり、音の機器には触らない）
        public static bool? MonitorSupported;
        public static ToolCmd AudioProbe()
        {
            var c = AudioList();
            c.Args.Add("--mic-monitor"); c.Args.Add("default");
            c.Args.Add("--mic-monitor-gain"); c.Args.Add("0");
            return c;
        }
        public static ToolCmd AudioList()
        {
            var c = new ToolCmd { Exe = ToolPaths.Find("ipaudio") };
            c.Args.Add("--list");
            return c;
        }

        // ---- 字幕
        public static ToolCmd Caption(Settings s)
        {
            var c = new ToolCmd { Exe = ToolPaths.Find("livecap") };
            c.Args.Add("--source"); c.Args.Add(s.Str("cap.source", "loopback") == "mic" ? "mic" : "loopback");
            c.Args.Add("--lang"); c.Args.Add(s.Str("cap.lang", L.Ja ? "ja-JP" : "en-US"));
            string tr = s.Str("cap.translate", "");
            if (tr.Length > 0) { c.Args.Add("--translate"); c.Args.Add(tr); }
            if (s.Bool("cap.accurate", false)) c.Args.Add("--accurate");
            if (!s.Bool("cap.window", true)) c.Args.Add("--no-window");
            if (s.Bool("cap.obs", false)) { c.Args.Add("--obs"); c.Args.Add(CaptionObsFile); }
            if (s.Bool("cap.minutes", false)) { c.Args.Add("--minutes"); c.Args.Add(AppPaths.Ensure(CaptionMinutesDir)); }
            c.Args.Add("--ui"); c.Args.Add(L.Ja ? "ja" : "en");
            c.Args.Add("--settings"); c.Args.Add(Path.Combine(AppPaths.Ensure(AppPaths.DataDir), "livecap_settings.json"));
            c.Args.Add("--end-wait"); c.Args.Add("3");
            string test = Test("PGPU_TEST_CAP_ADDR");
            if (test != null) { c.Args.Add("--addr"); c.Args.Add(test); }
            else if (MuxPort != null) { c.Args.Add("--mux"); c.Args.Add("127.0.0.1:" + MuxPort); }
            return c;
        }
        public static string CaptionObsFile { get { return Path.Combine(AppPaths.DataDir, "caption.txt"); } }
        public static string CaptionMinutesDir { get { return Path.Combine(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "PocketGPU"), "Captions"); } }
        public static string DefaultRecordDir { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), "PocketGPU"); } }

        // ---- ドライバに渡す環境変数（pocketgpu.cmd と pgpu_common.ps1 の Get-PgpuEnv と同じ）
        public static Dictionary<string, string> DriverEnv()
        {
            string kit = ToolPaths.KitDir;
            var e = new Dictionary<string, string>();
            e["VK_ADD_DRIVER_FILES"] = Path.Combine(kit, "iphone_icd.json") + ";" + Path.Combine(Path.Combine(kit, "x86"), "iphone_icd.json");
            e["IPHONE_VK"] = Test("IPHONE_VK") ?? "usb";
            if ((Test("IPHONE_VK_ONLY") ?? "1") == "1") e["VK_LOADER_DRIVERS_SELECT"] = "*iphone_icd*";
            e["DISABLE_TWITCH_VULKAN_OVERLAY"] = "1";
            e["DISABLE_VULKAN_OBS_CAPTURE"] = "1";
            e["DISABLE_VULKAN_OW_OBS_CAPTURE"] = "1";
            return e;
        }
        // チェックの vklist：ドライバの環境変数に加えて、ほかの層（Steam や OBS の重ね絵）を読ませない
        public static ToolCmd VkList()
        {
            var c = new ToolCmd { Exe = ToolPaths.Find("vklist") };
            foreach (var kv in DriverEnv()) c.Env[kv.Key] = kv.Value;
            c.Env["VK_LOADER_LAYERS_DISABLE"] = "~implicit~";
            return c;
        }
        // GPU の画面から起動するゲーム：ドライバ＋録画（obs/DESIGN.md）＋コマ数の記録（選んだときだけ）
        public static Dictionary<string, string> GameEnv(Settings s, string logPrefix)
        {
            var e = DriverEnv();
            e["IPHONE_VK_RECORD_FLAG"] = AppPaths.RecordFlag;
            if (s.Str("rec.codec", "hevc") == "h264") e["IPHONE_VK_RECORD_CODEC"] = "h264";
            if (!s.Bool("rec.audio", true)) e["IPHONE_VK_RECORD_AUDIO"] = "0";
            if (s.Bool("rec.fetch", true))
            {
                e["IPHONE_VK_RECORD_FETCH"] = s.Str("rec.folder", DefaultRecordDir); // フォルダはゲームを起動する時に作る（GameLaunch）
                if (!s.Bool("rec.keep", true)) e["IPHONE_VK_RECORD_KEEP"] = "0";
            }
            if (s.Bool("rec.photos", false)) e["IPHONE_VK_RECORD_PHOTOS"] = "1";
            if (logPrefix != null)
            {
                e["IPHONE_VK_LOG"] = "1";
                e["IPHONE_VK_FPS_LOG"] = "1";
                e["IPHONE_VK_ERR_FILE"] = logPrefix; // ドライバが .プロセス番号.txt を足す
            }
            return e;
        }
        public static string SteamLine { get { return "\"" + Path.Combine(ToolPaths.KitDir, "pocketgpu.cmd") + "\" %command%"; } }

        // Windows のコマンドラインの括り方（CommandLineToArgvW の決まり）
        public static string Join(List<string> args)
        {
            var sb = new StringBuilder();
            foreach (string a in args)
            {
                if (sb.Length > 0) sb.Append(' ');
                sb.Append(Quote(a));
            }
            return sb.ToString();
        }
        public static string Quote(string a)
        {
            if (a.Length > 0 && a.IndexOfAny(new[] { ' ', '\t', '"' }) < 0) return a;
            var sb = new StringBuilder("\"");
            int bs = 0;
            foreach (char ch in a)
            {
                if (ch == '\\') { bs++; continue; }
                if (ch == '"') { sb.Append('\\', bs * 2 + 1); sb.Append('"'); bs = 0; continue; }
                sb.Append('\\', bs); bs = 0;
                sb.Append(ch);
            }
            sb.Append('\\', bs * 2);
            sb.Append('"');
            return sb.ToString();
        }
    }
}
