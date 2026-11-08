// カメラの画面の「PC からの操作」（camctl）。webcam_rx から届いた caps の項目だけを出す（無い物は出さない）。
// まとまりと順と言葉は iPhone のアプリのカメラタブ（app/Sources/CameraBridge.swift の CameraControlsView）と同じ：
//   映像（XAML にある PC 側の設定）・向き・ズームと切り抜き・ピント・明るさ・色・ライト・手ぶれ補正・背景ぼかしとエフェクト・熱の対策・画面
// 値の同期：
//   ・state が届いたら、画面の値を直す。ただし利用者が触っている操作（スライダーをつかんでいる間と、最後に送ってから 0.7 秒）は
//     届いた値で動かさない（手元の値が跳ねない）。保留が切れたら、最後に届いた値に合わせる。
//   ・スライダーは 1 つの番号につき 0.1 秒に 1 回までにまとめて送る（最後の値は必ず送る）。選ぶ・スイッチ・ボタンはすぐ送る。
// 温度：iPhone の 4 段階（event thermal か state 102）を信号の 3 色で、カメラの画面の見出しの横に出す。危険は「危険」を太字の赤で出し、点を点滅させる。
//   電池の温度（℃・BatteryPoll.cs）が読めれば横に出す。4 段階がまだ無いときだけ、℃ で仮に色を決める（PROTOCOL.md の案：40℃ 未満は緑・45℃ 未満は黄・それ以上は赤）。
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Documents;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;

namespace PocketGPU
{
    // 画面の 1 行（操作 1 つ）
    class CtlRow
    {
        public CtlItem Item;
        public Border Row;
        public TextBlock Title, Sub, ValueText;
        public FrameworkElement Input;
        public Slider Slider;
        public ComboBox Combo;
        public List<RadioButton> Seg;
        public CheckBox Switch;
        public Button Button;
        public List<Button> Stops;
        public bool Dragging;
        public bool Indent;      // 上の行に付く行（切り抜くの位置など）。アイコンを出さずに字下げする
        public bool ReadOnly;    // 読むだけの値（ポートレート・スタジオ照明）
    }

    class CtlGroup
    {
        public string Key, Icon;
        public int[] Ids;
        public StackPanel Box;   // 見出しとカード
        public StackPanel Rows;
        public readonly List<CtlRow> List = new List<CtlRow>();
    }

    partial class Shell
    {
        public CamCtl Ctl;
        public bool? ctlIphone, ctlCtrl;                 // status：iPhone と接続中か・iPhone のアプリが PC からの操作を受けるか
        public List<CtlItem> ctlCaps;                    // null は届いていない
        string ctlCapsSig;
        public readonly Dictionary<int, double> ctlValues = new Dictionary<int, double>(); // 届いた今の値
        public readonly Dictionary<int, double> ctlLocal = new Dictionary<int, double>();  // 画面に出している値
        readonly Dictionary<int, DateTime> ctlHold = new Dictionary<int, DateTime>();     // この時刻までは、届いた値で画面を動かさない
        readonly Dictionary<int, double> ctlPending = new Dictionary<int, double>();      // まだ送っていない値（スライダー）
        readonly Dictionary<int, DateTime> ctlLastSent = new Dictionary<int, DateTime>();
        public readonly Dictionary<int, CtlRow> ctlRows = new Dictionary<int, CtlRow>();
        public readonly List<CtlGroup> ctlGroups = new List<CtlGroup>();
        public int ctlThermal = -1;                      // 0 正常・1 やや高い・2 高い・3 危険。-1 はまだ分からない
        public int ctlFpsLowered;                        // 熱で下げたフレームレート。0 は下げていない
        int camFpsMax;                                   // この送信で見た、いちばん大きいフレームレート（戻したかを見分ける）
        string ctlError = "";
        DateTime ctlErrorAt = DateTime.MinValue, ctlAskedAt = DateTime.MinValue, ctlIphoneLostAt = DateTime.MinValue;
        bool ctlApplying;
        int ctlBuildNo;
        DispatcherTimer ctlTimer, blinkTimer;
        bool blinkOff;
        public int ctlSetsSent;                          // 送った set の数（試験の記録用）
        public string ctlNoteShown = "";                 // 画面に出している案内（試験の記録用）

        const int CtlSendGapMs = 100;  // スライダーを送る間隔
        const int CtlHoldMs = 700;     // 送ってから、届いた値で画面を動かさない長さ
        const int CtlGraceSec = 5;     // 切れてから、前の操作を（押せないまま）出しておく長さ
        // 試験の対照用（試す人は使わない）：PGPU_TEST_CTL_NOHOLD=1 は触っている間も届いた値を出す・PGPU_TEST_CTL_NOTHROTTLE=1 はスライダーを動くたびに送る
        static readonly bool TestNoHold = Environment.GetEnvironmentVariable("PGPU_TEST_CTL_NOHOLD") == "1";
        static readonly bool TestNoThrottle = Environment.GetEnvironmentVariable("PGPU_TEST_CTL_NOTHROTTLE") == "1";

