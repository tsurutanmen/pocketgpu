// マイクとスピーカー：ipaudio を起動・停止する。出力の機器の一覧は ipaudio --list から作る。
// pcapp3 で足した設定（マイクとスピーカーの画面と、設定の画面の「マイクとスピーカー」の両方にある。名前の頭が Audio と Set で、後ろは同じ）：
//   audio.monitor・audio.monitorDev・audio.monitorVol   マイクの音を聞く。ipaudio の --mic-monitor と --mic-monitor-gain
//   audio.spkMutePc                                      iPhone から音を出す間は、音を取る出力をミュートする（AudioDevices.cs の PcMute）
//   audio.buf                                            スピーカーの遅延。ipaudio の --target-ms。短い 20・標準 40・長い 80
// ipaudio がマイクの音を聞く引数に対応しているかは、起動したときに 1 回見る（ToolArgs.AudioProbe）。対応していなければスイッチを押せない。
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;

namespace PocketGPU
{
    partial class Shell
    {
        List<string> outputs;        // 動作中の出力の機器
        string defaultOutput;        // 既定の出力の名前（一覧の「出力(既定)」）
        bool audioLoading, audioListing;
        string audioMsg = "";
        bool audioConnected;
        bool monRejected;            // 起動した ipaudio が --mic-monitor を知らなかった（一度だけ、付けずに起動し直す）
        string lastMuteKey;          // PC の音を消す：前に決めた「消す機器」。変わったときだけ機器に触る
        public PcMute Mute;
        static readonly string[] AudioPrefixes = { "Audio", "Set" };

        void InitAudio()
        {
            Mute = new PcMute(S);
            Mute.RestoreLeftover();   // 前に落ちたときにミュートしたままの機器を戻す

            audioLoading = true;
            Get<CheckBox>("AudioSpk").IsChecked = S.Bool("audio.spk", true);
            Get<CheckBox>("AudioMic").IsChecked = S.Bool("audio.mic", false);
            Get<CheckBox>("AudioVoice").IsChecked = S.Bool("audio.voice", false);
            foreach (string p in AudioPrefixes)
            {
                Get<RadioButton>(p + "Delay" + ToolArgs.SpeakerDelayMs(S)).IsChecked = true;
                Get<Slider>(p + "MonVol").Value = Math.Max(5, Math.Min(100, S.Int("audio.monitorVol", 100)));
            }
            audioLoading = false;

            Get<Button>("AudioStartBtn").Click += (o, e) => { if (Audio.Want) Audio.Stop(); else StartAudio(); Refresh(); };
            OnSwitch(Get<CheckBox>("AudioSpk"), on => AudioSet("audio.spk", on));
            OnSwitch(Get<CheckBox>("AudioMic"), on => AudioSet("audio.mic", on));
            OnSwitch(Get<CheckBox>("AudioVoice"), on => AudioSet("audio.voice", on));
            Get<ComboBox>("AudioSpkDev").SelectionChanged += (o, e) => { if (!audioLoading) AudioSet("audio.spkDev", SelectedTag((ComboBox) o) ?? ""); };
            Get<ComboBox>("AudioMicDev").SelectionChanged += (o, e) => { if (!audioLoading) AudioSet("audio.micDev", SelectedTag((ComboBox) o) ?? ""); };

            // 2 つの画面にある同じ設定
            foreach (string pre in AudioPrefixes)
            {
                string p = pre;
                OnSwitch(Get<CheckBox>(p + "MutePc"), on => { S.Set("audio.spkMutePc", on); UpdatePcMute(); Refresh(); });
                OnSwitch(Get<CheckBox>(p + "Mon"), on => AudioSet("audio.monitor", on));
                foreach (int ms in new[] { 20, 40, 80 })
                {
                    int v = ms;
                    Get<RadioButton>(p + "Delay" + v).Checked += (o, e) => { if (!audioLoading) AudioSet("audio.buf", v); };
                }
                Get<ComboBox>(p + "MonDev").SelectionChanged += (o, e) => { if (!audioLoading) AudioSet("audio.monitorDev", SelectedTag((ComboBox) o) ?? ""); };
                var vol = Get<Slider>(p + "MonVol");
                vol.ValueChanged += (o, e) => Get<TextBlock>(p + "MonVolText").Text = L.F("audio_mon_vol_val", (int) vol.Value);
                Action commit = () => { if (!audioLoading) AudioSet("audio.monitorVol", (int) vol.Value); };
                vol.AddHandler(Thumb.DragCompletedEvent, new DragCompletedEventHandler((o, e) => commit()));
                vol.PreviewMouseUp += (o, e) => commit();
                vol.KeyUp += (o, e) => commit();
                Get<TextBlock>(p + "MonVolText").Text = L.F("audio_mon_vol_val", (int) vol.Value);
            }

            Audio.Line += line =>
            {
                if (line.StartsWith("つながった") || line.StartsWith("t=")) { audioConnected = true; Audio.MarkRunning(); UpdatePcMute(); }
                else if (line.StartsWith("[exit")) audioConnected = false;
                // 前の ipaudio は知らない引数で止まる。マイクの音を聞く引数で止まったら、対応していないと覚えて、付けずに起動し直す
                if (line.Contains("--mic-monitor") && (line.Contains("知らない引数") || line.IndexOf("unknown", StringComparison.OrdinalIgnoreCase) >= 0))
                {
                    monRejected = true;
                    ToolArgs.MonitorSupported = false;
                    Program.Log("ipaudio rejected --mic-monitor: " + line);
                }
            };
            Audio.StateChanged += () =>
            {
                if (Audio.State != ToolState.Running) audioConnected = false;
                if (Audio.State == ToolState.Failed && monRejected)
                {
                    monRejected = false;
                    ui.BeginInvoke(new Action(() => { if (!Audio.Want) { Audio.Start(); Refresh(); } }));
                }
                if (ui.CheckAccess()) UpdatePcMute(); // 閉じるときは裏の糸から来る。そのときは Shutdown が戻す
            };

            ProbeAudio();
        }

