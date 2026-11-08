// 録画：ドライバの IPHONE_VK_RECORD_FLAG（そのファイルがある間だけ iPhone が録る）を、ボタンで作る・消す。
// 形式・持ち帰り・iPhone に残すかは、このアプリから起動するゲームの環境変数で渡す（ToolArgs.GameEnv）。
using System;
using System.IO;
using System.Windows.Controls;

namespace PocketGPU
{
    partial class Shell
    {
        DateTime recSince = DateTime.MinValue;
        bool recLoading;

        void InitRecord()
        {
            recLoading = true;
            Get<RadioButton>(S.Str("rec.codec", "hevc") == "h264" ? "RecH264" : "RecHevc").IsChecked = true;
            recLoading = false;
            Get<Button>("RecStartBtn").Click += (o, e) => { SetRecording(!File.Exists(AppPaths.RecordFlag)); Refresh(); };
            Get<Button>("RecOpenBtn").Click += (o, e) => OpenFolder(S.Str("rec.folder", ToolArgs.DefaultRecordDir));
            Get<RadioButton>("RecHevc").Checked += (o, e) => { if (!recLoading) S.Set("rec.codec", "hevc"); };
            Get<RadioButton>("RecH264").Checked += (o, e) => { if (!recLoading) S.Set("rec.codec", "h264"); };
            OnSwitch(Get<CheckBox>("RecAudio"), on => { S.Set("rec.audio", on); Refresh(); });
            OnSwitch(Get<CheckBox>("RecFetch"), on => { S.Set("rec.fetch", on); Refresh(); });
            OnSwitch(Get<CheckBox>("RecKeep"), on => { S.Set("rec.keep", on); Refresh(); });
            OnSwitch(Get<CheckBox>("RecPhotos"), on => { S.Set("rec.photos", on); Refresh(); });
            Get<Button>("RecFolderBtn").Click += (o, e) =>
            {
                using (var dlg = new System.Windows.Forms.FolderBrowserDialog { Description = L.T("rec_pick_folder"), SelectedPath = S.Str("rec.folder", ToolArgs.DefaultRecordDir) })
                    if (dlg.ShowDialog() == System.Windows.Forms.DialogResult.OK) { S.Set("rec.folder", dlg.SelectedPath); Refresh(); }
            };
            // 前に落ちて印が残っていたら消す（意図しない録画を始めない）
            try { if (File.Exists(AppPaths.RecordFlag)) File.Delete(AppPaths.RecordFlag); } catch { }
        }

        public void SetRecording(bool on)
        {
            try
            {
                if (on)
                {
                    AppPaths.Ensure(AppPaths.DataDir);
                    File.WriteAllText(AppPaths.RecordFlag, DateTime.Now.ToString("o"));
                    recSince = DateTime.Now;
                }
                else if (File.Exists(AppPaths.RecordFlag)) File.Delete(AppPaths.RecordFlag);
            }
            catch (Exception ex) { Program.Log("record flag: " + ex.Message); }
        }

        string RecDetailText()
        {
            if (!File.Exists(AppPaths.RecordFlag)) return L.T("rec_detail");
            string t = L.F("rec_detail_on", recSince == DateTime.MinValue ? File.GetCreationTime(AppPaths.RecordFlag).ToString("HH:mm") : recSince.ToString("HH:mm"));
            if (!GameRunning()) t += "\n" + L.T("rec_no_game");
            return t;
        }

        void RefreshRecord()
        {
            bool on = File.Exists(AppPaths.RecordFlag);
            Get<TextBlock>("RecState").Text = on ? L.T("rec_state_on") : L.T("rec_state_off");
            Get<TextBlock>("RecDetail").Text = RecDetailText();
            var b = Get<Button>("RecStartBtn");
            b.Content = on ? L.T("rec_stop") : L.T("rec_start");
            b.Style = St(on ? "Btn" : "BtnPrimary");
            bool fetch = S.Bool("rec.fetch", true);
            Get<CheckBox>("RecAudio").IsChecked = S.Bool("rec.audio", true);
            Get<CheckBox>("RecFetch").IsChecked = fetch;
            Get<CheckBox>("RecKeep").IsChecked = !fetch || S.Bool("rec.keep", true);
            Get<CheckBox>("RecKeep").IsEnabled = fetch;
            Get<CheckBox>("RecPhotos").IsChecked = S.Bool("rec.photos", false);
            Get<Button>("RecFolderBtn").IsEnabled = fetch;
            Get<TextBlock>("RecFolderSub").Text = L.F("rec_folder_sub", S.Str("rec.folder", ToolArgs.DefaultRecordDir));
        }
    }
}