        static readonly string[] GroupKeys = { "orient", "zoom", "focus", "exposure", "color", "light", "stab", "effects", "heat", "screen" };
        static readonly string[] GroupIcons = { "i_rotate", "i_zoom", "i_focus", "i_sun", "i_drop", "i_bulb", "i_stab", "i_person", "i_thermo", "i_phone" };
        static readonly int[][] GroupIds =
        {
            new[] { 17, 18, 19 },
            new[] { 10, 20, 21, 22, 23, 24, 25 },
            new[] { 1, 2, 3 },
            new[] { 4, 6, 7, 5 },
            new[] { 8, 9 },
            new[] { 11, 12, 26, 27 },
            new[] { 13 },
            new[] { 14, 15, 103, 105, 16 },   // 103・105 は読むだけ（ポートレート・スタジオ照明）
            new[] { 28 },
            new[] { 29 },
        };
        static readonly HashSet<int> IndentIds = new HashSet<int> { 2, 3, 5, 6, 7, 9, 12, 15, 21, 22, 23, 24 };

        void InitCamCtl()
        {
            Ctl = new CamCtl(ui);
            Ctl.Message += OnCtlMessage;
            Ctl.ConnectedChanged += on =>
            {
                if (on) ctlAskedAt = DateTime.Now;          // つないだ直後は webcam_rx が送ってくるのを待つ
                else { ctlPending.Clear(); ctlThermal = -1; ctlFpsLowered = 0; } // 温度とフレームレートの知らせは古くなる。つなぎ直すと state で届く
                RefreshCtlNow();
            };
            ctlTimer = new DispatcherTimer(DispatcherPriority.Normal, ui) { Interval = TimeSpan.FromMilliseconds(25) };
            ctlTimer.Tick += (o, e) => CtlPump();
            blinkTimer = new DispatcherTimer(DispatcherPriority.Background, ui) { Interval = TimeSpan.FromMilliseconds(600) };
            blinkTimer.Tick += (o, e) => { blinkOff = !blinkOff; PaintThermal(); };
            Get<Button>("CamDimBtn").Click += (o, e) => CtlUserSet(29, 1, true);
        }

        void CtlStart() { Ctl.Start(); }
        void CtlStop()
        {
            Ctl.Stop();
            CtlClear();
        }
        void CtlClear()
        {
            ctlIphone = ctlCtrl = null;
            ctlCaps = null;
            ctlValues.Clear();
            ctlLocal.Clear();
            ctlHold.Clear();
            ctlPending.Clear();
            ctlThermal = -1;
            ctlFpsLowered = 0;
            camFpsMax = 0;
            ctlError = "";
            CtlBuild(false);
        }

        // ---- 届いた物
        void OnCtlMessage(Dictionary<string, object> m)
        {
            string type = CamCtl.Str(m, "type");
            object o;
            switch (type)
            {
                case "status":
                    {
                        bool? c = CamCtl.Bool(m, "connected"), k = CamCtl.Bool(m, "ctrl");
                        if (c == false && ctlIphone != false) ctlIphoneLostAt = DateTime.Now;
                        if (c == true && ctlIphone != true) ctlAskedAt = DateTime.Now;
                        if (c == false) { ctlThermal = -1; ctlFpsLowered = 0; } // 温度とフレームレートの知らせは古くなる
                        ctlIphone = c;
                        ctlCtrl = k;
                        Program.Log("camctl: status connected=" + c + " ctrl=" + k);
                        break;
                    }
                case "caps":
                    {
                        var items = new List<CtlItem>();
                        if (m.TryGetValue("items", out o) && o is object[])
                            foreach (object x in (object[]) o)
                            {
                                var d = x as Dictionary<string, object>;
                                if (d == null) continue;
                                var it = CtlItem.Parse(d);
                                if (it.Id >= 0 && items.All(y => y.Id != it.Id)) items.Add(it);
                            }
                        // "iphone":false は iPhone から届いた一覧ではない（まだ接続していない）。出さない
                        ctlCaps = CamCtl.Bool(m, "iphone") == false ? null : items;
                        Program.Log("camctl: caps " + (ctlCaps == null ? "none" : string.Join(",", ctlCaps.Select(y => y.Id.ToString()).ToArray())));
                        CtlBuild(false);
                        break;
                    }
                case "state":
                    if (m.TryGetValue("values", out o) && o is Dictionary<string, object>)
                        foreach (var kv in (Dictionary<string, object>) o)
                        {
                            int id;
                            double v;
                            if (!int.TryParse(kv.Key, NumberStyles.Integer, CultureInfo.InvariantCulture, out id) || !CamCtl.TryNum(kv.Value, out v)) continue;
                            ctlValues[id] = v;
                            if (id == 102) ctlThermal = Math.Max(0, Math.Min(3, (int) Math.Round(v)));
                            // 熱で下げたあと戻したときは fps_lowered が来ず、送っているコマ数（108）が上がる
                            if (id == 108 && ctlFpsLowered > 0 && v > ctlFpsLowered + 0.5) ctlFpsLowered = 0;
                        }
                    foreach (int id in ctlRows.Keys.ToList()) CtlApply(id);
                    break;
                case "event":
                    {
                        string kind = CamCtl.Str(m, "kind");
                        if (kind == "thermal")
                        {
                            int lv = (int) CamCtl.Num(m, "level", -1);
                            ctlThermal = lv < 0 ? -1 : Math.Min(3, lv);
                        }
                        else if (kind == "fps_lowered")
                        {
                            // 下げたときも戻したときも来る。いつものフレームレートより低ければ「下げている」
                            int fps = (int) CamCtl.Num(m, "fps", 0);
                            int base0 = Math.Max(S.Int("cam.fps", 30) == 60 ? 60 : 30, camFpsMax);
                            ctlFpsLowered = fps > 0 && fps < base0 ? fps : 0;
                        }
                        Program.Log("camctl: event " + kind + " level=" + ctlThermal + " lowered=" + ctlFpsLowered);
                        break;
                    }
                case "error":
                    ctlError = CamCtl.Str(m, "message");
                    ctlErrorAt = DateTime.Now;
                    Program.Log("camctl: error " + ctlError);
                    break;
            }
            RefreshCtlNow();
        }