        // 設定を変えた。ipaudio に渡す引数が変わるときだけ、動いている ipaudio を起動し直す
        void AudioSet(string key, object v)
        {
            if (audioLoading) return;
            string before = S.Str(key, "\u0001"), after = Convert.ToString(v, CultureInfo.InvariantCulture);
            if (before == after) return;
            string lineBefore = ToolArgs.Audio(S).Line;
            S.Set(key, v);
            audioMsg = "";
            if (Audio.Want && ToolArgs.Audio(S).Line != lineBefore) Audio.Restart();
            UpdatePcMute();
            Refresh();
        }

        public void StartAudio()
        {
            bool spk = S.Bool("audio.spk", true), mic = S.Bool("audio.mic", false) && S.Str("audio.micDev", "").Length > 0;
            if (!spk && !mic)
            {
                audioMsg = S.Bool("audio.mic", false) ? L.T("audio_need_micdev") : L.T("audio_none");
                return;
            }
            audioMsg = S.Bool("audio.mic", false) && !mic ? L.T("audio_need_micdev") : "";
            Audio.Start();
        }

        // ---- PC の音を消す：iPhone とつながっていて、スピーカーを使っている間だけ、音を取る出力をミュートする
        // つながる前と、切れて待っている間は消さない（その間は PC から聞こえるように）。
        // 設定を変えて ipaudio を起動し直す間や、すぐつながり直すときに一瞬だけ PC から鳴らないよう、切れてから 5 秒は消したままにする
        DateTime audioLiveAt = DateTime.MinValue;
        public void UpdatePcMute()
        {
            if (Mute == null) return;
            bool connected = Audio.Want && Audio.Running && audioConnected;
            if (connected) audioLiveAt = DateTime.Now;
            bool live = connected || (Audio.Want && (DateTime.Now - audioLiveAt).TotalSeconds < 5);
            bool want = S.Bool("audio.spkMutePc", false) && live && S.Bool("audio.spk", true);
            string key = want ? "dev:" + S.Str("audio.spkDev", "") : null;
            if (key == lastMuteKey) return;
            lastMuteKey = key;
            if (want) Mute.Apply(S.Str("audio.spkDev", ""));
            else Mute.Restore();
        }

