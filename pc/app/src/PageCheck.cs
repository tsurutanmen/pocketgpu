// チェック：CheckRunner の 7 つの項目を行にして見せる。NG の行には案内と「もう一度」
using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace PocketGPU
{
    partial class Shell
    {
        void InitCheck()
        {
            Get<Button>("CheckRun").Click += (o, e) => Check.Run(1);
            Get<Button>("CheckCopy").Click += (o, e) =>
            {
                try { Clipboard.SetText(Check.Text()); ((Button) o).Content = L.T("copied"); } catch { }
            };
            Check.Updated += () => { if (Current == "Check") RenderCheck(); };
        }

        void RefreshCheck()
        {
            Get<TextBlock>("CheckIntro").Text = Check.MediaOnly ? L.T("check_intro_media") : L.T("check_intro");
            Get<TextBlock>("CheckWhen").Text = Check.Busy ? L.T("check_running") : Check.LastRun == null ? L.T("check_never")
                : L.F("check_when", Check.LastRun.Value.Date == DateTime.Today ? L.F("check_today", Check.LastRun.Value) : Check.LastRun.Value.ToString("g"));
            Get<Button>("CheckRun").IsEnabled = !Check.Busy;
            RenderCheck();
        }

        string checkKey = "";
        void RenderCheck()
        {
            var sb = new System.Text.StringBuilder(L.Ja ? "ja" : "en");
            foreach (var it in Check.Items) { sb.Append('|').Append(it.State).Append(it.Detail).Append(it.More.Count).Append(it.ActionText); }
            sb.Append(Check.Busy);
            if (sb.ToString() == checkKey) return;
            checkKey = sb.ToString();

            var rows = Get<StackPanel>("CheckRows");
            rows.Children.Clear();
            bool allOk = !Check.Busy && Check.LastRun != null;
            foreach (var it in Check.Items)
            {
                if (it.State == CheckState.Ng || it.State == CheckState.Wait || it.State == CheckState.Running) allOk = false;
                rows.Children.Add(CheckRow(it, rows.Children.Count == 0));
            }
            Get<FrameworkElement>("CheckDone").Visibility = allOk && !Check.MediaOnly ? Visibility.Visible : Visibility.Collapsed;
            Get<TextBlock>("CheckWhen").Text = Check.Busy ? L.T("check_running") : Get<TextBlock>("CheckWhen").Text;
        }

        Border CheckRow(CheckItem it, bool first)
        {
            bool bad = it.State == CheckState.Ng || it.State == CheckState.Warn;
            var row = new Border { Style = St(first ? "Row" : "RowNext") };
            if (bad) row.Padding = new Thickness(20, 12, 20, 12);
            var dp = new DockPanel();
            string icon = it.State == CheckState.Ok ? "i_check" : bad ? "i_warn" : "i_circle";
            var ic = Icon(icon, "RowIco");
            if (bad) { ic.VerticalAlignment = VerticalAlignment.Top; ic.Margin = new Thickness(0, 1, 16, 0); }
            if (it.State == CheckState.Wait || it.State == CheckState.Skip) ic.SetResourceReference(Control.ForegroundProperty, "Faint");
            dp.Children.Add(ic);

            // 右側：値か、もう一度・案内のボタン
            var right = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = bad ? VerticalAlignment.Top : VerticalAlignment.Center };
            DockPanel.SetDock(right, Dock.Right);
            if (bad && !Check.Busy)
            {
                if (it.ActionText != null)
                {
                    var act = it.Action;
                    right.Children.Add(Btn(it.ActionText, "BtnRow", (o, e) => { if (act != null) act(); }));
                }
                if (it.CanRetry)
                {
                    int n = it.N;
                    right.Children.Add(Btn(L.T("check_retry"), "BtnRow", (o, e) => Check.Run(n)));
                }
                else right.Children.Add(Text(L.T("v_warn"), "Val"));
            }
            else
            {
                string v = it.State == CheckState.Ok ? L.T("v_ok") : it.State == CheckState.Running ? L.T("v_run") : it.State == CheckState.Skip ? L.T("v_skip") : L.T("v_wait");
                right.Children.Add(Text(v, "Val"));
            }
            dp.Children.Add(right);

            var mid = new StackPanel { VerticalAlignment = bad ? VerticalAlignment.Top : VerticalAlignment.Center };
            var title = Text(L.T("c" + it.N), "RowTitle");
            if (bad) title.FontWeight = FontWeights.SemiBold;
            mid.Children.Add(title);
            string detail = it.Detail.Length > 0 ? it.Detail : L.T("c" + it.N + "d");
            var sub = Text(detail, "RowSub");
            if (bad) { sub.SetResourceReference(TextBlock.ForegroundProperty, "Ink"); sub.LineHeight = 20; }
            mid.Children.Add(sub);
            foreach (string m in it.More)
            {
                var t = Text(m, "RowSub");
                if (bad) { t.SetResourceReference(TextBlock.ForegroundProperty, "Ink"); t.LineHeight = 20; }
                mid.Children.Add(t);
            }
            dp.Children.Add(mid);
            row.Child = dp;
            return row;
        }
    }
}