        // 届いた値を画面に出す（触っている間は出さない）
        void CtlApply(int id)
        {
            CtlRow r;
            double v;
            if (!ctlRows.TryGetValue(id, out r) || r.ReadOnly) return;
            if (!TestNoHold && (ctlHold.ContainsKey(id) || ctlPending.ContainsKey(id) || r.Dragging)) return;
            if (!ctlValues.TryGetValue(id, out v)) return;
            ctlLocal[id] = v;
            CtlShow(r, v);
        }

        // ---- 利用者が変えた
        void CtlUserSet(int id, double v, bool now)
        {
            ctlLocal[id] = v;
            if (now)
            {
                ctlPending.Remove(id);
                CtlSendNow(id, v);
            }
            else
            {
                ctlPending[id] = v;
                ctlHold[id] = DateTime.Now.AddMilliseconds(CtlHoldMs);
                CtlPump();
            }
            CtlRefreshRows();
        }

        void CtlSendNow(int id, double v)
        {
            if (Ctl.SendSet(id, v)) ctlSetsSent++;
            ctlLastSent[id] = DateTime.Now;
            ctlHold[id] = DateTime.Now.AddMilliseconds(CtlHoldMs);
            if (!ctlTimer.IsEnabled) ctlTimer.Start();
        }

        // 25 ms ごと：まとめた値を送り、保留が切れた値を届いた値に合わせる
        void CtlPump()
        {
            var now = DateTime.Now;
            foreach (int id in ctlPending.Keys.ToList())
            {
                DateTime last;
                if (TestNoThrottle || !ctlLastSent.TryGetValue(id, out last) || (now - last).TotalMilliseconds >= CtlSendGapMs - 4)
                {
                    double v = ctlPending[id];
                    ctlPending.Remove(id);
                    CtlSendNow(id, v);
                }
            }
            foreach (int id in ctlHold.Keys.ToList())
            {
                CtlRow r;
                bool drag = ctlRows.TryGetValue(id, out r) && (r.Dragging || (r.Slider != null && r.Slider.IsMouseCaptureWithin));
                if (drag) { ctlHold[id] = now.AddMilliseconds(CtlHoldMs); continue; }
                if (ctlPending.ContainsKey(id) || now < ctlHold[id]) continue;
                ctlHold.Remove(id);
                CtlApply(id);
                CtlRefreshRows();
            }
            if (ctlPending.Count == 0 && ctlHold.Count == 0) ctlTimer.Stop();
            else if (!ctlTimer.IsEnabled) ctlTimer.Start();
        }

        // ---- 値の読み方
        bool CtlHas(int id) { return ctlRows.ContainsKey(id) && !ctlRows[id].ReadOnly; }
        double CtlV(int id, double def)
        {
            double v;
            if (ctlLocal.TryGetValue(id, out v)) return v;
            if (ctlValues.TryGetValue(id, out v)) return v;
            return def;
        }
        bool CtlIs(int id, double want) { return Math.Abs(CtlV(id, -9999) - want) < 0.001; }

        // ---- 行を作る（caps が変わったときだけ）
        void CtlBuild(bool force)
        {
            string sig = ctlCaps == null ? null : string.Join("\n", ctlCaps.Select(x => x.Sig()).ToArray()) + (L.Ja ? "|ja" : "|en");
            if (!force && sig == ctlCapsSig) return;
            ctlCapsSig = sig;
            ctlBuildNo++;
            var host = Get<StackPanel>("CamCtlHost");
            host.Children.Clear();
            ctlRows.Clear();
            ctlGroups.Clear();
            if (ctlCaps == null) return;
            var byId = new Dictionary<int, CtlItem>();
            foreach (var it in ctlCaps) byId[it.Id] = it;
            var used = new HashSet<int>();
            for (int g = 0; g < GroupKeys.Length; g++)
            {
                var grp = new CtlGroup { Key = GroupKeys[g], Icon = GroupIcons[g], Ids = GroupIds[g] };
                foreach (int id in grp.Ids)
                {
                    CtlItem it;
                    if (id == 103 || id == 105) grp.List.Add(MakeReadOnlyRow(id));
                    else if (byId.TryGetValue(id, out it)) { grp.List.Add(MakeRow(it, grp.Icon)); used.Add(id); }
                }
                if (grp.List.Any(r => !r.ReadOnly) || grp.Key == "effects") AddGroup(host, grp);
            }
            // 表に無い番号（新しい操作）は「その他」に、届いた英語の名前で出す
            var other = new CtlGroup { Key = "other", Icon = "i_settings", Ids = new int[0] };
            foreach (var it in ctlCaps) if (!used.Contains(it.Id) && it.Id != 103 && it.Id != 105) other.List.Add(MakeRow(it, other.Icon));
            if (other.List.Count > 0) AddGroup(host, other);
            foreach (int id in ctlRows.Keys.ToList()) CtlApply(id);
            CtlRefreshRows();
        }

