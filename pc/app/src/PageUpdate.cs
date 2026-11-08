// 設定の「アップデート」：キットの tools\update.ps1 を使う（10/12）。
// - チェック：update.ps1 -Check -Json をウィンドウなしで走らせ、最後の行「PGPU-UPDATE-JSON {...}」を読む。比べ方は Update-PocketGPU.cmd と同じ
// - 更新：Update-PocketGPU.cmd -FromApp <このアプリの PID> -Yes を起動してから、このアプリを閉じる。
//   PocketGPU.exe 自身も置き換えるため。update.ps1 はこのアプリが閉じるのを待ってから始め、終わったら起動し直す
// - update.ps1 が無い古いキットでは、Discord の /kit で更新用のファイルを足す手順を案内する
// 試験用：PGPU_TEST_UPDATE_HIDDEN=1 で、更新をウィンドウなしで走らせる（PGPU_NOPROMPT=1 も渡す）
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows;
using System.Windows.Controls;

namespace PocketGPU
{
    partial class Shell
    {
        // idle・checking・latest・available・norelease・unsupported・error・starting・game
        public string UpdState = "idle";
        string updVersion = "", updReason = "", updMessage = "";
        int updFiles, updIpa;
        long updBytes;

        static string KitRoot { get { return ToolPaths.KitDir.TrimEnd('\\'); } }
        static string UpdateScript { get { return Path.Combine(Path.Combine(KitRoot, "tools"), "update.ps1"); } }
        static string UpdateCmd { get { return Path.Combine(KitRoot, "Update-PocketGPU.cmd"); } }
        static string PowerShellExe
        {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), @"WindowsPowerShell\v1.0\powershell.exe"); }
        }

        void InitUpdate()
        {
            Get<Button>("SetUpdBtn").Click += (o, e) =>
            {
                if (UpdState == "available") StartUpdate();
                else CheckUpdate();
            };
        }

        public void CheckUpdate()
        {
            if (UpdState == "checking" || UpdState == "starting") return;
            if (!File.Exists(UpdateScript) || !File.Exists(UpdateCmd)) { UpdState = "unsupported"; Refresh(); return; }
            UpdState = "checking";
            Refresh();
            string script = UpdateScript, kit = KitRoot, lang = L.Ja ? "ja" : "en";
            var t = new Thread(() =>
            {
                var r = RunCheck(script, kit, lang);
                ui.BeginInvoke(new Action(() => ApplyCheck(r)));
            }) { IsBackground = true, Name = "update-check" };
            t.Start();
        }

        // update.ps1 -Check -Json の結果。読めなければ state=error
        static Dictionary<string, object> RunCheck(string script, string kit, string lang)
        {
            var bad = new Dictionary<string, object> { { "state", "error" }, { "reason", "" }, { "message", "" } };
            try
            {
                var psi = new ProcessStartInfo(PowerShellExe, "-NoProfile -ExecutionPolicy Bypass -File \"" + script + "\" -Check -Json -KitDir \"" + kit + "\"")
                {
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    StandardOutputEncoding = Encoding.UTF8,
                    StandardErrorEncoding = Encoding.UTF8,
                    WorkingDirectory = kit
                };
                psi.EnvironmentVariables["PGPU_LANG"] = lang;
                psi.EnvironmentVariables["PGPU_NOPROMPT"] = "1";
                using (var p = Process.Start(psi))
                {
                    var o = p.StandardOutput.ReadToEndAsync();
                    var e = p.StandardError.ReadToEndAsync();
                    if (!p.WaitForExit(120000)) { try { p.Kill(); } catch { } bad["message"] = "timeout"; return bad; }
                    string json = null;
                    foreach (string line in o.Result.Split('\n'))
                        if (line.StartsWith("PGPU-UPDATE-JSON ")) json = line.Substring(17).Trim();
                    Program.Log("update check exit=" + p.ExitCode + (json == null ? " no json: " + (o.Result + e.Result).Trim() : ""));
                    if (json == null) { bad["message"] = "exit " + p.ExitCode; return bad; }
                    var d = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(json);
                    return d ?? bad;
                }
            }
            catch (Exception ex)
            {
                Program.Log("update check: " + ex.Message);
                bad["message"] = ex.Message;
                return bad;
            }
        }

        static string JStr(Dictionary<string, object> d, string k) { object v; return d.TryGetValue(k, out v) && v != null ? Convert.ToString(v, CultureInfo.InvariantCulture) : ""; }
        static long JNum(Dictionary<string, object> d, string k) { long n; return long.TryParse(JStr(d, k), out n) ? n : 0; }

        void ApplyCheck(Dictionary<string, object> r)
        {
            string state = JStr(r, "state");
            updReason = JStr(r, "reason");
            updMessage = JStr(r, "message");
            updVersion = JStr(r, "version");
            updFiles = (int) JNum(r, "files") + (int) JNum(r, "remove");
            updBytes = JNum(r, "bytes");
            object ipa;
            updIpa = r.TryGetValue("ipa", out ipa) && ipa is System.Collections.ICollection ? ((System.Collections.ICollection) ipa).Count : 0;
            if (state == "latest") UpdState = updReason == "norelease" ? "norelease" : "latest";
            else if (state == "available") UpdState = "available";
            else if (updReason == "noini") UpdState = "unsupported";
            else UpdState = "error";
            Program.Log("update " + UpdState + " version=" + updVersion + " files=" + updFiles + " bytes=" + updBytes + " ipa=" + updIpa + " reason=" + updReason);
            Refresh();
        }

        void StartUpdate()
        {
            if (GameRunning()) { UpdState = "game"; Refresh(); return; }
            if (!Program.AutoMode && MessageBox.Show(L.T("upd_confirm"), "PocketGPU", MessageBoxButton.OKCancel, MessageBoxImage.Question) != MessageBoxResult.OK) return;
            try
            {
                bool hidden = Environment.GetEnvironmentVariable("PGPU_TEST_UPDATE_HIDDEN") == "1";
                string args = "/c \"\"" + UpdateCmd + "\" -FromApp " + Process.GetCurrentProcess().Id + " -Yes\"";
                var psi = new ProcessStartInfo(Environment.GetEnvironmentVariable("ComSpec") ?? "cmd.exe", args)
                {
                    UseShellExecute = false,
                    CreateNoWindow = hidden,
                    WorkingDirectory = KitRoot
                };
                psi.EnvironmentVariables["PGPU_LANG"] = L.Ja ? "ja" : "en";
                if (hidden) psi.EnvironmentVariables["PGPU_NOPROMPT"] = "1";
                var p = Process.Start(psi);
                Program.Log("update start pid=" + (p != null ? p.Id : 0) + " " + args);
            }
            catch (Exception ex)
            {
                UpdState = "error";
                updMessage = ex.Message;
                Program.Log("update start: " + ex);
                Refresh();
                return;
            }
            UpdState = "starting";
            Refresh();
            // 閉じる。ウィンドウがあれば Close（Closing で道具を止める）。--offscreen の試験では同じ後片付けをして終わる
            ui.BeginInvoke(new Action(() =>
            {
                var w = Application.Current.MainWindow;
                if (w != null) w.Close();
                else { Shutdown(); Application.Current.Shutdown(); }
            }));
        }

        static string SizeText(long n)
        {
            if (n >= 1048576) return (n / 1048576.0).ToString("0.0", CultureInfo.InvariantCulture) + " MB";
            if (n >= 1024) return (n / 1024).ToString(CultureInfo.InvariantCulture) + " KB";
            return n + " B";
        }

        void RefreshUpdate()
        {
            var btn = Get<Button>("SetUpdBtn");
            var sub = Get<TextBlock>("SetUpdSub");
            btn.Content = L.T(UpdState == "available" ? "upd_apply" : "upd_check");
            btn.IsEnabled = UpdState != "checking" && UpdState != "starting";   // 未対応でも押せる。更新用のファイルを足したあと、もう一度チェックできる
            switch (UpdState)
            {
                case "checking": sub.Text = L.T("upd_checking"); break;
                case "latest": sub.Text = L.F("upd_latest", updVersion); break;
                case "norelease": sub.Text = L.T("upd_norelease"); break;
                case "available":
                    sub.Text = L.F("upd_available", updVersion, updFiles, SizeText(updBytes)) + (updIpa > 0 ? L.T("upd_available_ipa") : "");
                    break;
                case "unsupported": sub.Text = L.T("upd_unsupported"); break;
                case "starting": sub.Text = L.T("upd_starting"); break;
                case "game": sub.Text = L.T("upd_game"); break;
                case "error":
                    if (updReason == "token") sub.Text = L.T("upd_token");
                    else if (updReason == "network") sub.Text = L.T("upd_network");
                    else sub.Text = L.F("upd_error", updMessage.Length > 0 ? updMessage : updReason);
                    break;
                default: sub.Text = L.T("upd_idle"); break;
            }
        }
    }
}
