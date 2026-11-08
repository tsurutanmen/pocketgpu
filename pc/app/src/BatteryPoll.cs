// iPhone の電池の温度（℃）と残り：10 秒ごとに裏の糸で IphoneBattery.Read（1 回 70 ms ほど。UI の糸では呼ばない）。
// IphoneBattery.cs は別の係の部品（work/battemp）をそのまま写した物。差し替えるときもそのまま写す。試験の差し替えはこのファイルに置く。
// usbmuxd に iPhone が見えているときだけ読む。
// 読めないときは度数を出さない。温度の色は今までどおり iPhone の 4 段階で決める（PageCamCtl.cs）。
// 出す所：カメラの画面の見出しの横（温度の色の横に 36.9℃）・ホームの iPhone の行（温度と電池の残り）
using System;
using System.Collections.Generic;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;

namespace PocketGPU
{
    partial class Shell
    {
        public BatteryInfo Batt;
        public DateTime BattAt = DateTime.MinValue;
        string battErrLogged;

        void StartBatteryPoll()
        {
            var t = new Thread(BatteryLoop) { IsBackground = true, Name = "battery" };
            t.Start();
        }

        // 試験用（試す人は使わない）：
        //   PGPU_TEST_BATTERY=温度,残り,充電中   部品を呼ばずにこの値にする（例 36.9,82,1。空の所は読めなかった扱い。@ファイル なら毎回その 1 行目）
        //   PGPU_TEST_BATTERY_ALWAYS=1           iPhone が見えていなくても部品を呼ぶ（本物の部品の「認識されていません」の形を見る。usbmuxd の口は IPHONE_VK_MUX）
        static readonly string TestBatt = Environment.GetEnvironmentVariable("PGPU_TEST_BATTERY");
        static readonly bool TestBattAlways = Environment.GetEnvironmentVariable("PGPU_TEST_BATTERY_ALWAYS") == "1";

        void BatteryLoop()
        {
            bool test = !string.IsNullOrEmpty(TestBatt) || TestBattAlways;
            Thread.Sleep(2500); // 初めの機器の一覧（PollDevices）を待つ
            while (true)
            {
                var d = Dev;
                string udid = d != null && d.Dev != null ? d.Dev.Serial : null;
                if (udid != null || test)
                {
                    BatteryInfo b;
                    try { b = string.IsNullOrEmpty(TestBatt) ? IphoneBattery.Read(udid, 3000) : TestBattery(TestBatt); }
                    catch (Exception ex) { b = new BatteryInfo { Error = ex.Message }; }
                    if (b == null) b = new BatteryInfo { Error = "null" };
                    ui.BeginInvoke(new Action(() =>
                    {
                        Batt = b;
                        BattAt = DateTime.Now;
                        string e = b.Error ?? "";
                        if (e != battErrLogged) { battErrLogged = e; Program.Log("battery: " + (e.Length > 0 ? "error " + e : "ok") + " temp=" + b.TemperatureC + " pct=" + b.Percent + " charging=" + b.Charging); }
                    }));
                }
                else ui.BeginInvoke(new Action(() => { Batt = null; }));
                Thread.Sleep(10000);
            }
        }

        static BatteryInfo TestBattery(string t)
        {
            var b = new BatteryInfo();
            try
            {
                if (t.StartsWith("@")) t = System.IO.File.ReadAllLines(t.Substring(1))[0];
                string[] p = t.Split(',');
                double v;
                int n;
                if (p.Length > 0 && double.TryParse(p[0].Trim(), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out v)) b.TemperatureC = v;
                if (p.Length > 1 && int.TryParse(p[1].Trim(), out n)) b.Percent = n;
                if (p.Length > 2 && p[2].Trim().Length > 0) b.Charging = p[2].Trim() == "1";
                if (b.TemperatureC == null) b.Error = "試験：温度なし";
            }
            catch (Exception ex) { b.Error = ex.Message; }
            return b;
        }

        // 35 秒より古い値は使わない（読めなくなった・iPhone を抜いた）
        bool BattFresh { get { return Batt != null && (DateTime.Now - BattAt).TotalSeconds < 35; } }
        public double? BattTempC { get { return BattFresh ? Batt.TemperatureC : null; } }
        public int? BattPercent { get { return BattFresh ? Batt.Percent : null; } }
        public bool? BattCharging { get { return BattFresh ? Batt.Charging : null; } }

        public void RefreshHomeStatsForTest() { RefreshHomeStats(); PaintThermal(); }

        // ホームの iPhone の行の 2 行目：温度 正常 36.9℃・電池 82%・充電中
        void RefreshHomeStats()
        {
            var tb = Get<TextBlock>("HomeDevStats");
            tb.Inlines.Clear();
            bool any = false;
            if (ctlThermal >= 0 || CelsiusText().Length > 0)
            {
                tb.Inlines.Add(new Run(L.T("stat_temp") + " "));
                AddThermalRuns(tb.Inlines);
                any = true;
            }
            int? pct = BattPercent;
            if (pct != null)
            {
                if (any) tb.Inlines.Add(new Run(L.T("stat_sep")));
                tb.Inlines.Add(new Run(L.F("stat_batt", pct.Value)));
                if (BattCharging == true) tb.Inlines.Add(new Run(L.T("stat_sep") + L.T("stat_charging")));
                any = true;
            }
            Get<FrameworkElement>("HomeDevStatsRow").Visibility = any ? Visibility.Visible : Visibility.Collapsed;
        }
    }
}