        void AddGroup(StackPanel host, CtlGroup grp)
        {
            grp.Box = new StackPanel();
            grp.Box.Children.Add(Text(L.T("ctl_g_" + grp.Key), "Sec"));
            var card = new Border { Style = St("Card") };
            grp.Rows = new StackPanel();
            card.Child = grp.Rows;
            grp.Box.Children.Add(card);
            foreach (var r in grp.List)
            {
                grp.Rows.Children.Add(r.Row);
                ctlRows[r.Item.Id] = r;
            }
            host.Children.Add(grp.Box);
            ctlGroups.Add(grp);
        }

        string CtlTitle(CtlItem it)
        {
            string k = "ctl_" + it.Id;
            string t = L.T(k);
            return t == k ? (it.Name.Length > 0 ? it.Name : "#" + it.Id) : t;
        }

        CtlRow MakeRow(CtlItem it, string icon)
        {
            var r = new CtlRow { Item = it, Indent = IndentIds.Contains(it.Id) };
            r.Row = new Border { Style = St("RowNext") };
            if (r.Indent) r.Row.Padding = new Thickness(56, 9, 20, 9);
            var dp = new DockPanel();
            if (!r.Indent) dp.Children.Add(Icon(icon, "RowIco"));
            var right = MakeInput(r);
            DockPanel.SetDock(right, Dock.Right);
            dp.Children.Add(right);
            var mid = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            r.Title = Text(CtlTitle(it), "RowTitle");
            mid.Children.Add(r.Title);
            r.Sub = Text("", "RowSub");
            r.Sub.Visibility = Visibility.Collapsed;
            mid.Children.Add(r.Sub);
            dp.Children.Add(mid);
            r.Row.Child = dp;
            r.Input = right;
            return r;
        }

        CtlRow MakeReadOnlyRow(int id)
        {
            var r = new CtlRow { Item = new CtlItem { Id = id, Kind = "ro" }, ReadOnly = true };
            r.Row = new Border { Style = St("RowNext"), Padding = new Thickness(56, 9, 20, 9) };
            var dp = new DockPanel();
            r.ValueText = Text("", "Val");
            dp.Children.Add(r.ValueText);
            var mid = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            r.Title = Text(L.T("ctl_" + id), "RowTitle");
            mid.Children.Add(r.Title);
            r.Sub = Text("", "RowSub");
            r.Sub.Visibility = Visibility.Collapsed;
            mid.Children.Add(r.Sub);
            dp.Children.Add(mid);
            r.Row.Child = dp;
            r.Input = r.ValueText;
            return r;
        }

