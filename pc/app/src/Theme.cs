// 画面の色：ライトとダーク。
// ・設定の "theme" は auto・light・dark。auto のときは Windows のアプリのモードに合わせる。
//   読む場所は HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize の AppsUseLightTheme（0 ならダーク）。
// ・動いている間に Windows の設定が変わったら、裏の糸が RegNotifyChangeKeyValue で気づいて、その場で切り替える。
//   届かなかったときのために、10 秒ごとにも読み直す。
// ・色の名前と値は ui\ThemeLight.xaml・ui\ThemeDark.xaml。Shell が中身の外枠（Root）の Resources に差し込む。
// ・タイトルバーは DWM に頼む。DWMWA_USE_IMMERSIVE_DARK_MODE は 20（Microsoft の DWMWINDOWATTRIBUTE の資料）。
//   Windows 10 の 20H1 より前のビルドは 19 だったので、20 が失敗したら 19 で頼み直す（Microsoft の Terminal と同じやり方）。
//   タイトルバーの地の色（DWMWA_CAPTION_COLOR = 35）は Windows 11 から。古い Windows では失敗して何も変わらない。
// 試験用：PGPU_THEME_KEY に HKCU の下の別の場所を書くと、そこを読む（本物の Windows の設定を書き換えずに、切り替えを試すため）。
using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

namespace PocketGPU
{
    static class Theme
    {
        public const string PersonalizeKey = @"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize";
        public const string ValueName = "AppsUseLightTheme";

        public static string KeyPath
        {
            get
            {
                string t = Environment.GetEnvironmentVariable("PGPU_THEME_KEY");
                return string.IsNullOrEmpty(t) ? PersonalizeKey : t;
            }
        }
        public static bool IsTestKey { get { return KeyPath != PersonalizeKey; } }

        // Windows のアプリのモードがダークか。値が無い・読めないときはライト（Windows の既定）
        public static bool WindowsDark()
        {
            try
            {
                using (var k = Registry.CurrentUser.OpenSubKey(KeyPath, false))
                {
                    if (k == null) return false;
                    object v = k.GetValue(ValueName);
                    if (v is int) return (int) v == 0;
                }
            }
            catch { }
            return false;
        }

        public static bool PickDark(string setting)
        {
            if (setting == "dark") return true;
            if (setting == "light") return false;
            return WindowsDark();
        }

        public static ResourceDictionary Load(bool dark)
        {
            return (ResourceDictionary) Shell.LoadXaml(dark ? "ThemeDark" : "ThemeLight");
        }

        // ---- Windows の設定の見張り
        [DllImport("advapi32.dll")]
        static extern int RegNotifyChangeKeyValue(SafeRegistryHandle key, bool watchSubtree, int filter, SafeWaitHandle ev, bool async);
        const int REG_NOTIFY_CHANGE_LAST_SET = 4;
        static Thread watcher;

        // Windows のモードが変わったら、UI の糸で changed(ダークか) を呼ぶ
        public static void Watch(Dispatcher ui, Action<bool> changed)
        {
            if (watcher != null) return;
            // 鍵は開いたままにして、知らせが来るたびに頼み直す（閉じると知らせが鳴るので、毎回は開け閉めしない）。
            // 10 秒で知らせが来なければ、頼んだまま読み直すだけ（頼みを重ねない）
            watcher = new Thread(() =>
            {
                bool last = WindowsDark();
                RegistryKey k = null;
                bool armed = false;
                using (var ev = new AutoResetEvent(false))
                {
                    while (true)
                    {
                        if (k == null)
                        {
                            try { k = Registry.CurrentUser.OpenSubKey(KeyPath, false); } catch { }
                            armed = false;
                        }
                        if (k != null && !armed)
                        {
                            int rc = -1;
                            try { rc = RegNotifyChangeKeyValue(k.Handle, false, REG_NOTIFY_CHANGE_LAST_SET, ev.SafeWaitHandle, true); } catch { }
                            armed = rc == 0;
                            if (!armed) { try { k.Close(); } catch { } k = null; }
                        }
                        if (armed) { if (ev.WaitOne(10000)) armed = false; }
                        else { Thread.Sleep(2000); ev.Reset(); }
                        bool now = WindowsDark();
                        if (now != last)
                        {
                            last = now;
                            bool d = now;
                            ui.BeginInvoke(new Action(() => changed(d)));
                        }
                    }
                }
            }) { IsBackground = true, Name = "theme" };
            watcher.Start();
        }

        // ---- タイトルバー
        [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr h, int attr, ref int v, int size);
        [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
        const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
        const int DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 = 19;
        const int DWMWA_CAPTION_COLOR = 35;
        public static string LastTitleBar = "";

        // dark：タイトルバーを暗く描かせるか。bg：タイトルバーの地の色（中身の Bg と同じにする）
        public static void ApplyTitleBar(Window w, bool dark, Color bg)
        {
            try
            {
                IntPtr h = new WindowInteropHelper(w).Handle;
                if (h == IntPtr.Zero) return;
                int on = dark ? 1 : 0;
                int hr = DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, ref on, 4);
                int used = DWMWA_USE_IMMERSIVE_DARK_MODE;
                if (hr < 0)
                {
                    used = DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1;
                    hr = DwmSetWindowAttribute(h, used, ref on, 4);
                }
                int colorref = bg.R | (bg.G << 8) | (bg.B << 16);
                int hr2 = DwmSetWindowAttribute(h, DWMWA_CAPTION_COLOR, ref colorref, 4);
                // 枠を描き直させる（動いている間に切り替えたとき）
                SetWindowPos(h, IntPtr.Zero, 0, 0, 0, 0, 0x0020 | 0x0002 | 0x0001 | 0x0004 | 0x0010); // FRAMECHANGED・NOMOVE・NOSIZE・NOZORDER・NOACTIVATE
                LastTitleBar = string.Format("dark={0} attr={1} hr=0x{2:X8} caption=0x{3:X6} hr=0x{4:X8}", on, used, hr, colorref, hr2);
                Program.Log("titlebar " + LastTitleBar);
            }
            catch (Exception ex) { Program.Log("titlebar: " + ex.Message); }
        }
    }
}
