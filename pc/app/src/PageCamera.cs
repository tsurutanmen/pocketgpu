// カメラ：webcam_rx を起動・停止し、共有メモリからプレビューを描く。仮想カメラの登録は camera_setup に任せる。
using System;
using System.IO;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Threading;

namespace PocketGPU
{
    partial class Shell
    {
        public CamPreview preview = new CamPreview();
        DispatcherTimer previewTimer;
        int[] camFormat;                 // webcam_rx が書いた「決まった形」：幅・高さ・コマ数・カメラ
        long camSeq;
        DateTime camSeqAt = DateTime.MinValue;
        string vcamState = "", vcamMsg = "", camMsg = "";
        bool camLoading;

        static string CamDir { get { return Path.Combine(AppPaths.DataDir, "camera"); } }

        void InitCamera()
        {
            camLoading = true;
            FillCameraCombos();
            // cam4k：解像度は 720p・1080p・4K（OBS 向け）。4K を選んだときだけ、仮想カメラが 3840x2160 も名乗る（Tools.cs）
            var res = S.Int("cam.res", 1080);
            Get<RadioButton>(res == 720 ? "CamRes720" : res >= 2160 ? "CamRes2160" : "CamRes1080").IsChecked = true;
            Get<RadioButton>(S.Int("cam.fps", 30) == 60 ? "CamFps60" : "CamFps30").IsChecked = true;
            Get<RadioButton>(S.Str("cam.decode", "gpu") == "cpu" ? "CamDecCpu" : "CamDecGpu").IsChecked = true; // camgpu1015：デコードの場所
            Get<Slider>("CamQuality").Value = Math.Max(0, Math.Min(3, S.Int("cam.quality", 1)));
            Get<CheckBox>("CamWifi").IsChecked = S.Bool("cam.wifi", false);
            Get<TextBox>("CamWifiAddr").Text = S.Str("cam.addr", "");
            Get<TextBox>("CamWifiCode").Text = S.Str("cam.code", "");
            camLoading = false;

            Get<ComboBox>("CamWhich").SelectionChanged += (o, e) => { if (!camLoading) CamSet("cam.camera", SelectedTag((ComboBox) o)); };
            foreach (var kv in new[] { new[] { "CamRes720", "720" }, new[] { "CamRes1080", "1080" }, new[] { "CamRes2160", "2160" } })
            {
                string v = kv[1];
                Get<RadioButton>(kv[0]).Checked += (o, e) => { if (!camLoading) CamSet("cam.res", int.Parse(v)); };
            }
            Get<RadioButton>("CamFps30").Checked += (o, e) => { if (!camLoading) CamSet("cam.fps", 30); };
            Get<RadioButton>("CamFps60").Checked += (o, e) => { if (!camLoading) CamSet("cam.fps", 60); };
            Get<RadioButton>("CamDecGpu").Checked += (o, e) => { if (!camLoading) CamSet("cam.decode", "gpu"); };
            Get<RadioButton>("CamDecCpu").Checked += (o, e) => { if (!camLoading) CamSet("cam.decode", "cpu"); };
            var q = Get<Slider>("CamQuality");
            q.ValueChanged += (o, e) => { Get<TextBlock>("CamQualityText").Text = L.T("q_" + (int) q.Value); };
            q.AddHandler(Thumb.DragCompletedEvent, new DragCompletedEventHandler((o, e) => CamSet("cam.quality", (int) q.Value)));
            q.PreviewMouseUp += (o, e) => { if (!camLoading) CamSet("cam.quality", (int) q.Value); };
            OnSwitch(Get<CheckBox>("CamWifi"), on => CamSet("cam.wifi", on));
            Get<TextBox>("CamWifiAddr").LostFocus += (o, e) => CamSet("cam.addr", ((TextBox) o).Text.Trim());
            Get<TextBox>("CamWifiCode").LostFocus += (o, e) => CamSet("cam.code", ((TextBox) o).Text.Trim());

            Get<Button>("CamStartBtn").Click += (o, e) => { if (Cam.Want) StopCamera(); else StartCamera(); Refresh(); };
            Get<Button>("CamPreviewBtn").Click += (o, e) => { S.Set("cam.preview", !S.Bool("cam.preview", true)); UpdatePreviewTimer(); Refresh(); };
            Get<Button>("CamVcamBtn").Click += (o, e) => VcamToggle();

            Cam.Line += CamLine;
            Cam.StateChanged += () => { if (!Cam.Want) { camFormat = null; if (Ctl != null && Ctl.Wanted) CtlStop(); } UpdatePreviewTimer(); };
            InitCamQuick();
            InitCamCtl();
        }