        FrameworkElement MakeInput(CtlRow r)
        {
            var it = r.Item;
            int id = it.Id;
            switch (it.Kind)
            {
                case "toggle":
                    {
                        var cb = new CheckBox { Style = St("Switch") };
                        cb.Click += (o, e) => CtlUserSet(id, cb.IsChecked == true ? 1 : 0, true);
                        r.Switch = cb;
                        return cb;
                    }
                case "button":
                    {
                        string k = "ctl_btn_" + id, t = L.T(k);
                        var b = new Button { Style = St("Btn"), Content = t == k ? L.T("ctl_btn_run") : t, Margin = new Thickness(16, 0, 0, 0) };
                        b.Click += (o, e) => CtlUserSet(id, 1, true);
                        r.Button = b;
                        return b;
                    }
                case "choice":
                    {
                        // 少なくて短い選択肢は iPhone の切り替え（segmented）と同じ組、ほかは一覧から選ぶ
                        bool seg = it.Choices.Count > 0 && it.Choices.Count <= 4 && it.Choices.All(c => c.Label.Length <= (L.Ja ? 5 : 9));
                        if (seg)
                        {
                            var box = new Border { Style = St("SegBox") };
                            var sp = new StackPanel { Orientation = Orientation.Horizontal };
                            r.Seg = new List<RadioButton>();
                            string grp = "ctl" + ctlBuildNo + "_" + id;
                            foreach (var c in it.Choices)
                            {
                                double v = c.Value;
                                var rb = new RadioButton { Style = St("SegItem"), GroupName = grp, Content = c.Label, Tag = v };
                                rb.Checked += (o, e) => { if (!ctlApplying) CtlUserSet(id, v, true); };
                                r.Seg.Add(rb);
                                sp.Children.Add(rb);
                            }
                            box.Child = sp;
                            return box;
                        }
                        var cbx = new ComboBox { Style = St("Select") };
                        foreach (var c in it.Choices) cbx.Items.Add(new ComboBoxItem { Tag = c.Value, Content = c.Label });
                        cbx.SelectionChanged += (o, e) =>
                        {
                            if (ctlApplying) return;
                            var sel = cbx.SelectedItem as ComboBoxItem;
                            if (sel != null) CtlUserSet(id, (double) sel.Tag, true);
                        };
                        r.Combo = cbx;
                        return cbx;
                    }
                default:
                    {
                        // range（知らない kind も数として出す）
                        var sp = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(16, 0, 0, 0) };
                        var s = new Slider { Style = St("Slider") };
                        if (it.Log && it.Min > 0 && it.Max > it.Min)
                        {
                            s.Minimum = 0; s.Maximum = 1;
                            s.IsSnapToTickEnabled = false;
                            s.SmallChange = 0.01; s.LargeChange = 0.1;
                        }
                        else
                        {
                            s.Minimum = it.Min;
                            s.Maximum = Math.Max(it.Max, it.Min + 1e-6);
                            if (it.Step > 0) { s.TickFrequency = it.Step; s.IsSnapToTickEnabled = true; s.SmallChange = it.Step; }
                            else { s.IsSnapToTickEnabled = false; s.SmallChange = (s.Maximum - s.Minimum) / 100; }
                            s.LargeChange = (s.Maximum - s.Minimum) / 10;
                        }
                        if (id == 2)
                        {
                            sp.Children.Add(new TextBlock { Text = L.T("ctl_near"), Style = St("Foot"), Margin = new Thickness(0, 0, 10, 0) });
                            sp.Children.Add(s);
                            sp.Children.Add(new TextBlock { Text = L.T("ctl_far"), Style = St("Foot"), Margin = new Thickness(10, 0, 0, 0) });
                        }
                        else
                        {
                            if (it.Stops.Count > 0)
                            {
                                r.Stops = new List<Button>();
                                foreach (double z in it.Stops)
                                {
                                    double zz = z;
                                    var b = new Button { Style = St("Btn"), Content = ZoomLabel(z), Height = 28, Padding = new Thickness(9, 0, 9, 0), Margin = new Thickness(0, 0, 6, 0), Tag = z };
                                    b.Click += (o, e) => CtlUserSet(id, zz, true);
                                    r.Stops.Add(b);
                                    sp.Children.Add(b);
                                }
                            }
                            r.ValueText = new TextBlock { MinWidth = 52, TextAlignment = TextAlignment.Right, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(6, 0, 12, 0) };
                            r.ValueText.SetResourceReference(TextBlock.ForegroundProperty, "Sub");
                            sp.Children.Add(r.ValueText);
                            sp.Children.Add(s);
                        }
                        s.ValueChanged += (o, e) =>
                        {
                            if (ctlApplying) return;
                            double v = CtlFromSlider(it, s.Value);
                            if (r.ValueText != null) r.ValueText.Text = CtlFormat(it, v);
                            CtlUserSet(id, v, false);
                        };
                        s.AddHandler(Thumb.DragStartedEvent, new DragStartedEventHandler((o, e) => { r.Dragging = true; }));
                        s.AddHandler(Thumb.DragCompletedEvent, new DragCompletedEventHandler((o, e) =>
                        {
                            r.Dragging = false;
                            ctlHold[id] = DateTime.Now.AddMilliseconds(CtlHoldMs);
                            CtlPump();
                        }));
                        r.Slider = s;
                        return sp;
                    }
            }
        }

        // スライダーの位置と値。ズームと ISO は対数（1 倍から 2 倍と、5 倍から 10 倍が同じ長さ。iPhone と同じ）
        public static double CtlFromSlider(CtlItem it, double pos)
        {
            double v;
            if (it.Log && it.Min > 0 && it.Max > it.Min) v = it.Min * Math.Pow(it.Max / it.Min, Math.Max(0, Math.Min(1, pos)));
            else v = pos;
            if (it.Step > 0) v = it.Min + Math.Round((v - it.Min) / it.Step) * it.Step;
            v = Math.Max(it.Min, Math.Min(it.Max, v));
            return Math.Round(v, 6);
        }
        public static double CtlToSlider(CtlItem it, double v)
        {
            if (it.Log && it.Min > 0 && it.Max > it.Min)
            {
                if (v <= 0) v = Math.Sqrt(it.Min * it.Max); // ISO の 0 は「今の値」。真ん中に置く（iPhone と同じ）
                return Math.Max(0, Math.Min(1, Math.Log(Math.Max(v, it.Min) / it.Min) / Math.Log(it.Max / it.Min)));
            }
            return Math.Max(it.Min, Math.Min(it.Max, v));
        }

        static string ZoomLabel(double z)
        {
            double r = Math.Round(z * 10) / 10;
            return (Math.Abs(r - Math.Round(r)) < 0.001 ? r.ToString("0", CultureInfo.InvariantCulture) : r.ToString("0.0", CultureInfo.InvariantCulture)) + "×";
        }

