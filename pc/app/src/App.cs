// PocketGPU.exe の入口。WPF のウィンドウを作り、中身（Shell）を入れる。管理者の権限は要らない。
//   PocketGPU.exe                         ふつうに開く（同時に 1 つだけ）
// 作る人・試験用（試す人は使わない）：
//   --offscreen                           ウィンドウを出さずに中身だけ作る（1280x768 で並べる）。--auto と一緒に使う
//   --window-at X,Y                       ウィンドウをこの位置に出す。試験では、どのモニターにも載らない位置にする
//   --no-activate                         ウィンドウを出しても前に出さない・タスクバーに出さない（試験用。--window-at と一緒に使う）
//   --auto 台本.txt --auto-log 記録.txt   台本の手順で操作して PNG に撮る（Auto.cs）。--auto の間は GPU を使わずに描く
//   --make-icon app.ico                   アイコンのファイルを書いて終わる（build.cmd が使う）
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace PocketGPU
{
    static class Program
    {
        public const string Version = "0.1.0";
        static readonly object logLock = new object();
        public static string AutoLog;
        public static bool AutoMode;   // 試験の台本（--auto）の間。確認のウィンドウを出さない

        public static void Log(string s)
        {
            lock (logLock)
            {
                try
                {
                    string line = DateTime.Now.ToString("HH:mm:ss.fff") + " " + s + "\r\n";
                    File.AppendAllText(Path.Combine(AppPaths.Ensure(AppPaths.LogDir), "app.log"), line, Encoding.UTF8);
                    if (AutoLog != null) File.AppendAllText(AutoLog, line, Encoding.UTF8);
                }
                catch { }
            }
        }

        [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
        [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);

        [STAThread]
        static int Main(string[] args)
        {
            string auto = null, iconOut = null;
            bool offscreen = false, noActivate = false;
            double[] at = null;
            for (int i = 0; i < args.Length; i++)
            {
                if (args[i] == "--offscreen") offscreen = true;
                else if (args[i] == "--no-activate") noActivate = true;
                else if (args[i] == "--auto" && i + 1 < args.Length) auto = args[++i];
                else if (args[i] == "--auto-log" && i + 1 < args.Length) AutoLog = args[++i];
                else if (args[i] == "--make-icon" && i + 1 < args.Length) iconOut = args[++i];
                else if (args[i] == "--window-at" && i + 1 < args.Length)
                {
                    string[] p = args[++i].Split(',');
                    at = new[] { double.Parse(p[0]), double.Parse(p[1]) };
                }
            }
            if (iconOut != null) { WriteIcon(iconOut); return 0; }
            // 試験の台本では GPU を使わない（ウィンドウを出すときも、ソフトで描く）
            if (auto != null) RenderOptions.ProcessRenderMode = System.Windows.Interop.RenderMode.SoftwareOnly;
            AutoMode = auto != null;

            Mutex single = null;
            if (auto == null)
            {
                bool created;
                single = new Mutex(true, "Local\\PocketGPU.App", out created);
                if (!created)
                {
                    // もう開いている：そのウィンドウを前に出して終わる
                    foreach (var p in System.Diagnostics.Process.GetProcessesByName(System.Diagnostics.Process.GetCurrentProcess().ProcessName))
                        if (p.Id != System.Diagnostics.Process.GetCurrentProcess().Id && p.MainWindowHandle != IntPtr.Zero)
                        {
                            ShowWindow(p.MainWindowHandle, 9);
                            SetForegroundWindow(p.MainWindowHandle);
                        }
                    return 0;
                }
            }

            var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
            app.DispatcherUnhandledException += (o, e) => { Log("unhandled: " + e.Exception); e.Handled = true; };
            AppDomain.CurrentDomain.UnhandledException += (o, e) => Log("fatal: " + e.ExceptionObject);
            var settings = Settings.Load();
            L.Ja = L.PickJa(settings.Str("lang", "auto"));
            Shell.LoadResources(app);
            Shell shell;
            try { shell = new Shell(settings, app.Dispatcher); }
            catch (Exception ex)
            {
                Log("start: " + ex);
                MessageBox.Show(ex.Message, "PocketGPU", MessageBoxButton.OK, MessageBoxImage.Error);
                return 1;
            }
            Log("start " + Version + " lang=" + (L.Ja ? "ja" : "en") + " data=" + AppPaths.DataDir + (offscreen ? " offscreen" : ""));

            Window win = null;
            if (!offscreen)
            {
                win = new Window
                {
                    Title = "PocketGPU",
                    Width = settings.Int("win.w", 1280),
                    Height = settings.Int("win.h", 800),
                    MinWidth = 960,
                    MinHeight = 600,
                    Background = shell.Paint("Bg"),
                    Content = shell.Root,
                    Icon = IconImage(32),
                    WindowStartupLocation = WindowStartupLocation.CenterScreen
                };
                if (noActivate) { win.ShowActivated = false; win.ShowInTaskbar = false; }
                if (at != null)
                {
                    win.WindowStartupLocation = WindowStartupLocation.Manual;
                    win.Left = at[0];
                    win.Top = at[1];
                    win.Width = 1280;
                    win.Height = 800;
                }
                else if (settings.Str("win.x", "").Length > 0)
                {
                    // 前の位置。いまの画面の外なら真ん中に
                    double x = settings.Int("win.x", 0), y = settings.Int("win.y", 0);
                    if (x >= SystemParameters.VirtualScreenLeft && y >= SystemParameters.VirtualScreenTop &&
                        x + 200 <= SystemParameters.VirtualScreenLeft + SystemParameters.VirtualScreenWidth && y + 100 <= SystemParameters.VirtualScreenTop + SystemParameters.VirtualScreenHeight)
                    {
                        win.WindowStartupLocation = WindowStartupLocation.Manual;
                        win.Left = x;
                        win.Top = y;
                    }
                }
                // タイトルバーとウィンドウの地を、中身の色（ライト・ダーク）に合わせる。切り替わったときもその場で
                Action fitWindow = () =>
                {
                    var bg = shell.Paint("Bg") as SolidColorBrush;
                    win.Background = bg;
                    Theme.ApplyTitleBar(win, shell.Dark, bg != null ? bg.Color : Colors.White);
                };
                win.SourceInitialized += (o, e) => fitWindow();
                shell.ThemeChanged += () => fitWindow();
                win.Closing += (o, e) =>
                {
                    if (at == null && win.WindowState == WindowState.Normal)
                    {
                        settings.Set("win.x", (int) win.Left);
                        settings.Set("win.y", (int) win.Top);
                        settings.Set("win.w", (int) win.Width);
                        settings.Set("win.h", (int) win.Height);
                    }
                    shell.Shutdown();
                };
                win.Closed += (o, e) => app.Shutdown();
                GameLaunch.SetOwner(win);
                win.Show();
            }
            else
            {
                shell.Root.Measure(new Size(1280, 768));
                shell.Root.Arrange(new Rect(0, 0, 1280, 768));
            }

            if (auto != null)
            {
                var runner = new Auto(shell, win, app, auto);
                runner.Start();
            }
            app.Run();
            if (single != null) single.ReleaseMutex();
            return 0;
        }

        // ---- アイコン：線のアイコン（i_app）を白い角丸の上に。描いてから使う
        public static BitmapSource IconBitmap(int size)
        {
            var dv = new DrawingVisual();
            using (var dc = dv.RenderOpen())
            {
                double s = size / 24.0;
                var g = ((Geometry) Application.Current.Resources["i_app"]).Clone();
                g.Transform = new ScaleTransform(s, s);
                var pen = new Pen(new SolidColorBrush(Color.FromRgb(0x1B, 0x1B, 0x1B)), Math.Max(1.0, 1.5 * s)) { StartLineCap = PenLineCap.Round, EndLineCap = PenLineCap.Round, LineJoin = PenLineJoin.Round };
                var tile = new RectangleGeometry(new Rect(3.2 * s, 3.2 * s, 17.6 * s, 17.6 * s), 4.2 * s, 4.2 * s);
                dc.DrawGeometry(Brushes.White, null, tile);
                dc.DrawGeometry(null, pen, g);
            }
            var rtb = new RenderTargetBitmap(size, size, 96, 96, PixelFormats.Pbgra32);
            rtb.Render(dv);
            return rtb;
        }
        static ImageSource IconImage(int size) { try { return IconBitmap(size); } catch { return null; } }

        // .ico（中身は PNG）を書く。16・24・32・48・256
        static void WriteIcon(string path)
        {
            var app = new Application();
            Shell.LoadResources(app);
            int[] sizes = { 16, 24, 32, 48, 256 };
            var pngs = new List<byte[]>();
            foreach (int sz in sizes)
            {
                var enc = new PngBitmapEncoder();
                enc.Frames.Add(BitmapFrame.Create(IconBitmap(sz)));
                using (var ms = new MemoryStream()) { enc.Save(ms); pngs.Add(ms.ToArray()); }
            }
            using (var fs = File.Create(path))
            using (var w = new BinaryWriter(fs))
            {
                w.Write((ushort) 0); w.Write((ushort) 1); w.Write((ushort) sizes.Length);
                int off = 6 + 16 * sizes.Length;
                for (int i = 0; i < sizes.Length; i++)
                {
                    w.Write((byte) (sizes[i] >= 256 ? 0 : sizes[i])); w.Write((byte) (sizes[i] >= 256 ? 0 : sizes[i]));
                    w.Write((byte) 0); w.Write((byte) 0); w.Write((ushort) 1); w.Write((ushort) 32);
                    w.Write(pngs[i].Length); w.Write(off);
                    off += pngs[i].Length;
                }
                foreach (var p in pngs) w.Write(p);
            }
        }
    }
}
