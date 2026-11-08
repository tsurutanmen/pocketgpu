// 設定：言語・テーマ・使い方・保存先・マイクとスピーカー・ツールの場所・バージョン・アップデート（PageUpdate.cs）
using System;
using System.IO;
using System.Reflection;
using System.Windows.Controls;

namespace PocketGPU
{
    partial class Shell
    {
        bool setLoading;

        void InitSettings()
        {
            FillSettingsCombos();
            Get<ComboBox>("SetLang").SelectionChanged += (o, e) =>
            {
                if (setLoading) return;
                string v = SelectedTag((ComboBox) o);
                if (v != null && v != S.Str("lang", "auto")) SetLanguage(v);
            };
            Get<ComboBox>("SetTheme").SelectionChanged += (o, e) =>
            {
                if (setLoading) return;
                string v = SelectedTag((ComboBox) o);
                if (v != null && v != S.Str("theme", "auto")) SetTheme(v);
            };
            OnSwitch(Get<CheckBox>("SetMediaOnly"), on => { S.Set("mediaOnly", on); ApplyMode(); Refresh(); });
            Get<Button>("SetDataOpen").Click += (o, e) => OpenFolder(AppPaths.DataDir);
            Get<TextBlock>("SetVersion").Text = L.F("set_version", Program.Version);
            InitUpdate();
        }

        void FillSettingsCombos()
        {
            setLoading = true;
            var cb = Get<ComboBox>("SetLang");
            Fill(cb, "auto", L.T("lang_auto"), "ja", L.T("lang_ja_self"), "en", L.T("lang_en_self"));
            SelectByTag(cb, S.Str("lang", "auto"));
            var th = Get<ComboBox>("SetTheme");
            Fill(th, "auto", L.T("theme_auto"), "light", L.T("theme_light"), "dark", L.T("theme_dark"));
            SelectByTag(th, S.Str("theme", "auto"));
            setLoading = false;
        }

        // マイクとスピーカーの欄は、マイクとスピーカーの画面と同じ設定（PageAudio.cs の RefreshAudioShared）
        void SettingsShown() { AudioShown(); }

        string toolsKey = "";
        void RefreshSettings()
        {
            Get<CheckBox>("SetMediaOnly").IsChecked = S.Bool("mediaOnly", false);
            Get<TextBlock>("SetDataPath").Text = AppPaths.DataDir;
            Get<TextBlock>("SetVersion").Text = L.F("set_version", Program.Version);
            RefreshUpdate();
            RefreshAudioShared("Set");
            string key = (L.Ja ? "ja" : "en") + ToolPaths.KitDir;
            if (key == toolsKey) return;
            toolsKey = key;
            var rows = Get<StackPanel>("SetToolRows");
            rows.Children.Clear();
            string[] icons = { "i_camera", "i_vcam", "i_mic", "i_caption", "i_check" };
            for (int i = 0; i < ToolPaths.Names.Length; i++)
            {
                string n = ToolPaths.Names[i];
                string p = ToolPaths.Find(n);
                rows.Children.Add(Row(i == 0, icons[i], L.T("tool_" + n) + (L.Ja ? "：" : ": ") + n + ".exe", p ?? L.T("tool_missing")));
            }
            string icd = Path.Combine(ToolPaths.KitDir, "iphone_icd.json");
            rows.Children.Add(Row(false, "i_gpu", L.T("tool_kit") + (L.Ja ? "：" : ": ") + "iphone_icd.json", File.Exists(icd) ? icd : L.T("tool_missing")));
        }
    }
}