        static string CtlFormat(CtlItem it, double v)
        {
            var inv = CultureInfo.InvariantCulture;
            switch (it.Id)
            {
                case 2: case 23: case 24: return "";
                case 5: return v.ToString("+0.0;-0.0;0.0", inv);
                case 6: return v <= 0 ? L.T("ctl_current") : Math.Round(v).ToString("0", inv);
                case 9: return Math.Round(v).ToString("0", inv) + " K";
                case 10: return ZoomLabel(v);
                case 12: case 22: return Math.Round(v * 100).ToString("0", inv) + "%";
                case 15: return "f/" + v.ToString("0.0", inv);
            }
            string s = it.Step >= 1 ? Math.Round(v).ToString("0", inv) : v.ToString("0.##", inv);
            return it.Unit.Length > 0 ? s + " " + it.Unit : s;
        }

        // 値を部品に出す（送らない）
        void CtlShow(CtlRow r, double v)
        {
            ctlApplying = true;
            try
            {
                if (r.Switch != null) r.Switch.IsChecked = v != 0;
                else if (r.Slider != null)
                {
                    r.Slider.Value = CtlToSlider(r.Item, v);
                    if (r.ValueText != null) r.ValueText.Text = CtlFormat(r.Item, v);
                }
                else if (r.Seg != null)
                {
                    // 選べない値なら、初めの選択肢として見せる（iPhone の pick と同じ）
                    RadioButton hit = r.Seg.FirstOrDefault(b => Math.Abs((double) b.Tag - v) < 0.001) ?? r.Seg.FirstOrDefault();
                    foreach (var b in r.Seg) b.IsChecked = b == hit;
                }
                else if (r.Combo != null)
                {
                    ComboBoxItem hit = null;
                    foreach (ComboBoxItem c in r.Combo.Items) if (Math.Abs((double) c.Tag - v) < 0.001) hit = c;
                    if (hit == null && r.Combo.Items.Count > 0) hit = (ComboBoxItem) r.Combo.Items[0];
                    r.Combo.SelectedItem = hit;
                }
            }
            finally { ctlApplying = false; }
        }

        // ---- 見える行・説明の文（値が変わるたびと、0.5 秒ごと）
        bool CtlRowVisible(int id)
        {
            bool blur = CtlHas(14) && CtlIs(14, 1);
            switch (id)
            {
                case 2: return (!CtlHas(1) || CtlIs(1, 2)) && !blur;
                case 3: return (!CtlHas(1) || CtlIs(1, 0)) && !blur;
                case 6: case 7: return !CtlHas(4) || CtlIs(4, 2);
                case 5: return !CtlHas(4) || !CtlIs(4, 2);
                case 9: return !CtlHas(8) || CtlIs(8, 2);
                case 12: return !CtlHas(11) || CtlIs(11, 1);
                case 21: case 22: return !CtlHas(20) || CtlIs(20, 1);
                case 23: case 24: return (!CtlHas(20) || CtlIs(20, 1)) && (!CtlHas(21) || CtlIs(21, 4));
                case 15: return !CtlHas(14) || CtlIs(14, 1);
                case 103: return ctlValues.ContainsKey(103);
                case 105: return ctlValues.ContainsKey(105);
            }
            return true;
        }

        string CtlSub(int id)
        {
            var inv = CultureInfo.GetCultureInfo(L.Ja ? "ja-JP" : "en-US");
            bool blur = CtlHas(14) && CtlIs(14, 1);
            double v;
            switch (id)
            {
                case 19: return CtlIs(19, 90) || CtlIs(19, 270) ? L.T("ctl_rot_note") : "";
                case 20: return L.T("ctl_crop_note");
                case 21: return CtlIs(21, 3) ? L.T("ctl_face_note") : "";
                case 25:
                    if (!CtlIs(25, 1)) return "";
                    return L.T("ctl_level_note") + (ctlValues.TryGetValue(107, out v) ? "\n" + L.F("ctl_level_now", v.ToString("0.0", CultureInfo.InvariantCulture)) : "");
                case 1: return blur ? L.T("ctl_focus_blur") : CtlIs(1, 1) ? L.T("ctl_focus_lock") : "";
                case 4: return CtlIs(4, 1) ? L.T("ctl_exp_lock") : "";
                case 7: return L.T("ctl_flicker");
                case 8: return CtlIs(8, 1) ? L.T("ctl_wb_lock") : "";
                case 9: return L.T("ctl_wb_note");
                case 11:
                    {
                        if (CtlIs(11, 1) && ctlValues.ContainsKey(100))
                        {
                            if (ctlValues[100] < 0.5) return L.T("ctl_torch_off");
                            double now = ctlValues.ContainsKey(101) ? ctlValues[101] : CtlV(12, 1);
                            double want = Math.Max(0.05, Math.Min(1, CtlV(12, 1)));
                            int pct = (int) Math.Round(now * 100);
                            return now + 0.05 < want ? L.F("ctl_torch_low", pct) : L.F("ctl_torch_on", pct);
                        }
                        return L.T("ctl_torch_note");
                    }
                case 26: return L.T("ctl_screen_light");
                case 13: return L.T("ctl_stab_" + Math.Max(0, Math.Min(3, (int) Math.Round(CtlV(13, 0)))));
                case 14:
                    return L.T("ctl_fx_note") + (CtlIs(14, 1) && ctlValues.TryGetValue(106, out v) && v < 0.5 ? "\n" + L.T("ctl_cine_na") : "");
                case 15: return L.T("ctl_cine_note");
                case 105: return L.T("ctl_fx_cc");
                case 103: return ctlValues.ContainsKey(105) ? "" : L.T("ctl_fx_cc");
                case 16: return L.T("ctl_stage_note");
                case 28: return L.T("ctl_heat_note");
                case 29: return L.T("ctl_dim_note");
            }
            return "";
        }

