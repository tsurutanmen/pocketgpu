// ウィンドウの中身。XAML（exe に埋め込み）を読み、名前で部品を探してつなぐ。ページごとの動きは Page*.cs（partial）。
// 画面の更新は 0.5 秒ごとの Refresh でまとめて行う（道具の状態・iPhone の状態から、見えている文を作り直す）。
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Threading;

namespace PocketGPU
{
    class DevInfo
    {
        public bool MuxUp;
        public MuxDevice Dev;      // 使う 1 台（USB を先に）
        public string Model;       // iPhone 17 Pro など。分からなければ null
    }

    partial class Shell
    {
        public static readonly string[] PageNames = { "Home", "Gpu", "Camera", "Audio", "Caption", "Record", "Check", "Settings" };
        public readonly Grid Root;
        public readonly Settings S;
        readonly Dispatcher ui;
        readonly Dictionary<string, FrameworkElement> pages = new Dictionary<string, FrameworkElement>();
        public string Current = "Home";
        DispatcherTimer tick;
        public DevInfo Dev = new DevInfo();
        readonly Dictionary<long, string> models = new Dictionary<long, string>();
        public ChildTool Cam, Audio, Cap;
        public CheckRunner Check;
        public readonly List<RunningGame> Games = new List<RunningGame>();

        public static Stream Xaml(string name)
        {
            string dir = Environment.GetEnvironmentVariable("PGPU_XAML_DIR"); // 作る人用：組み直さずに XAML だけ直して見る
            if (!string.IsNullOrEmpty(dir) && File.Exists(Path.Combine(dir, name + ".xaml"))) return File.OpenRead(Path.Combine(dir, name + ".xaml"));
            var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("ui." + name + ".xaml");
            if (s == null) throw new FileNotFoundException("ui." + name + ".xaml");
            return s;
        }
        public static object LoadXaml(string name)
        {
            using (var s = Xaml(name)) return XamlReader.Load(s);
        }

        // アプリ全体の見た目（Styles.xaml）を Application.Resources に入れる。ページを読む前に呼ぶ
        public static void LoadResources(Application app)
        {
            app.Resources = (ResourceDictionary) LoadXaml("Styles");
        }
        // 文（L）と色（Theme）は中身の外枠（Root）に持たせる。差し替えると、その下の DynamicResource がその場で変わる
        // （Application に持たせると、ウィンドウの無い時に変わらない）
        ResourceDictionary stringsDict, colorsDict;
        void ApplyLanguage()
        {
            var md = Root.Resources.MergedDictionaries;
            if (stringsDict != null) md.Remove(stringsDict);
            stringsDict = L.MakeDictionary();
            md.Add(stringsDict);
        }

        // ---- ライトとダーク。設定の theme（auto・light・dark）と Windows のモードから決める
        public bool Dark;
        public event Action ThemeChanged;   // ウィンドウの地とタイトルバーを合わせる（App.cs）
        public void ApplyTheme()
        {
            string setting = S.Str("theme", "auto");
            bool dark = Theme.PickDark(setting);
            if (colorsDict != null && dark == Dark) return;
            var md = Root.Resources.MergedDictionaries;
            var next = Theme.Load(dark);
            md.Add(next);
            if (colorsDict != null) md.Remove(colorsDict);
            colorsDict = next;
            Dark = dark;
            Program.Log("theme " + (dark ? "dark" : "light") + " setting=" + setting + " windows=" + (Theme.WindowsDark() ? "dark" : "light") + (Theme.IsTestKey ? " key=" + Theme.KeyPath : ""));
            if (ThemeChanged != null) ThemeChanged();
        }
        public void SetTheme(string setting)
        {
            S.Set("theme", setting);
            ApplyTheme();
            FillSettingsCombos(); // 設定の画面の欄もそろえる（ここ以外から変えたとき）
            Refresh();
        }
        // 今の色。コードで色を塗るときは、切り替わりに付いて来るよう SetResourceReference を使う。これは今の値を読むだけ
        public Brush Paint(string key) { return Root.TryFindResource(key) as Brush; }

