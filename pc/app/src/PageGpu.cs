// GPU：ゲームやアプリを iPhone の GPU で起動する。実行中のアプリ・最近のアプリ・Steam の 1 行・iPhone の熱
using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Windows;
using System.Windows.Controls;

namespace PocketGPU
{
    partial class Shell
    {
        string heatText = "", heatSub = "";
        bool heatBusy;
        string gpuRowsKey = "";

        void InitGpu()
        {
            Get<Button>("GpuLaunch").Click += (o, e) => LaunchPicked();
            Get<Button>("GpuGoCheck").Click += (o, e) => { Show("Check"); Check.Run(1); };
            Get<Button>("CheckGoGpu").Click += (o, e) => LaunchPicked();
            Get<Button>("GpuSteamCopy").Click += (o, e) =>
            {
                try { Clipboard.SetText(ToolArgs.SteamLine); ((Button) o).Content = L.T("copied"); } catch { }
            };
            Get<Button>("GpuStatusBtn").Click += (o, e) => RefreshHeat();
            OnSwitch(Get<CheckBox>("GpuDiag"), on => { S.Set("gpu.diag", on); Refresh(); });
        }

        void LaunchPicked()
        {
            string f = GameLaunch.Pick();
            if (f != null) LaunchGame(f);
        }

        public void LaunchGame(string path)
        {
            var g = GameLaunch.Launch(path, S, GameRunning());
            if (g == null) return;
            Games.Add(g);
            var recent = S.List("gpu.recent");
            recent.RemoveAll(x => string.Equals(x, path, StringComparison.OrdinalIgnoreCase));
            recent.Insert(0, path);
            if (recent.Count > 6) recent.RemoveRange(6, recent.Count - 6);
            S.SetList("gpu.recent", recent);
            gpuRowsKey = "";
            Refresh();
        }

        // 熱の状態は、このアプリから起動したゲームが動いていない時だけ聞く（受け手は 1 人しか相手をしない）
        void RefreshHeat()
        {
            if (heatBusy) return;
            if (GameRunning()) { heatSub = L.T("gpu_heat_busy"); Refresh(); return; }
            if (Dev.Dev == null || Dev.Dev.Type != "USB") { heatText = L.T("heat_unknown"); heatSub = L.T("gpu_heat_sub"); Refresh(); return; }
            heatBusy = true;
            long id = Dev.Dev.Id;
            var t = new Thread(() =>
            {
                int th;
                var h = Mux.TestReceiver(id, 2500, true, out th);
                ui.BeginInvoke(new Action(() =>
                {
                    heatBusy = false;
                    if (h.Ok) { Check.LastHello = h; Check.LastThermal = th; }
                    heatText = !h.Ok ? L.T("heat_unknown") : th >= 0 && th <= 3 ? L.T("heat_" + th) : L.T("heat_unknown");
                    heatSub = !h.Ok ? L.T("heat_fail") : th == -2 ? L.T("heat_old") : L.T("gpu_heat_sub");
                    Refresh();
                }));
            }) { IsBackground = true };
            t.Start();
        }

        void RefreshGpu()
        {
            bool media = S.Bool("mediaOnly", false);
            var h = Check.LastHello;
            if (media) Get<TextBlock>("GpuState").Text = L.T("gpu_state_name");
            else if (Dev.Dev == null) Get<TextBlock>("GpuState").Text = L.T("gpu_state_none");
            else Get<TextBlock>("GpuState").Text = h != null && h.Name.Length > 0 ? h.Name : L.T("gpu_state_name");
            string detail = media ? L.T("gpu_detail_media") : L.T("gpu_detail");
            if (!media && h != null && h.MemMB > 0) detail = L.F("gpu_detail_mem", h.MemMB / 1024.0) + "\n" + detail;
            Get<TextBlock>("GpuDetail").Text = detail;
            Get<Button>("GpuLaunch").IsEnabled = !media;
            Get<TextBlock>("GpuSteamLine").Text = ToolArgs.SteamLine;
            Get<CheckBox>("GpuDiag").IsChecked = S.Bool("gpu.diag", false);
            if (heatText.Length == 0) { heatText = Check.LastThermal >= 0 && Check.LastThermal <= 3 ? L.T("heat_" + Check.LastThermal) : L.T("heat_unknown"); heatSub = L.T("gpu_heat_sub"); }
            Get<TextBlock>("GpuStatusVal").Text = heatBusy ? L.T("vcam_checking") : heatText;
            Get<TextBlock>("GpuStatusSub").Text = heatSub;
            Get<Button>("GpuStatusBtn").IsEnabled = !heatBusy && !GameRunning();

            // 実行中のアプリ（コマ数は「コマ数を記録」のときだけ）
            foreach (var g in Games) if (g.Alive) g.Fps = GameLaunch.ReadFps(g);
            var run = Get<StackPanel>("GpuRunningRows");
            var key = new System.Text.StringBuilder(L.Ja ? "ja" : "en");
            foreach (var g in Games) key.Append('|').Append(g.Proc.Id).Append(g.Alive).Append(g.Fps.ToString("F1"));
            foreach (string p in S.List("gpu.recent")) key.Append('|').Append(p);
            if (key.ToString() == gpuRowsKey) return;
            gpuRowsKey = key.ToString();
            run.Children.Clear();
            bool first = true;
            foreach (var g in Games)
            {
                if (!g.Alive) continue;
                string sub = L.F("gpu_running_since", g.Started.ToString("HH:mm"));
                var right = new List<UIElement>();
                if (g.Fps >= 0) right.Add(Text(L.F("gpu_fps", g.Fps), "Val"));
                run.Children.Add(Row(first, "i_play", g.Name, sub, right.ToArray()));
                first = false;
            }
            if (first) run.Children.Add(Row(true, "i_play", L.T("gpu_running_none"), null));

            var rec = Get<StackPanel>("GpuRecentRows");
            rec.Children.Clear();
            first = true;
            foreach (string p in S.List("gpu.recent"))
            {
                string path = p;
                var b = Btn(L.T("gpu_run"), "BtnRow", (o, e) => LaunchGame(path));
                b.IsEnabled = !media && File.Exists(path);
                rec.Children.Add(Row(first, "i_app", Path.GetFileNameWithoutExtension(path), path, b));
                first = false;
            }
            if (first) rec.Children.Add(Row(true, "i_app", L.T("gpu_recent_none"), null));
        }
    }
}