        void CtlRefreshRows()
        {
            bool blur = CtlHas(14) && CtlIs(14, 1);
            foreach (var g in ctlGroups)
            {
                bool first = true, any = false;
                foreach (var r in g.List)
                {
                    int id = r.Item.Id;
                    bool vis = CtlRowVisible(id);
                    r.Row.Visibility = vis ? Visibility.Visible : Visibility.Collapsed;
                    if (!vis) continue;
                    any = true;
                    var want = St(first ? "Row" : "RowNext");
                    if (r.Row.Style != want) r.Row.Style = want;
                    first = false;
                    string sub = CtlSub(id);
                    r.Sub.Text = sub;
                    r.Sub.Visibility = sub.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
                    if (id == 1 && r.Input != null) r.Input.IsEnabled = !blur;
                    if (r.ReadOnly)
                    {
                        bool on = CtlIs(id, 1), active = id == 103 && CtlIs(104, 1);
                        r.ValueText.Text = on ? (active ? L.T("ctl_in_use") : L.T("on")) : L.T("off");
                    }
                    if (r.Stops != null)
                    {
                        double z = CtlV(id, 1);
                        foreach (var b in r.Stops)
                        {
                            var st = St(Math.Abs((double) b.Tag - z) < 0.05 ? "BtnPrimary" : "Btn");
                            if (b.Style != st) b.Style = st;
                        }
                    }
                }
                g.Box.Visibility = any ? Visibility.Visible : Visibility.Collapsed;
            }
        }

        // ---- 案内・温度（0.5 秒ごとと、届いたとき）
        void RefreshCtlNow()
        {
            if (Current == "Camera") RefreshCamCtl();
            if (Current == "Home") RefreshHomeStats();
            PaintThermal();
        }

        string CtlNote(out bool showControls, out bool usable)
        {
            showControls = false;
            usable = false;
            var now = DateTime.Now;
            if (!Cam.Want) return L.T("ctl_note_off");
            bool fresh = ctlCaps != null && ctlCtrl != false;
            if (!Ctl.Connected)
            {
                // 切れてすぐ（webcam_rx の起動し直しなど）は、前の操作を押せないまま出しておく
                if (fresh && Ctl.Connects > 0 && (now - Ctl.LostAt).TotalSeconds < CtlGraceSec) { showControls = true; return ""; }
                if (Ctl.Connects == 0 && CamFlowing && (now - Ctl.WantSince).TotalSeconds > 10) return L.T("ctl_note_nopipe");
                return CamFlowing ? L.T("ctl_note_connecting") : L.T("ctl_note_wait_iphone");
            }
            if (ctlIphone == false)
            {
                if (fresh && (now - ctlIphoneLostAt).TotalSeconds < CtlGraceSec) { showControls = true; return ""; }
                return L.T("ctl_note_wait_iphone");
            }
            if (ctlCtrl == false) return L.T("ctl_note_old_app");
            if (ctlCaps == null) return ctlIphone == true ? L.T("ctl_note_loading") : L.T("ctl_note_connecting");
            showControls = true;
            usable = true;
            return "";
        }

        void RefreshCamCtl()
        {
            var now = DateTime.Now;
            // 切れたまま 5 秒たったら、前の操作を片づける
            if (ctlCaps != null && ((!Ctl.Connected && Ctl.Wanted && (now - Ctl.LostAt).TotalSeconds >= CtlGraceSec && Ctl.LostAt > Ctl.ConnectedAt) ||
                                    (ctlIphone == false && (now - ctlIphoneLostAt).TotalSeconds >= CtlGraceSec)))
            {
                ctlCaps = null;
                ctlValues.Clear();
                ctlLocal.Clear();
                CtlBuild(false);
            }
            // 届かないときは頼み直す（つないだ直後は webcam_rx が送ってくるので 2 秒待つ）
            if (Ctl.Connected && ctlCaps == null && ctlIphone != false && ctlCtrl != false && (now - ctlAskedAt).TotalSeconds > 2)
            {
                Ctl.Send("{\"cmd\":\"get_caps\"}");
                Ctl.Send("{\"cmd\":\"get_state\"}");
                ctlAskedAt = now;
                Program.Log("camctl: asked caps and state");
            }

            bool show, usable;
            string note = CtlNote(out show, out usable);
            bool err = ctlError.Length > 0 && (now - ctlErrorAt).TotalSeconds < 10;
            ctlNoteShown = note + (err ? " | " + ctlError : "");
            string errText = err ? L.F("ctl_error", ctlError) : "";
            Get<FrameworkElement>("CamCtlNoteBox").Visibility = note.Length > 0 || err ? Visibility.Visible : Visibility.Collapsed;
            Get<TextBlock>("CamCtlNote").Text = note.Length > 0 ? note : errText;
            var et = Get<TextBlock>("CamCtlErr");
            et.Text = note.Length > 0 ? errText : "";
            et.Visibility = et.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
            var host = Get<StackPanel>("CamCtlHost");
            host.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
            host.IsEnabled = usable;
            if (show) CtlRefreshRows();

            // 熱の知らせ：熱でフレームレートを下げている・危険
            bool crit = ctlThermal == 3;
            var card = Get<FrameworkElement>("CamHeatCard");
            if (Cam.Want && (ctlFpsLowered > 0 || crit))
            {
                card.Visibility = Visibility.Visible;
                Get<TextBlock>("CamHeatTitle").Text = ctlFpsLowered > 0 ? L.F("ctl_heat_fps", ctlFpsLowered) : L.T("ctl_heat_critical");
                var hs = Get<TextBlock>("CamHeatSub");
                hs.Text = crit ? L.T("ctl_heat_dim_hint") : "";
                hs.Visibility = crit ? Visibility.Visible : Visibility.Collapsed;
                var db = Get<Button>("CamDimBtn");
                db.Visibility = crit && usable && CtlHas(29) ? Visibility.Visible : Visibility.Collapsed;
                db.IsEnabled = !CtlIs(29, 1);
            }
            else card.Visibility = Visibility.Collapsed;
            PaintThermal();
        }