        // ---- 音と字幕（配信しながら、カメラの画面だけで操作する用）
        // スイッチは「いま実際に動いているか」を見せる。値はほかの画面と同じ所から読む：
        //   スピーカー：ipaudio が動いていて、スピーカーの設定 audio.spk がオン
        //               （ホームの「マイクとスピーカー」・マイクとスピーカーの画面の「PC の音を iPhone から出す」と同じ値）
        //   マイク    ：ipaudio が動いていて、マイクの設定 audio.mic がオンで、流す先が選ばれている
        //   字幕      ：livecap が動いている（ホームの「字幕」・字幕の画面の開始と停止と同じ）
        // 止まっているところからスピーカーかマイクを入れたときは、押した方だけを使う（もう片方の設定はオフにする）。
        // こうすると、動いている間はマイクとスピーカーの画面のスイッチとも同じ値になる。
        bool camMicNeedDev;

        void InitCamQuick()
        {
            OnSwitch(Get<CheckBox>("CamSpkSw"), on => QuickAudio(true, on));
            OnSwitch(Get<CheckBox>("CamMicSw"), on =>
            {
                if (on && S.Str("audio.micDev", "").Length == 0) { camMicNeedDev = true; Refresh(); return; }
                QuickAudio(false, on);
            });
            OnSwitch(Get<CheckBox>("CamCapSw"), on =>
            {
                camMicNeedDev = false;
                if (on) { capLines.Clear(); Cap.Start(); } else Cap.Stop();
                Refresh();
            });
        }

        void QuickAudio(bool speaker, bool on)
        {
            camMicNeedDev = false;
            if (on && !Audio.Want) S.Set(speaker ? "audio.mic" : "audio.spk", false);
            S.Set(speaker ? "audio.spk" : "audio.mic", on);
            audioMsg = "";
            bool spk = S.Bool("audio.spk", true), mic = S.Bool("audio.mic", false) && S.Str("audio.micDev", "").Length > 0;
            if (!spk && !mic) { if (Audio.Want) Audio.Stop(); }
            else if (Audio.Want) Audio.Restart();
            else StartAudio();
            Refresh();
        }

        void RefreshCamQuick()
        {
            bool aw = Audio.Want;
            string micDev = S.Str("audio.micDev", "");
            bool spkOn = aw && S.Bool("audio.spk", true);
            bool micOn = aw && S.Bool("audio.mic", false) && micDev.Length > 0;
            bool live = aw && audioConnected && Audio.Running;
            bool audioBad = !aw && (Audio.State == ToolState.Missing || Audio.State == ToolState.Failed);
            Get<CheckBox>("CamSpkSw").IsChecked = spkOn;
            Get<CheckBox>("CamMicSw").IsChecked = micOn;
            Get<TextBlock>("CamSpkSub").Text = spkOn ? (live ? L.T("cam_spk_on") : AudioStateText()) : audioBad ? AudioStateText() : L.T("cam_spk_off");
            Get<TextBlock>("CamMicSub").Text = micOn ? (live ? L.F("cam_mic_on", micDev) : AudioStateText())
                : camMicNeedDev && micDev.Length == 0 ? L.T("cam_mic_nodev") : audioBad ? AudioStateText() : L.T("cam_mic_off");

            Get<CheckBox>("CamCapSw").IsChecked = Cap.Want;
            string cap;
            if (Cap.Want) cap = CapStateText() + (capLines.Count > 0 ? "\n" + capLines[capLines.Count - 1] : "");
            else if (Cap.State == ToolState.Missing || Cap.State == ToolState.Failed) cap = CapStateText();
            else cap = L.T("home_cap_off");
            Get<TextBlock>("CamCapSub").Text = cap;
        }

        void FillCameraCombos()
        {
            bool was = camLoading;
            camLoading = true;
            var cb = Get<ComboBox>("CamWhich");
            Fill(cb, "auto", L.T("cam_auto"), "back", L.T("cam_back"), "front", L.T("cam_front"), "ultrawide", L.T("cam_ultra"), "tele", L.T("cam_tele"));
            SelectByTag(cb, S.Str("cam.camera", "auto"));
            Get<TextBlock>("CamQualityText").Text = L.T("q_" + (int) Get<Slider>("CamQuality").Value);
            camLoading = was;
            if (Ctl != null) CtlBuild(true); // 言語を変えたら操作の行を作り直す
        }

        void CamSet(string key, object v)
        {
            if (camLoading) return;
            S.Set(key, v);
            if (Cam.Want) Cam.Restart();
            Refresh();
        }