        public Shell(Settings s, Dispatcher ui)
        {
            S = s;
            this.ui = ui;
            Root = (Grid) LoadXaml("Main");
            ApplyLanguage();
            ApplyTheme();
            Root.Language = XmlLanguage.GetLanguage(L.Ja ? "ja-JP" : "en-US");
            var host = Get<Grid>("Pages");
            foreach (string n in PageNames)
            {
                var p = (FrameworkElement) LoadXaml(n);
                p.Visibility = Visibility.Collapsed;
                host.Children.Add(p);
                pages[n] = p;
            }

            Cam = new ChildTool("webcam_rx", () => ToolArgs.Webcam(S), ui) { StopWaitMs = 2500 };
            Audio = new ChildTool("ipaudio", () => ToolArgs.Audio(S), ui) { StopWaitMs = 1500 };
            Cap = new ChildTool("livecap", () => ToolArgs.Caption(S), ui) { StopWaitMs = 6000 };
            Check = new CheckRunner(ui);
            Check.MediaOnly = S.Bool("mediaOnly", false);

            foreach (string n in PageNames)
            {
                string name = n;
                var nav = Get<RadioButton>("Nav" + n);
                nav.Checked += (o, e) => Show(name);
            }
            InitHome();
            InitGpu();
            InitCamera();
            InitAudio();
            InitCaption();
            InitRecord();
            InitCheck();
            InitSettings();

            Get<RadioButton>("NavHome").IsChecked = true;
            ApplyMode();
            Refresh();

            tick = new DispatcherTimer(DispatcherPriority.Background, ui) { Interval = TimeSpan.FromMilliseconds(500) };
            tick.Tick += (o, e) => Refresh();
            tick.Start();
            Theme.Watch(ui, d => { if (S.Str("theme", "auto") == "auto") ApplyTheme(); });
            var t = new Thread(PollDevices) { IsBackground = true, Name = "devices" };
            t.Start();
            StartBatteryPoll();
        }

        // 名前で部品を探す（外枠とページはそれぞれ名前の範囲が別）
        public T Get<T>(string name) where T : class
        {
            object o = Root.FindName(name);
            if (o == null) foreach (var p in pages.Values) { o = p.FindName(name); if (o != null) break; }
            if (o == null) throw new KeyNotFoundException("XAML に " + name + " がありません");
            return (T) o;
        }

        public void Show(string name)
        {
            Current = name;
            foreach (var kv in pages) kv.Value.Visibility = kv.Key == name ? Visibility.Visible : Visibility.Collapsed;
            Get<ScrollViewer>("Scroll").ScrollToTop();
            var nav = Get<RadioButton>("Nav" + name);
            if (nav.IsChecked != true) nav.IsChecked = true;
            OnShow(name);
            Refresh();
        }

        void OnShow(string name)
        {
            if (name == "Camera") CameraShown();
            if (name == "Audio") AudioShown();
            if (name == "Check" && Check.LastRun == null && !Check.Busy) Check.Run(1);
            if (name == "Settings") SettingsShown();
        }

        // マイクとカメラだけ使う：GPU と録画の項目を隠す
        public void ApplyMode()
        {
            bool media = S.Bool("mediaOnly", false);
            Check.MediaOnly = media;
            var hide = media ? Visibility.Collapsed : Visibility.Visible;
            Get<RadioButton>("NavGpu").Visibility = hide;
            Get<RadioButton>("NavRecord").Visibility = hide;
            Get<FrameworkElement>("HomeRowGpu").Visibility = hide;
            Get<FrameworkElement>("HomeRowRecord").Visibility = hide;
            // 1 行目を隠したら、次の行の上の線を消す
            Get<Border>("HomeRowCamera").Style = (Style) Application.Current.Resources[media ? "Row" : "RowNext"];
            if (media && (Current == "Gpu" || Current == "Record")) Show("Home");
        }

        public void SetLanguage(string setting)
        {
            S.Set("lang", setting);
            L.Ja = L.PickJa(setting);
            ApplyLanguage();
            Root.Language = XmlLanguage.GetLanguage(L.Ja ? "ja-JP" : "en-US");
            FillCombos();
            Refresh();
        }

        void FillCombos()
        {
            FillCameraCombos();
            FillCaptionCombos();
            FillSettingsCombos();
            AudioShown();
        }

        // ---- iPhone の様子：2 秒ごとに usbmuxd に聞く（iPhone には触れない）。機種名は新しい機器を見た時に 1 回だけ lockdownd に聞く
        void PollDevices()
        {
            while (true)
            {
                var d = new DevInfo();
                var list = Mux.ListDevices();
                d.MuxUp = list != null;
                if (list != null)
                {
                    var usb = list.Find(x => x.Type == "USB");
                    d.Dev = usb ?? list.Find(x => x.Type == "Network");
                    if (d.Dev != null)
                    {
                        string m;
                        if (!models.TryGetValue(d.Dev.Id, out m))
                        {
                            m = Mux.ModelName(Mux.LockdownValue(d.Dev.Id, "ProductType"));
                            models[d.Dev.Id] = m;
                        }
                        d.Model = m;
                    }
                }
                ui.BeginInvoke(new Action(() => { Dev = d; }));
                Thread.Sleep(2000);
            }
        }

        public string DevStateText()
        {
            if (!Dev.MuxUp) return L.T("dev_nomux");
            if (Dev.Dev == null) return L.T("dev_none");
            if (Dev.Dev.Type == "Network") return L.T("dev_wifi");
            if (Dev.Dev.Speed >= 5000000000L) return L.T("dev_usb3");
            if (Dev.Dev.Speed > 0) return L.T("dev_usb2");
            return L.T("dev_usb");
        }
        public string DevName() { return Dev.Dev != null && Dev.Model != null ? Dev.Model : L.T("dev_name"); }