        // ---- 機器の一覧と、マイクの音を聞く引数に対応しているか（起動したときに 1 回。裏の糸で）
        void ProbeAudio()
        {
            if (audioListing) return;
            audioListing = true;
            var t = new Thread(() =>
            {
                int code;
                bool? supported;
                string o;
                if (ToolPaths.Find("ipaudio") == null) { o = ""; supported = false; }
                else
                {
                    o = ChildTool.RunOnce(ToolArgs.AudioProbe(), 15000, out code) ?? "";
                    supported = code == 0 && o.IndexOf("知らない引数", StringComparison.Ordinal) < 0;
                    Program.Log("ipaudio probe --mic-monitor: exit " + code + " supported=" + supported + (supported == true ? "" : " " + FirstLine(o)));
                    if (supported != true) o = ChildTool.RunOnce(ToolArgs.AudioList(), 15000, out code) ?? "";
                }
                if (Environment.GetEnvironmentVariable("PGPU_TEST_MON_ASSUME") == "1") supported = true;
                var list = new List<string>();
                string def = null;
                // 出力(既定) [動作中] 名前  48000Hz 2ch float 音量1.00
                foreach (string line in o.Split('\n'))
                {
                    var m = Regex.Match(line.TrimEnd('\r'), @"^出力(\(既定\))? \[動作中\] (.+?)  ");
                    if (!m.Success) continue;
                    list.Add(m.Groups[2].Value);
                    if (m.Groups[1].Success) def = m.Groups[2].Value;
                }
                ui.BeginInvoke(new Action(() =>
                {
                    outputs = list;
                    defaultOutput = def;
                    audioListing = false;
                    bool lineChanged = ToolArgs.MonitorSupported != supported;
                    string before = ToolArgs.Audio(S).Line;
                    ToolArgs.MonitorSupported = supported;
                    FillAudioCombos();
                    if (lineChanged && Audio.Want && ToolArgs.Audio(S).Line != before) Audio.Restart();
                    Refresh();
                }));
            }) { IsBackground = true };
            t.Start();
        }
        static string FirstLine(string s)
        {
            s = (s ?? "").Trim();
            int n = s.IndexOf('\n');
            return n >= 0 ? s.Substring(0, n).Trim() : s;
        }

        // 機器の一覧（マイクとスピーカーの画面・設定の画面を開いた時と、言語を変えた時）
        void AudioShown()
        {
            if (outputs != null) FillAudioCombos();
            else ProbeAudio();
        }

        void FillAudioCombos()
        {
            if (outputs == null) return;
            audioLoading = true;
            var a = new List<string> { "", L.T("audio_default_out") };
            var b = new List<string> { "", L.T("audio_mic_choose") };
            foreach (string n in outputs) { a.Add(n); a.Add(n); b.Add(n); b.Add(n); }
            string sd = S.Str("audio.spkDev", ""), md = S.Str("audio.micDev", ""), mon = S.Str("audio.monitorDev", "");
            var c = new List<string>(a);
            if (sd.Length > 0 && !outputs.Contains(sd)) { a.Add(sd); a.Add(sd); }
            if (md.Length > 0 && !outputs.Contains(md)) { b.Add(md); b.Add(md); }
            if (mon.Length > 0 && !outputs.Contains(mon)) { c.Add(mon); c.Add(mon); }
            Fill(Get<ComboBox>("AudioSpkDev"), a.ToArray());
            Fill(Get<ComboBox>("AudioMicDev"), b.ToArray());
            SelectByTag(Get<ComboBox>("AudioSpkDev"), sd);
            SelectByTag(Get<ComboBox>("AudioMicDev"), md);
            foreach (string p in AudioPrefixes)
            {
                Fill(Get<ComboBox>(p + "MonDev"), c.ToArray());
                SelectByTag(Get<ComboBox>(p + "MonDev"), mon);
            }
            audioLoading = false;
        }

        // 聞く出力と音を取る出力が同じ機器か（どちらも空なら既定の出力どうし）
        bool MonitorSameAsCapture()
        {
            string m = S.Str("audio.monitorDev", ""), s = S.Str("audio.spkDev", "");
            if (m.Length == 0) m = defaultOutput ?? "";
            if (s.Length == 0) s = defaultOutput ?? "";
            if (m.Length == 0 && s.Length == 0) return S.Str("audio.monitorDev", "") == S.Str("audio.spkDev", "");
            return string.Equals(m, s, StringComparison.OrdinalIgnoreCase);
        }
        string MonitorDevName()
        {
            string m = S.Str("audio.monitorDev", "");
            return m.Length > 0 ? m : L.T("audio_default_out");
        }
        bool MonitorInUse { get { return S.Bool("audio.monitor", false) && ToolArgs.MonitorSupported == true && S.Bool("audio.mic", false) && S.Str("audio.micDev", "").Length > 0; } }

        string AudioStateText()
        {
            if (!Audio.Want && audioMsg.Length > 0) return audioMsg;
            if (Audio.Want && audioConnected && Audio.Running) return L.T("audio_state_on");
            return ToolStateText(Audio, L.T("st_wait"));
        }

