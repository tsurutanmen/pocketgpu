// ホーム：接続の状態と、各機能の入り切り
using System;
using System.IO;
using System.Windows.Controls;

namespace PocketGPU
{
    partial class Shell
    {
        void InitHome()
        {
            Get<Button>("HomeCheck").Click += (o, e) => { Show("Check"); Check.Run(1); };
            Get<Button>("HomeGpuLaunch").Click += (o, e) => LaunchPicked();
            OnSwitch(Get<CheckBox>("HomeCamSw"), on => { if (on) StartCamera(); else StopCamera(); Refresh(); });
            OnSwitch(Get<CheckBox>("HomeAudioSw"), on => { if (on) StartAudio(); else Audio.Stop(); Refresh(); });
            OnSwitch(Get<CheckBox>("HomeCapSw"), on => { if (on) Cap.Start(); else Cap.Stop(); Refresh(); });
            OnSwitch(Get<CheckBox>("HomeRecSw"), on => { SetRecording(on); Refresh(); });
            OnSwitch(Get<CheckBox>("HomeMediaOnly"), on => { S.Set("mediaOnly", on); ApplyMode(); Refresh(); });
        }

        void RefreshHome()
        {
            bool connected = Dev.Dev != null;
            Get<TextBlock>("HomeDevTitle").Text = connected ? DevName() : L.T("home_dev_none_title");
            Get<TextBlock>("HomeDevSub").Text = connected ? L.F("home_dev_ok_sub", DevStateText()) : (Dev.MuxUp ? L.T("home_dev_none_sub") : L.T("home_dev_nomux_sub"));

            int n = 0;
            foreach (var g in Games) if (g.Alive) n++;
            Get<TextBlock>("HomeGpuSub").Text = n > 0 ? L.F("home_gpu_running", n) : L.T("home_gpu_idle");

            Get<CheckBox>("HomeCamSw").IsChecked = Cam.Want;
            Get<TextBlock>("HomeCamSub").Text = Cam.Want || Cam.State == ToolState.Missing || Cam.State == ToolState.Failed ? CamStateText() : L.T("home_cam_off");
            Get<CheckBox>("HomeAudioSw").IsChecked = Audio.Want;
            Get<TextBlock>("HomeAudioSub").Text = Audio.Want || Audio.State == ToolState.Missing || Audio.State == ToolState.Failed ? AudioStateText() : L.T("home_audio_off");
            Get<CheckBox>("HomeCapSw").IsChecked = Cap.Want;
            Get<TextBlock>("HomeCapSub").Text = Cap.Want || Cap.State == ToolState.Missing || Cap.State == ToolState.Failed ? CapStateText() : L.T("home_cap_off");
            bool rec = File.Exists(AppPaths.RecordFlag);
            Get<CheckBox>("HomeRecSw").IsChecked = rec;
            Get<TextBlock>("HomeRecSub").Text = rec ? RecDetailText() : L.T("home_rec_off");
            Get<CheckBox>("HomeMediaOnly").IsChecked = S.Bool("mediaOnly", false);
            RefreshHomeStats(); // 温度と電池の残り（BatteryPoll.cs）
            PaintThermal();
        }
    }
}