        public bool GameRunning()
        {
            Games.RemoveAll(g => !g.Alive && (DateTime.Now - g.Started).TotalSeconds > 5);
            foreach (var g in Games) if (g.Alive) return true;
            return false;
        }

        public void Refresh()
        {
            try
            {
                Get<TextBlock>("DevName").Text = DevName();
                Get<TextBlock>("DevState").Text = DevStateText();
                RefreshCameraState();
                UpdatePcMute();
                RefreshHome();
                switch (Current)
                {
                    case "Gpu": RefreshGpu(); break;
                    case "Camera": RefreshCamera(); break;
                    case "Audio": RefreshAudio(); break;
                    case "Caption": RefreshCaption(); break;
                    case "Record": RefreshRecord(); break;
                    case "Check": RefreshCheck(); break;
                    case "Settings": RefreshSettings(); break;
                }
            }
            catch (Exception ex) { Program.Log("refresh: " + ex); }
        }

        // 閉じるとき：道具を全部止め、録画の印を消す
        public void Shutdown()
        {
            tick.Stop();
            Ctl.Stop();
            StopPreview();
            var stops = new List<Thread>();
            foreach (var t in new[] { Cam, Audio, Cap })
            {
                var tool = t;
                var th = new Thread(() => tool.Stop(true)) { IsBackground = true };
                th.Start();
                stops.Add(th);
            }
            foreach (var th in stops) th.Join(8000);
            // スピーカーとして使う間にミュートした PC の出力を戻す
            try { if (Mute != null) Mute.Restore(); } catch (Exception ex) { Program.Log("pcmute restore: " + ex.Message); }
            try { if (File.Exists(AppPaths.RecordFlag)) File.Delete(AppPaths.RecordFlag); } catch { }
            S.Save();
        }

        // ---- 部品を作る小道具（コードで作る行）
        public static Style St(string key) { return (Style) Application.Current.Resources[key]; }
        public static object Res(string key) { return Application.Current.Resources[key]; }

        public static ContentControl Icon(string key, string style)
        {
            var c = new ContentControl { Style = St(style), Tag = Res(key) };
            if (key == "i_rec") c.Content = Res("i_rec_fill");
            return c;
        }
        public static TextBlock Text(string s, string style)
        {
            return new TextBlock { Text = s, Style = St(style) };
        }
        public static Button Btn(string text, string style, RoutedEventHandler click)
        {
            var b = new Button { Content = text, Style = St(style) };
            if (click != null) b.Click += click;
            return b;
        }

        // 行：左にアイコン、右に部品（ボタンや値）、真ん中に題と説明
        public static Border Row(bool first, string icon, string title, string sub, params UIElement[] right)
        {
            var row = new Border { Style = St(first ? "Row" : "RowNext") };
            var dp = new DockPanel();
            dp.Children.Add(Icon(icon, "RowIco"));
            foreach (var r in right) { DockPanel.SetDock(r, Dock.Right); dp.Children.Add(r); }
            var mid = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            mid.Children.Add(Text(title, "RowTitle"));
            if (!string.IsNullOrEmpty(sub)) mid.Children.Add(Text(sub, "RowSub"));
            dp.Children.Add(mid);
            row.Child = dp;
            return row;
        }

        public static void SelectByTag(ComboBox cb, string tag)
        {
            foreach (object o in cb.Items)
            {
                var it = o as ComboBoxItem;
                if (it != null && (string) it.Tag == tag) { cb.SelectedItem = it; return; }
            }
            if (cb.Items.Count > 0) cb.SelectedIndex = 0;
        }
        public static string SelectedTag(ComboBox cb)
        {
            var it = cb.SelectedItem as ComboBoxItem;
            return it != null ? (string) it.Tag : null;
        }
        public static void Fill(ComboBox cb, params string[] tagText)
        {
            cb.Items.Clear();
            for (int i = 0; i + 1 < tagText.Length; i += 2) cb.Items.Add(new ComboBoxItem { Tag = tagText[i], Content = tagText[i + 1] });
        }

        // スイッチ：利用者が押したときだけ動く（Click）。表示は Refresh が状態に合わせる
        public static void OnSwitch(CheckBox cb, Action<bool> act)
        {
            cb.Click += (o, e) => act(cb.IsChecked == true);
        }

        public static void OpenFolder(string dir)
        {
            try
            {
                AppPaths.Ensure(dir);
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("explorer.exe", "\"" + dir + "\"") { UseShellExecute = true });
            }
            catch { }
        }

        // 道具の状態から、見出しの文を作る（共通）
        public static string ToolStateText(ChildTool t, string onText)
        {
            switch (t.State)
            {
                case ToolState.Missing: return L.F("st_missing", t.Name + ".exe");
                case ToolState.Failed: return L.F("st_failed", t.LastError);
                case ToolState.Off: return L.T("st_off");
                case ToolState.Starting: return L.T("st_starting");
                case ToolState.Waiting: return L.T("st_wait");
                default: return onText;
            }
        }
    }
}