        void StartCamera()
        {
            if (S.Bool("cam.wifi", false) && Environment.GetEnvironmentVariable("PGPU_TEST_CAM_ADDR") == null &&
                (S.Str("cam.addr", "").Length == 0 || ToolArgs.Digits(S.Str("cam.code", "")).Length == 0))
            {
                camMsg = L.T("cam_wifi_need");
                return;
            }
            camMsg = "";
            camFormat = null;
            Cam.Start();
            CtlStart();
            UpdatePreviewTimer();
        }

        void StopCamera()
        {
            Cam.Stop();
            CtlStop();
            camFormat = null;
            StopPreview();
        }

        void StopPreview()
        {
            if (previewTimer != null) { previewTimer.Stop(); previewTimer = null; }
            preview.Close();
            try { Get<Image>("CamImage").Source = null; } catch { }
        }

        void UpdatePreviewTimer()
        {
            bool want = Cam.Want && Current == "Camera" && S.Bool("cam.preview", true);
            if (want && previewTimer == null)
            {
                previewTimer = new DispatcherTimer(DispatcherPriority.Render, ui) { Interval = TimeSpan.FromMilliseconds(33) };
                previewTimer.Tick += (o, e) => DrawPreview();
                previewTimer.Start();
            }
            else if (!want && previewTimer != null)
            {
                previewTimer.Stop();
                previewTimer = null;
                if (!Cam.Want) { preview.Close(); Get<Image>("CamImage").Source = null; }
            }
        }

        void DrawPreview()
        {
            var src = preview.Poll(672);
            if (src != null) Get<Image>("CamImage").Source = src;
        }

        void CameraShown()
        {
            UpdatePreviewTimer();
            CheckVcam();
        }

        // webcam_rx の記録の行から様子を読む
        static readonly Regex FormatRx = new Regex(@"(決まった形|形が変わった)：(\d+) x (\d+)・(\d+) コマ/秒・(\d+) kbps・カメラ (\d+)");
        void CamLine(string line)
        {
            var m = FormatRx.Match(line);
            if (m.Success)
            {
                camFormat = new[] { int.Parse(m.Groups[2].Value), int.Parse(m.Groups[3].Value), int.Parse(m.Groups[4].Value), int.Parse(m.Groups[6].Value) };
                if (ctlFpsLowered == 0) camFpsMax = Math.Max(camFpsMax, camFormat[2]); // 熱で下げている間の形は数えない
                Cam.MarkRunning();
            }
            else if (line.Contains("iPhone との接続が切れた") || line.Contains("iPhone を待っています") || line.Contains("つなげない") || line.Contains("iPhone の返事が来ない"))
            {
                camFormat = null;
                Cam.MarkWaiting();
            }
        }

        // 0.5 秒ごと（どのページでも）：共有メモリのコマの番号が進んでいるかを見る
        void RefreshCameraState()
        {
            if (!Cam.Want) return;
            long seq = preview.CurrentSeq;
            if (seq != camSeq) { camSeq = seq; camSeqAt = DateTime.Now; }
        }
        bool CamFlowing { get { return Cam.Want && Cam.Running && (DateTime.Now - camSeqAt).TotalSeconds < 2; } }

        string CamStateText()
        {
            if (!Cam.Want && camMsg.Length > 0) return camMsg;
            if (Cam.Want && CamFlowing) return L.T("cam_state_on");
            if (Cam.Want && Cam.State == ToolState.Running) return L.T("st_wait");
            return ToolStateText(Cam, L.T("st_wait"));
        }

