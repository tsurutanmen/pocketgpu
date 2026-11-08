// 字幕：livecap を起動・停止する。確定した文（livecap が標準出力に書く行）を最後の 3 行だけ見せる
using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;

namespace PocketGPU
{
    partial class Shell
    {
        readonly List<string> capLines = new List<string>();
        bool capReady, capLoading;
        static readonly Regex CapFinal = new Regex(@"^\[\s*[\d.]+-\s*[\d.]+\] (.*?)  (遅延|latency) \d+ ms$");

        void InitCaption()
        {
            capLoading = true;
            FillCaptionCombos();
            Get<CheckBox>("CapAccurate").IsChecked = S.Bool("cap.accurate", false);
            Get<CheckBox>("CapWindow").IsChecked = S.Bool("cap.window", true);
            Get<CheckBox>("CapObs").IsChecked = S.Bool("cap.obs", false);
            Get<CheckBox>("CapMinutes").IsChecked = S.Bool("cap.minutes", false);
            capLoading = false;

            Get<Button>("CapStartBtn").Click += (o, e) => { if (Cap.Want) Cap.Stop(); else { capLines.Clear(); Cap.Start(); } Refresh(); };
            Get<ComboBox>("CapSource").SelectionChanged += (o, e) => CapSet("cap.source", SelectedTag((ComboBox) o));
            Get<ComboBox>("CapLang").SelectionChanged += (o, e) => CapSet("cap.lang", SelectedTag((ComboBox) o));
            Get<ComboBox>("CapTranslate").SelectionChanged += (o, e) => CapSet("cap.translate", SelectedTag((ComboBox) o));
            OnSwitch(Get<CheckBox>("CapAccurate"), on => CapSet("cap.accurate", on));
            OnSwitch(Get<CheckBox>("CapWindow"), on => CapSet("cap.window", on));
            OnSwitch(Get<CheckBox>("CapObs"), on => CapSet("cap.obs", on));
            OnSwitch(Get<CheckBox>("CapMinutes"), on => CapSet("cap.minutes", on));
            Get<Button>("CapObsOpen").Click += (o, e) => OpenFolder(Path.GetDirectoryName(ToolArgs.CaptionObsFile));
            Get<Button>("CapMinutesOpen").Click += (o, e) => OpenFolder(ToolArgs.CaptionMinutesDir);

            Cap.Line += line =>
            {
                var m = CapFinal.Match(line.Trim());
                if (m.Success)
                {
                    capReady = true;
                    Cap.MarkRunning();
                    capLines.Add(m.Groups[1].Value);
                    if (capLines.Count > 3) capLines.RemoveAt(0);
                    return;
                }
                if (line.StartsWith("iPhone: ")) { capReady = true; Cap.MarkRunning(); }
            };
            Cap.StateChanged += () => { if (Cap.State != ToolState.Running) capReady = false; };
        }

        void FillCaptionCombos()
        {
            bool was = capLoading;
            capLoading = true;
            Fill(Get<ComboBox>("CapSource"), "loopback", L.T("cap_src_pc"), "mic", L.T("cap_src_mic"));
            Fill(Get<ComboBox>("CapLang"), "ja-JP", L.T("lang_ja"), "en-US", L.T("lang_en"));
            Fill(Get<ComboBox>("CapTranslate"), "", L.T("cap_tr_none"), "ja", L.T("lang_ja"), "en", L.T("lang_en"));
            SelectByTag(Get<ComboBox>("CapSource"), S.Str("cap.source", "loopback"));
            SelectByTag(Get<ComboBox>("CapLang"), S.Str("cap.lang", L.Ja ? "ja-JP" : "en-US"));
            SelectByTag(Get<ComboBox>("CapTranslate"), S.Str("cap.translate", ""));
            capLoading = was;
        }

        void CapSet(string key, object v)
        {
            if (capLoading || v == null) return;
            S.Set(key, v);
            if (Cap.Want) Cap.Restart();
            Refresh();
        }

        string CapStateText()
        {
            if (Cap.Want && capReady && Cap.Running) return L.T("cap_state_on");
            return ToolStateText(Cap, L.T("st_wait"));
        }

        void RefreshCaption()
        {
            Get<TextBlock>("CapState").Text = CapStateText();
            Get<TextBlock>("CapDetail").Text = !Cap.Want ? L.T("cap_detail_off") : S.Bool("cap.window", true) ? L.T("cap_detail_window") : L.T("cap_detail_nowin");
            var lines = Get<TextBlock>("CapLines");
            lines.Text = string.Join("\n", capLines.ToArray());
            lines.Visibility = Cap.Want && capLines.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
            var b = Get<Button>("CapStartBtn");
            b.Content = Cap.Want ? L.T("stop") : L.T("start");
            b.Style = St(Cap.Want ? "Btn" : "BtnPrimary");
            Get<CheckBox>("CapAccurate").IsChecked = S.Bool("cap.accurate", false);
            Get<CheckBox>("CapWindow").IsChecked = S.Bool("cap.window", true);
            Get<CheckBox>("CapObs").IsChecked = S.Bool("cap.obs", false);
            Get<CheckBox>("CapMinutes").IsChecked = S.Bool("cap.minutes", false);
            Get<TextBlock>("CapObsSub").Text = L.F("cap_obs_sub", ToolArgs.CaptionObsFile);
            Get<TextBlock>("CapMinutesSub").Text = L.F("cap_minutes_sub", ToolArgs.CaptionMinutesDir);
        }
    }
}