        void RefreshAudio()
        {
            Get<TextBlock>("AudioState").Text = AudioStateText();
            string d;
            if (!Audio.Want) d = audioMsg.Length > 0 ? "" : L.T("audio_detail_off");
            else
            {
                var parts = new List<string>();
                if (S.Bool("audio.spk", true)) parts.Add(L.T("audio_spk_active"));
                if (Mute.Active) parts.Add(L.F("audio_mute_pc_active", Mute.MutedName));
                if (S.Bool("audio.mic", false) && S.Str("audio.micDev", "").Length > 0) parts.Add(L.F("audio_mic_active", S.Str("audio.micDev", "")));
                if (MonitorInUse) parts.Add(L.F("audio_mon_active", MonitorDevName()));
                if (audioMsg.Length > 0) parts.Add(audioMsg);
                d = string.Join("\n", parts.ToArray());
                if (!audioConnected) d = L.T("audio_detail_off");
            }
            Get<TextBlock>("AudioDetail").Text = d;
            var b = Get<Button>("AudioStartBtn");
            b.Content = Audio.Want ? L.T("stop") : L.T("start");
            b.Style = St(Audio.Want ? "Btn" : "BtnPrimary");
            Get<CheckBox>("AudioSpk").IsChecked = S.Bool("audio.spk", true);
            Get<CheckBox>("AudioMic").IsChecked = S.Bool("audio.mic", false);
            Get<CheckBox>("AudioVoice").IsChecked = S.Bool("audio.voice", false);
            Get<ComboBox>("AudioSpkDev").IsEnabled = S.Bool("audio.spk", true);
            Get<ComboBox>("AudioMicDev").IsEnabled = S.Bool("audio.mic", false);
            Get<CheckBox>("AudioVoice").IsEnabled = S.Bool("audio.mic", false);
            Get<CheckBox>("AudioMutePc").IsEnabled = S.Bool("audio.spk", true);
            RefreshAudioShared("Audio");
        }

        // 2 つの画面にある同じ設定の表示
        void RefreshAudioShared(string p)
        {
            bool was = audioLoading;
            audioLoading = true;
            try
            {
                // PC の音を消す
                Get<CheckBox>(p + "MutePc").IsChecked = S.Bool("audio.spkMutePc", false);
                string ms;
                if (Mute.Active) ms = L.F("audio_mute_pc_on", Mute.MutedName);
                else if (S.Bool("audio.spkMutePc", false) && Mute.AlreadyMutedName != null && lastMuteKey != null) ms = L.F("audio_mute_pc_already", Mute.AlreadyMutedName);
                else ms = L.T("audio_mute_pc_sub");
                Get<TextBlock>(p + "MutePcSub").Text = ms;

                // スピーカーの遅延
                var rb = Get<RadioButton>(p + "Delay" + ToolArgs.SpeakerDelayMs(S));
                if (rb.IsChecked != true) rb.IsChecked = true;

                // マイクの音を聞く
                bool sup = ToolArgs.MonitorSupported == true, mon = S.Bool("audio.monitor", false);
                var sw = Get<CheckBox>(p + "Mon");
                sw.IsChecked = mon && sup;
                sw.IsEnabled = sup;
                string sub;
                if (ToolPaths.Find("ipaudio") == null) sub = L.F("st_missing", "ipaudio.exe");
                else if (ToolArgs.MonitorSupported == null) sub = L.T("audio_mon_checking");
                else if (!sup) sub = L.T("audio_mon_old");
                else if (mon && S.Bool("audio.spk", true) && MonitorSameAsCapture()) sub = S.Bool("audio.spkMutePc", false) ? L.T("audio_mon_muted") : L.T("audio_mon_loop");
                else sub = L.T("audio_mon_sub");
                Get<TextBlock>(p + "MonSub").Text = sub;
                Get<ComboBox>(p + "MonDev").IsEnabled = sup && mon;
                var vol = Get<Slider>(p + "MonVol");
                vol.IsEnabled = sup && mon;
                int v = Math.Max(5, Math.Min(100, S.Int("audio.monitorVol", 100)));
                if (!vol.IsMouseCaptureWithin && !vol.IsKeyboardFocusWithin && (int) vol.Value != v) vol.Value = v;
                Get<TextBlock>(p + "MonVolText").Text = L.F("audio_mon_vol_val", (int) vol.Value);
                Get<TextBlock>(p + "MonVolText").Opacity = vol.IsEnabled ? 1.0 : 0.45;
                // 一覧の選び直し（もう一方の画面で変えたとき）
                var cb = Get<ComboBox>(p + "MonDev");
                if ((SelectedTag(cb) ?? "") != S.Str("audio.monitorDev", "")) SelectByTag(cb, S.Str("audio.monitorDev", ""));
            }
            finally { audioLoading = was; }
        }
    }
}