        // ---- 温度の色と文字（カメラの画面の見出しの横・ホームの iPhone の行）
        // 色の段：0 緑・1 黄・2 赤。-1 は出さない
        int ThermalColorLevel()
        {
            if (ctlThermal >= 0) return Math.Min(2, ctlThermal);
            double? c = BattTempC;
            if (c == null) return -1;
            return c.Value < 40 ? 0 : c.Value < 45 ? 1 : 2; // 仮の境目（本物の iPhone で 4 段階と並べて決め直す）
        }
        string CelsiusText()
        {
            double? c = BattTempC;
            return c == null ? "" : L.F("stat_celsius", c.Value);
        }
        static readonly string[] SigKeys = { "SigOk", "SigWarm", "SigHot" };

        void PaintDot(Ellipse dot, int lv)
        {
            if (lv < 0) { dot.Visibility = Visibility.Collapsed; return; }
            dot.Visibility = Visibility.Visible;
            dot.SetResourceReference(Shape.FillProperty, SigKeys[lv]);
            dot.Opacity = ctlThermal == 3 && blinkOff ? 0.15 : 1;
        }

        // 温度の文字：段の言葉（危険は太字の赤）と度数
        void FillThermalText(TextBlock tb)
        {
            tb.Inlines.Clear();
            AddThermalRuns(tb.Inlines);
        }
        void AddThermalRuns(InlineCollection ic)
        {
            string c = CelsiusText();
            if (ctlThermal >= 0)
            {
                var run = new Run(L.T("therm_" + ctlThermal));
                if (ctlThermal == 3)
                {
                    run.FontWeight = FontWeights.Bold;
                    run.SetResourceReference(TextElement.ForegroundProperty, "SigHotText");
                }
                ic.Add(run);
                if (c.Length > 0) ic.Add(new Run(" " + c));
            }
            else if (c.Length > 0) ic.Add(new Run(c));
        }

        void PaintThermal()
        {
            int lv = ThermalColorLevel();
            if (ctlThermal == 3) { if (!blinkTimer.IsEnabled) blinkTimer.Start(); }
            else if (blinkTimer.IsEnabled) { blinkTimer.Stop(); blinkOff = false; }
            try
            {
                var box = Get<FrameworkElement>("CamThermal");
                box.Visibility = lv >= 0 ? Visibility.Visible : Visibility.Collapsed;
                PaintDot(Get<Ellipse>("CamThermalDot"), lv);
                if (lv >= 0) FillThermalText(Get<TextBlock>("CamThermalText"));
                PaintDot(Get<Ellipse>("HomeTempDot"), lv);
            }
            catch (Exception ex) { Program.Log("thermal: " + ex.Message); }
        }

        // 試験の記録用：見えている操作の行
        public string CtlRowsText()
        {
            var parts = new List<string>();
            foreach (var g in ctlGroups)
            {
                if (g.Box.Visibility != Visibility.Visible) continue;
                var items = new List<string>();
                foreach (var r in g.List)
                {
                    if (r.Row.Visibility != Visibility.Visible) continue;
                    string val = r.ValueText != null ? r.ValueText.Text : r.Switch != null ? (r.Switch.IsChecked == true ? "on" : "off")
                        : r.Seg != null ? string.Join("", r.Seg.Where(b => b.IsChecked == true).Select(b => (string) b.Content).ToArray())
                        : r.Combo != null && r.Combo.SelectedItem != null ? (string) ((ComboBoxItem) r.Combo.SelectedItem).Content : "";
                    items.Add(r.Item.Id + ":" + r.Title.Text + "=" + val);
                }
                parts.Add("[" + L.T("ctl_g_" + g.Key) + "] " + string.Join(", ", items.ToArray()));
            }
            return string.Join(" ", parts.ToArray());
        }
        public string CtlShownIds()
        {
            var ids = new List<int>();
            foreach (var g in ctlGroups) foreach (var r in g.List) if (!r.ReadOnly) ids.Add(r.Item.Id);
            ids.Sort();
            return string.Join(",", ids.Select(x => x.ToString()).ToArray());
        }
    }
}
