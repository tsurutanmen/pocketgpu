// 試験用：台本のとおりに画面を操作して PNG に撮る（--auto 台本.txt）。試す人は使わない。
// 1 行 1 つ。# から後ろは無視。
//   page Camera            ページを開く（Home Gpu Camera Audio Caption Record Check Settings）
//   click 名前             ボタンを押す・スイッチを切り替える・選ぶ（XAML の x:Name）
//   wait 秒                待つ（画面は止めない）
//   waitfor 名前 文字 秒    その TextBlock に文字が出るまで待つ（出なければ記録に書いて先へ）
//   waitcam 秒              カメラのプレビューに絵が出るまで待つ
//   shot ファイル.png [高さ] 中身を描いて PNG に（ウィンドウの枠なし・1280x768。高さを書くとその高さ）
//   screen ファイル.png     ウィンドウを画面から撮る（--window-at のとき。枠つき）。ウィンドウを前に出すので、今は使わない
//   winshot ファイル.png    自分のウィンドウだけを PrintWindow で撮る（枠とタイトルバーつき）。前に出さない。
//                           画面の外では中身を WPF が描かないので、中身の所は WPF に描かせた絵を重ねる（WinShot の説明）
//   set キー 値             設定を変える（true・false・数・文字）
//   lang ja|en|auto         言語を変える
//   theme auto|light|dark   テーマを変える（設定の画面で選んだのと同じ）
//   select 名前 タグ        ComboBox の項目を選ぶ（利用者が選んだのと同じ動き）
//   regtheme 0|1            試験用の場所（PGPU_THEME_KEY）の AppsUseLightTheme を書く。本物の Windows の設定には書かない
//   waittheme dark|light 秒 その色に切り替わるまで待つ
//   color 名前              今の色（ThemeLight・ThemeDark の名前）を記録に書く
//   titlebar                タイトルバーに頼んだ内容と結果を記録に書く
//   popup 名前 ファイル.png  ComboBox の開いた一覧を描いて PNG に（ウィンドウ無しでは開けないので、中身だけ描く）
//   dump 名前               TextBlock の文を記録に書く
//   tool cam|audio|cap      その道具の状態と出力の最後の行を記録に書く
//   pids                    道具の PID を記録に書く
//   env                     GPU の画面から起動するゲームに渡す環境変数と、録画の印のファイルを記録に書く
//   endpoint 名前           その出力の機器の音量とミュートを記録に書く（読むだけ）
//   mute                    PC の音を消す（PcMute）の状態を記録に書く
//   probe・waitprobe 秒     ipaudio がマイクの音を聞く引数に対応しているか。分かるまで待つ
//   args                    いまの設定で ipaudio に渡す引数を記録に書く
//   slider 名前 値          つまみを動かして離す
//   enabled 名前            部品が押せるか
//   quit                   閉じる（道具を全部止めてから）
// カメラの操作（camctl。PageCamCtl.cs）：
//   ctl                     パイプ・status・caps・温度・案内の文・送った数を記録に書く
//   ctlrows                 見えている操作の行（まとまり・番号・題・値）を記録に書く
//   waitctl caps|nocaps|pipe|old|connects N|thermal N 秒   その状態になるまで待つ
//   ctlclick 番号           スイッチを切り替える・ボタンを押す
//   ctlselect 番号 値       選択肢を選ぶ（利用者が選んだのと同じ）
//   ctlslider 番号 値       つまみを動かして離す
//   ctldrag 番号 から まで ミリ秒   つまみをつかんで、から→まで動かして離す（15 ms ごと）。送った数と、つかんでいる間に値が跳ねた回数を記録に書く
//   ctlvalue 番号           画面の値と、届いた値を記録に書く
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace PocketGPU
{
    class Auto
    {
        readonly Shell sh;
        readonly Window win;
        readonly Application app;
        readonly List<string> lines = new List<string>();
        int pc;
        DispatcherTimer timer;
        DateTime until = DateTime.MinValue;
        Func<bool> waitCond;

        public Auto(Shell sh, Window win, Application app, string path)
        {
            this.sh = sh;
            this.win = win;
            this.app = app;
            foreach (string raw in File.ReadAllLines(path, Encoding.UTF8))
            {
                string l = raw;
                int h = l.IndexOf('#');
                if (h >= 0) l = l.Substring(0, h);
                l = l.Trim();
                if (l.Length > 0) lines.Add(l);
            }
        }

        void Log(string s) { Program.Log("[auto] " + s); }

        public void Start()
        {
            timer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(100) };
            timer.Tick += (o, e) => Step();
            timer.Start();
        }

        void Step()
        {
            if (DateTime.Now < until && (waitCond == null || !waitCond())) return;
            if (waitCond != null && DateTime.Now >= until && !waitCond()) Log("  timeout");
            waitCond = null;
            until = DateTime.MinValue;
            while (pc < lines.Count && until == DateTime.MinValue)
            {
                string l = lines[pc++];
                Log(l);
                try { Do(l); }
                catch (Exception ex) { Log("  error " + ex.Message); }
            }
            if (pc >= lines.Count && until == DateTime.MinValue) { timer.Stop(); Quit(); }
        }

        void Do(string l)
        {
            string[] a = l.Split(new[] { ' ' }, 2);
            string cmd = a[0], rest = a.Length > 1 ? a[1] : "";
            switch (cmd)
            {
                case "page": sh.Show(rest); break;
                case "click": Click(rest); break;
                case "wait": until = DateTime.Now.AddSeconds(double.Parse(rest, CultureInfo.InvariantCulture)); break;
                case "waitfor":
                    {
                        string[] p = rest.Split(' ');
                        string name = p[0], text = string.Join(" ", p, 1, p.Length - 2);
                        double secs = double.Parse(p[p.Length - 1], CultureInfo.InvariantCulture);
                        waitCond = () => { sh.Refresh(); return sh.Get<TextBlock>(name).Text.Contains(text); };
                        until = DateTime.Now.AddSeconds(secs);
                        break;
                    }
                case "waitcam":
                    waitCond = () => sh.Get<Image>("CamImage").Source != null;
                    until = DateTime.Now.AddSeconds(double.Parse(rest, CultureInfo.InvariantCulture));
                    break;
                case "shot":
                    {
                        // shot ファイル.png [高さ|fit]：高さを書くと、その高さで並べ直して撮る（長い画面を下まで 1 枚に）。fit はページの中身の高さ
                        string[] p = rest.Split(' ');
                        Shot(p[0], p.Length > 1 ? (p[1] == "fit" ? FitHeight() : int.Parse(p[1])) : 768);
                        break;
                    }
                case "screen": Screen(rest); break;
                case "set":
                    {
                        string[] p = rest.Split(new[] { ' ' }, 2);
                        object v = p.Length < 2 ? "" : p[1] == "true" ? (object) true : p[1] == "false" ? (object) false : p[1];
                        int n;
                        if (v is string && int.TryParse((string) v, out n)) v = n;
                        sh.S.Set(p[0], v);
                        sh.ApplyMode();
                        sh.Refresh();
                        break;
                    }
                case "lang": sh.SetLanguage(rest); break;
                case "theme": sh.SetTheme(rest); break;
                case "select":
                    {
                        string[] p = rest.Split(new[] { ' ' }, 2);
                        Shell.SelectByTag(sh.Get<ComboBox>(p[0]), p.Length > 1 ? p[1] : "");
                        Log("  selected " + Shell.SelectedTag(sh.Get<ComboBox>(p[0])));
                        break;
                    }
                case "regtheme":
                    {
                        if (!Theme.IsTestKey) { Log("  refused: PGPU_THEME_KEY is not set (the real Windows setting is never written)"); break; }
                        using (var k = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(Theme.KeyPath))
                            k.SetValue(Theme.ValueName, int.Parse(rest), Microsoft.Win32.RegistryValueKind.DWord);
                        Log("  wrote HKCU\\" + Theme.KeyPath + " " + Theme.ValueName + "=" + rest);
                        break;
                    }
                case "waittheme":
                    {
                        string[] p = rest.Split(' ');
                        bool want = p[0] == "dark";
                        DateTime t0 = DateTime.Now;
                        waitCond = () => { if (sh.Dark == want) { Log("  switched after " + (int) (DateTime.Now - t0).TotalMilliseconds + " ms"); return true; } return false; };
                        until = DateTime.Now.AddSeconds(double.Parse(p[1], CultureInfo.InvariantCulture));
                        break;
                    }
                case "color":
                    {
                        var b = sh.Paint(rest) as SolidColorBrush;
                        Log("  " + rest + " = " + (b != null ? b.Color.ToString() : "(none)") + " dark=" + sh.Dark);
                        break;
                    }
                case "titlebar": Log("  " + Theme.LastTitleBar); break;
                case "popup":
                    {
                        string[] p = rest.Split(new[] { ' ' }, 2);
                        Popup(p[0], p[1]);
                        break;
                    }
                case "winshot":
                    {
                        // winshot ファイル.png [PrintWindow の flags]。既定は 2（PW_RENDERFULLCONTENT）
                        string[] p = rest.Split(' ');
                        WinShot(p[0], p.Length > 1 ? uint.Parse(p[1]) : 2u);
                        break;
                    }
                case "dump":
                    {
                        sh.Refresh();
                        object o = sh.Get<object>(rest);
                        var tb = o as ToggleButton;
                        if (tb != null) Log("  " + rest + " = " + (tb.IsChecked == true ? "on" : "off"));
                        else Log("  " + rest + " = " + ((TextBlock) o).Text.Replace("\n", " / "));
                        break;
                    }
                case "tool":
                    {
                        var t = rest == "cam" ? sh.Cam : rest == "audio" ? sh.Audio : sh.Cap;
                        Log("  " + t.Name + " state=" + t.State + " want=" + t.Want + " running=" + t.Running + " pid=" + t.Pid + " starts=" + t.Starts);
                        lock (t.Recent) foreach (string x in t.Recent.GetRange(Math.Max(0, t.Recent.Count - 12), Math.Min(12, t.Recent.Count))) Log("    | " + x);
                        break;
                    }
                case "pids": Log("  cam=" + sh.Cam.Pid + " audio=" + sh.Audio.Pid + " cap=" + sh.Cap.Pid + " shm_writer=" + sh.preview.WriterPid + " preview_frames=" + sh.preview.Frames + " size=" + sh.preview.Width + "x" + sh.preview.Height); break;
                case "env":
                    foreach (var kv in ToolArgs.GameEnv(sh.S, null)) Log("  " + kv.Key + "=" + kv.Value);
                    Log("  flag " + (File.Exists(AppPaths.RecordFlag) ? "exists" : "none") + " " + AppPaths.RecordFlag);
                    break;
                case "endpoint":
                    {
                        // endpoint 名前：その出力の機器の音量とミュートを記録に書く（読むだけ）
                        var e = Endpoints.Get(rest);
                        Log("  " + (e != null ? e.ToString() : "(not found)"));
                        break;
                    }
                case "mute": Log("  pcmute active=" + sh.Mute.Active + " muted=" + sh.Mute.MutedName + " already=" + sh.Mute.AlreadyMutedName + " note=" + sh.Mute.LastNote + " saved=" + sh.S.Str("audio.mutedDev", "")); break;
                case "probe": Log("  monitor supported=" + (ToolArgs.MonitorSupported == null ? "unknown" : ToolArgs.MonitorSupported.ToString())); break;
                case "waitprobe":
                    waitCond = () => ToolArgs.MonitorSupported != null;
                    until = DateTime.Now.AddSeconds(double.Parse(rest, CultureInfo.InvariantCulture));
                    break;
                case "args": Log("  ipaudio " + ToolArgs.Audio(sh.S).Line); break;
                case "slider":
                    {
                        // slider 名前 値：つまみを動かして離したのと同じ
                        string[] p = rest.Split(' ');
                        var sl = sh.Get<Slider>(p[0]);
                        sl.Value = double.Parse(p[1], CultureInfo.InvariantCulture);
                        sl.RaiseEvent(new DragCompletedEventArgs(0, 0, false) { RoutedEvent = Thumb.DragCompletedEvent });
                        Log("  value " + sl.Value);
                        break;
                    }
                case "enabled":
                    {
                        var fe = sh.Get<FrameworkElement>(rest);
                        Log("  " + rest + " enabled=" + fe.IsEnabled);
                        break;
                    }
                case "quit": pc = lines.Count; break;
                case "ctl": CtlLog(); break;
                case "ctlrows": sh.Refresh(); Log("  " + sh.CtlRowsText()); break;
                case "waitctl":
                    {
                        string[] p = rest.Split(' ');
                        string what = p[0];
                        int n = p.Length > 2 ? int.Parse(p[1]) : 0;
                        waitCond = () =>
                        {
                            switch (what)
                            {
                                case "caps": return sh.ctlCaps != null && sh.ctlRows.Count > 0;
                                case "nocaps": return sh.ctlCaps == null;
                                case "pipe": return sh.Ctl.Connected;
                                case "old": return sh.ctlCtrl == false;
                                case "connects": return sh.Ctl.Connects >= n && sh.Ctl.Connected;
                                case "thermal": return sh.ctlThermal == n;
                                case "lowered": return sh.ctlFpsLowered == n;
                            }
                            return true;
                        };
                        until = DateTime.Now.AddSeconds(double.Parse(p[p.Length - 1], CultureInfo.InvariantCulture));
                        break;
                    }
                case "ctlclick":
                    {
                        var r = CtlRowOf(rest);
                        if (r == null) break;
                        if (r.Switch != null) { r.Switch.IsChecked = r.Switch.IsChecked != true; r.Switch.RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent)); Log("  switch " + (r.Switch.IsChecked == true ? "on" : "off")); }
                        else if (r.Button != null) { r.Button.RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent)); Log("  pressed"); }
                        else Log("  not clickable");
                        break;
                    }
                case "ctlselect":
                    {
                        string[] p = rest.Split(' ');
                        var r = CtlRowOf(p[0]);
                        if (r == null) break;
                        double v = double.Parse(p[1], CultureInfo.InvariantCulture);
                        bool hit = false;
                        if (r.Seg != null) foreach (var b in r.Seg) if (Math.Abs((double) b.Tag - v) < 0.001) { b.IsChecked = true; hit = true; }
                        if (r.Combo != null) foreach (ComboBoxItem c in r.Combo.Items) if (Math.Abs((double) c.Tag - v) < 0.001) { r.Combo.SelectedItem = c; hit = true; }
                        Log(hit ? "  selected " + p[1] : "  no such choice");
                        break;
                    }
                case "ctlslider":
                    {
                        string[] p = rest.Split(' ');
                        var r = CtlRowOf(p[0]);
                        if (r == null || r.Slider == null) { Log("  no slider"); break; }
                        r.Slider.Value = Shell.CtlToSlider(r.Item, double.Parse(p[1], CultureInfo.InvariantCulture));
                        r.Slider.RaiseEvent(new DragCompletedEventArgs(0, 0, false) { RoutedEvent = Thumb.DragCompletedEvent });
                        Log("  value " + sh.ctlLocal[r.Item.Id]);
                        break;
                    }
                case "ctldrag": CtlDrag(rest); break;
                case "ctlvalue":
                    {
                        var r = CtlRowOf(rest);
                        if (r == null) break;
                        int id = r.Item.Id;
                        double va, vb;
                        Log("  " + id + " shown=" + (sh.ctlLocal.TryGetValue(id, out va) ? va.ToString(CultureInfo.InvariantCulture) : "-") +
                            " received=" + (sh.ctlValues.TryGetValue(id, out vb) ? vb.ToString(CultureInfo.InvariantCulture) : "-") +
                            (r.Slider != null ? " slider=" + r.Slider.Value.ToString("0.####", CultureInfo.InvariantCulture) : "") +
                            (r.ValueText != null ? " text=" + r.ValueText.Text : ""));
                        break;
                    }
                default: Log("  unknown"); break;
            }
        }

        // 今のページを下まで入れる高さ（DesiredSize はページの外側の余白も含む）
        int FitHeight()
        {
            sh.Refresh();
            var pages = sh.Get<Grid>("Pages");
            pages.Measure(new Size(1280 - 300, double.PositiveInfinity));
            return Math.Max(768, (int) Math.Ceiling(pages.DesiredSize.Height) + 8);
        }

        CtlRow CtlRowOf(string id)
        {
            CtlRow r;
            if (sh.ctlRows.TryGetValue(int.Parse(id.Trim()), out r)) return r;
            Log("  no row " + id);
            return null;
        }

        static string Plain(TextBlock tb) { return new System.Windows.Documents.TextRange(tb.ContentStart, tb.ContentEnd).Text; }

        void CtlLog()
        {
            sh.Refresh();
            sh.RefreshHomeStatsForTest();
            Log("  pipe=" + sh.Ctl.Connected + " connects=" + sh.Ctl.Connects + " iphone=" + sh.ctlIphone + " ctrl=" + sh.ctlCtrl +
                " caps=" + (sh.ctlCaps == null ? "none" : string.Join(",", sh.ctlCaps.ConvertAll(x => x.Id.ToString()).ToArray())) +
                " rows=" + sh.CtlShownIds() + " thermal=" + sh.ctlThermal + " lowered=" + sh.ctlFpsLowered +
                " battC=" + sh.BattTempC + " battPct=" + sh.BattPercent + " sets=" + sh.ctlSetsSent + " lines=" + sh.Ctl.SentCount);
            Log("  note=" + sh.ctlNoteShown + " host=" + sh.Get<FrameworkElement>("CamCtlHost").Visibility + " enabled=" + sh.Get<FrameworkElement>("CamCtlHost").IsEnabled +
                " heat=" + sh.Get<FrameworkElement>("CamHeatCard").Visibility + " heatTitle=" + sh.Get<TextBlock>("CamHeatTitle").Text +
                " badge=" + sh.Get<FrameworkElement>("CamThermal").Visibility + " badgeText=" + Plain(sh.Get<TextBlock>("CamThermalText")) +
                " dotOpacity=" + sh.Get<System.Windows.Shapes.Ellipse>("CamThermalDot").Opacity +
                " home=" + Plain(sh.Get<TextBlock>("HomeDevStats")));
        }

        // つまみをつかんで動かす。つかんでいる間に、届いた値で位置が変わっていないか（跳ねたか）を毎回見る
        void CtlDrag(string rest)
        {
            string[] p = rest.Split(' ');
            var r = CtlRowOf(p[0]);
            if (r == null || r.Slider == null) { Log("  no slider"); return; }
            double from = double.Parse(p[1], CultureInfo.InvariantCulture), to = double.Parse(p[2], CultureInfo.InvariantCulture);
            double ms = double.Parse(p[3], CultureInfo.InvariantCulture);
            var s = r.Slider;
            int sets0 = sh.ctlSetsSent, jumps = 0, moves = 0;
            DateTime t0 = DateTime.Now;
            s.RaiseEvent(new DragStartedEventArgs(0, 0) { RoutedEvent = Thumb.DragStartedEvent });
            s.Value = Shell.CtlToSlider(r.Item, from);
            double last = s.Value;
            bool done = false;
            var t = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(15) };
            t.Tick += (o, e) =>
            {
                if (Math.Abs(s.Value - last) > 1e-9) jumps++;
                double k = Math.Min(1, (DateTime.Now - t0).TotalMilliseconds / ms);
                s.Value = Shell.CtlToSlider(r.Item, from + (to - from) * k);
                last = s.Value;
                moves++;
                if (k >= 1)
                {
                    t.Stop();
                    s.RaiseEvent(new DragCompletedEventArgs(0, 0, false) { RoutedEvent = Thumb.DragCompletedEvent });
                    double secs = (DateTime.Now - t0).TotalSeconds;
                    int sent = sh.ctlSetsSent - sets0;
                    Log("  drag " + r.Item.Id + " moves=" + moves + " sent_during=" + sent + " secs=" + secs.ToString("0.00", CultureInfo.InvariantCulture) +
                        " per_sec=" + (sent / secs).ToString("0.0", CultureInfo.InvariantCulture) + " jumps=" + jumps + " final=" + sh.ctlLocal[r.Item.Id]);
                    done = true;
                }
            };
            t.Start();
            waitCond = () => done;
            until = DateTime.Now.AddMilliseconds(ms + 5000);
        }

        void Click(string name)
        {
            object o = sh.Get<object>(name);
            var rb = o as RadioButton;
            var cb = o as CheckBox;
            var b = o as ButtonBase;
            if (rb != null) { rb.IsChecked = true; return; }
            if (cb != null) { cb.IsChecked = cb.IsChecked != true; cb.RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent)); return; }
            if (b != null) { b.RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent)); return; }
            Log("  not clickable");
        }

        void Shot(string file, int height)
        {
            sh.Refresh();
            FrameworkElement root = sh.Root;
            int w = 1280, h = height;
            if (win == null)
            {
                root.Measure(new Size(w, h));
                root.Arrange(new Rect(0, 0, w, h));
            }
            else { w = (int) root.ActualWidth; h = (int) root.ActualHeight; }
            root.UpdateLayout();
            var rtb = new RenderTargetBitmap(w, h, 96, 96, PixelFormats.Pbgra32);
            rtb.Render(root);
            var enc = new PngBitmapEncoder();
            enc.Frames.Add(BitmapFrame.Create(rtb));
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(file)));
            using (var fs = File.Create(file)) enc.Save(fs);
            Log("  wrote " + file + " " + w + "x" + h);
        }

        [System.Runtime.InteropServices.DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
        struct RECT { public int Left, Top, Right, Bottom; }

        // ウィンドウを画面から撮る（枠とタイトルバーも）。見えている枠の範囲は DWM に聞く（外側の透明な縁を含めない）
        void Screen(string file)
        {
            if (win == null) { Log("  no window"); return; }
            win.Activate();
            sh.Refresh();
            win.UpdateLayout();
            IntPtr hwnd = new System.Windows.Interop.WindowInteropHelper(win).Handle;
            RECT r;
            if (DwmGetWindowAttribute(hwnd, 9, out r, System.Runtime.InteropServices.Marshal.SizeOf(typeof(RECT))) != 0) { Log("  no frame bounds"); return; }
            int w = r.Right - r.Left, h = r.Bottom - r.Top;
            using (var bmp = new System.Drawing.Bitmap(w, h))
            {
                using (var g = System.Drawing.Graphics.FromImage(bmp)) g.CopyFromScreen(r.Left, r.Top, 0, 0, new System.Drawing.Size(w, h));
                Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(file)));
                bmp.Save(file, System.Drawing.Imaging.ImageFormat.Png);
            }
            Log("  wrote " + file + " " + w + "x" + h + " at " + r.Left + "," + r.Top);
        }

        // ComboBox の開いた一覧の中身（Popup の子）を、その場で並べて描く。色が切り替わっているかを見るため
        void Popup(string name, string file)
        {
            var cb = sh.Get<ComboBox>(name);
            cb.ApplyTemplate();
            var pop = cb.Template.FindName("PART_Popup", cb) as System.Windows.Controls.Primitives.Popup;
            var child = pop != null ? pop.Child as FrameworkElement : null;
            if (child == null) { Log("  no popup"); return; }
            child.Measure(new Size(400, 400));
            child.Arrange(new Rect(child.DesiredSize));
            child.UpdateLayout();
            int w = (int) Math.Ceiling(child.ActualWidth), h = (int) Math.Ceiling(child.ActualHeight);
            // 一覧の外側（角の外）が分かるよう、ページの地の上に描く
            var dv = new DrawingVisual();
            using (var dc = dv.RenderOpen())
            {
                dc.DrawRectangle(sh.Paint("Bg"), null, new Rect(0, 0, w + 16, h + 16));
                dc.DrawRectangle(new VisualBrush(child), null, new Rect(8, 8, w, h));
            }
            var rtb = new RenderTargetBitmap(w + 16, h + 16, 96, 96, PixelFormats.Pbgra32);
            rtb.Render(dv);
            var enc = new PngBitmapEncoder();
            enc.Frames.Add(BitmapFrame.Create(rtb));
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(file)));
            using (var fs = File.Create(file)) enc.Save(fs);
            Log("  wrote " + file + " " + (w + 16) + "x" + (h + 16) + " items=" + cb.Items.Count);
        }

        [System.Runtime.InteropServices.DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
        [System.Runtime.InteropServices.DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
        [System.Runtime.InteropServices.DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
        [System.Runtime.InteropServices.DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
        struct POINT { public int X, Y; }

        // 自分のウィンドウだけを撮る（画面からは撮らない）。PW_RENDERFULLCONTENT = 2 で、DWM が持っている絵をもらう。
        // 外側の透明な縁は DWM の見えている枠の範囲で切り落とす。
        // どのモニターにも載らない位置では WPF が中身を描かない（PrintWindow の中身は白）。そのときは、
        // 枠とタイトルバーは PrintWindow の絵のまま、中身の所だけ WPF に同じ中身（Root）を描かせて重ねる（記録に client=wpf と書く）
        void WinShot(string file, uint flags)
        {
            if (win == null) { Log("  no window"); return; }
            sh.Refresh();
            win.UpdateLayout();
            IntPtr hwnd = new System.Windows.Interop.WindowInteropHelper(win).Handle;
            RECT wr, fr;
            if (!GetWindowRect(hwnd, out wr)) { Log("  no window rect"); return; }
            if (DwmGetWindowAttribute(hwnd, 9, out fr, System.Runtime.InteropServices.Marshal.SizeOf(typeof(RECT))) != 0) fr = wr;
            int w = wr.Right - wr.Left, h = wr.Bottom - wr.Top;
            bool ok;
            using (var bmp = new System.Drawing.Bitmap(w, h, System.Drawing.Imaging.PixelFormat.Format32bppArgb))
            {
                using (var g = System.Drawing.Graphics.FromImage(bmp))
                {
                    IntPtr hdc = g.GetHdc();
                    ok = PrintWindow(hwnd, hdc, flags);
                    g.ReleaseHdc(hdc);
                }
                // 中身が描かれていないか（真っ白）を見る
                var pt = new POINT();
                ClientToScreen(hwnd, ref pt);
                int cx = pt.X - wr.Left, cy = pt.Y - wr.Top;
                FrameworkElement root = sh.Root;
                int rw = (int) root.ActualWidth, rh = (int) root.ActualHeight;
                bool blank = true;
                for (int i = 0; i < 64 && blank; i++)
                {
                    int sx = cx + (rw * (i % 8) + rw / 2) / 8, sy = cy + (rh * (i / 8) + rh / 2) / 8;
                    if (sx < w && sy < h && bmp.GetPixel(sx, sy).ToArgb() != System.Drawing.Color.White.ToArgb()) blank = false;
                }
                string client = "printwindow";
                if (blank)
                {
                    var rtb = new RenderTargetBitmap(rw, rh, 96, 96, PixelFormats.Pbgra32);
                    rtb.Render(root);
                    var enc = new PngBitmapEncoder();
                    enc.Frames.Add(BitmapFrame.Create(rtb));
                    using (var ms = new MemoryStream())
                    {
                        enc.Save(ms);
                        ms.Position = 0;
                        using (var content = new System.Drawing.Bitmap(ms))
                        using (var g = System.Drawing.Graphics.FromImage(bmp))
                            g.DrawImageUnscaled(content, cx, cy);
                    }
                    client = "wpf";
                }
                var crop = new System.Drawing.Rectangle(fr.Left - wr.Left, fr.Top - wr.Top, fr.Right - fr.Left, fr.Bottom - fr.Top);
                crop.Intersect(new System.Drawing.Rectangle(0, 0, w, h));
                Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(file)));
                using (var c = bmp.Clone(crop, System.Drawing.Imaging.PixelFormat.Format32bppArgb)) c.Save(file, System.Drawing.Imaging.ImageFormat.Png);
                Log("  wrote " + file + " " + crop.Width + "x" + crop.Height + " window at " + wr.Left + "," + wr.Top + "-" + wr.Right + "," + wr.Bottom +
                    " printwindow=" + ok + " client=" + client + " foreground=" + (GetForegroundWindow() == hwnd) + " virtualscreen=" + SystemParameters.VirtualScreenLeft + "," + SystemParameters.VirtualScreenTop + " " + SystemParameters.VirtualScreenWidth + "x" + SystemParameters.VirtualScreenHeight);
            }
        }

        void Quit()
        {
            Log("quit");
            if (win != null) win.Close();
            else { sh.Shutdown(); app.Shutdown(); }
        }
    }
}