        void RefreshCamera()
        {
            bool flowing = CamFlowing;
            Get<TextBlock>("CamState").Text = CamStateText();
            string detail;
            if (!Cam.Want) detail = camMsg.Length > 0 ? "" : L.T("cam_detail_off");
            else if (flowing && camFormat != null)
            {
                string res = camFormat[1] >= 2160 ? "4K" : camFormat[1] + "p";
                detail = L.F("cam_detail_on", L.T("camname_" + Math.Max(0, Math.Min(3, camFormat[3]))), res, camFormat[2]) + "\n" + L.T("cam_keep_screen");
            }
            else if (flowing) detail = L.T("cam_keep_screen");
            else detail = L.T("cam_detail_wait");
            Get<TextBlock>("CamDetail").Text = detail;

            bool pv = S.Bool("cam.preview", true);
            var pbtn = Get<Button>("CamPreviewBtn");
            pbtn.Content = pv ? L.T("cam_preview_stop") : L.T("cam_preview_show");
            pbtn.IsEnabled = Cam.Want;
            var sbtn = Get<Button>("CamStartBtn");
            sbtn.Content = Cam.Want ? L.T("cam_stop") : L.T("cam_start");
            sbtn.Style = St(Cam.Want ? "Btn" : "BtnPrimary");
            string note = "";
            if (!Cam.Want) Get<Image>("CamImage").Source = null;
            if (Cam.Want && !pv) note = L.T("cam_preview_off");
            else if (Cam.Want && !flowing) note = L.T("cam_wait_video");
            Get<TextBlock>("CamPreviewNote").Text = note;
            if (Cam.Want && !flowing) Get<Image>("CamImage").Source = null;

            RefreshCamQuick();
            RefreshCamCtl();

            bool wifi = S.Bool("cam.wifi", false);
            Get<CheckBox>("CamWifi").IsChecked = wifi;
            Get<FrameworkElement>("CamWifiAddrRow").Visibility = wifi ? Visibility.Visible : Visibility.Collapsed;
            Get<FrameworkElement>("CamWifiCodeRow").Visibility = wifi ? Visibility.Visible : Visibility.Collapsed;

            // 仮想カメラ
            var vb = Get<Button>("CamVcamBtn");
            Get<TextBlock>("CamVcamState").Text = vcamState == "on" ? L.T("vcam_on") : vcamState == "off" ? L.T("vcam_off") : vcamState == "checking" ? L.T("vcam_checking") : vcamState == "partial" ? L.T("vcam_off") : "";
            vb.Content = vcamState == "on" || vcamState == "partial" ? L.T("vcam_btn_on") : L.T("vcam_btn_off");
            vb.IsEnabled = vcamState != "checking" && vcamState != "missing";
            Get<TextBlock>("CamVcamSub").Text = vcamMsg.Length > 0 ? vcamMsg : vcamState == "missing" ? L.F("st_missing", "camera_setup.exe") : L.T("cam_vcam_sub");
        }

        // ---- 仮想カメラ。登録は DLL の場所を覚えるので、動かない場所（データの置き場の camera）に写してから登録する
        string VcamExe()
        {
            string inst = Path.Combine(CamDir, "camera_setup.exe");
            if (File.Exists(inst)) return inst;
            return ToolPaths.Find("camera_setup");
        }

        void CheckVcam()
        {
            string exe = VcamExe();
            if (exe == null) { vcamState = "missing"; return; }
            if (vcamState == "checking") return;
            vcamState = "checking";
            RunVcam(exe, "status", null);
        }

        void RunVcam(string exe, string verb, string doneMsg)
        {
            var t = new Thread(() =>
            {
                int code;
                string outp = ChildTool.RunOnce(ToolArgs.CameraSetup(exe, verb), 30000, out code) ?? "";
                string st = "";
                if (verb != "status") st = ChildTool.RunOnce(ToolArgs.CameraSetup(exe, "status"), 15000, out code) ?? "";
                else st = outp;
                int yes = Regex.Matches(st, "登録されています").Count;
                ui.BeginInvoke(new Action(() =>
                {
                    vcamState = yes >= 2 ? "on" : yes == 1 ? "partial" : "off";
                    if (doneMsg != null) vcamMsg = yes == 1 ? L.T("vcam_partial") : doneMsg;
                    Refresh();
                }));
            }) { IsBackground = true };
            t.Start();
        }

        void VcamToggle()
        {
            if (vcamState == "on" || vcamState == "partial")
            {
                string exe = VcamExe();
                if (exe == null) return;
                vcamState = "checking";
                RunVcam(exe, "uninstall", L.T("vcam_done_uninstall"));
                return;
            }
            string src = ToolPaths.Find("camera_setup");
            if (src == null) { vcamState = "missing"; return; }
            try
            {
                AppPaths.Ensure(CamDir);
                string sdir = Path.GetDirectoryName(src);
                foreach (string f in new[] { "camera_setup.exe", "dshow_cam64.dll", "dshow_cam32.dll" })
                {
                    string from = Path.Combine(sdir, f), to = Path.Combine(CamDir, f);
                    if (!File.Exists(from)) throw new FileNotFoundException(from);
                    // 読み込まれていて上書きできないときは、同じ物ならそのまま使う
                    try { File.Copy(from, to, true); }
                    catch (IOException) { if (!File.Exists(to) || new FileInfo(to).Length != new FileInfo(from).Length) throw; }
                }
            }
            catch (Exception ex) { vcamMsg = L.F("vcam_fail", ex.Message); Refresh(); return; }
            vcamState = "checking";
            RunVcam(Path.Combine(CamDir, "camera_setup.exe"), "install", L.T("vcam_done_install"));
        }
    }
}
