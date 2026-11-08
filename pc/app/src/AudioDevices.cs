// Windows の音の出力の機器（Core Audio の IMMDevice と IAudioEndpointVolume）を C# から触る。
// ・出力の機器の一覧と名前・既定の出力
// ・ミュートの読み書き（スピーカーとして使う間に PC の音を消す。PcMute）
// 名前の選び方は ipaudio の wa_find と同じ：空か "default" なら既定の出力（eConsole）、それ以外は名前に含む最初の動作中の出力。
// WASAPI の loopback は、既定ではミュートと音量の前の音を取る（Microsoft の AUDCLNT_STREAMOPTIONS の説明）。
// そのため、取る出力をミュートしても ipaudio は音を取れる。test\epvol で、誰も聞いていない Steam Streaming Speakers で確かめた。
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace PocketGPU
{
    static class CoreAudio
    {
        public const int eRender = 0, eCapture = 1, eConsole = 0, DEVICE_STATE_ACTIVE = 1, CLSCTX_ALL = 23;

        [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumeratorCo { }

        [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        public interface IMMDeviceEnumerator
        {
            [PreserveSig] int EnumAudioEndpoints(int dataFlow, int stateMask, out IMMDeviceCollection devices);
            [PreserveSig] int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice endpoint);
            [PreserveSig] int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
            [PreserveSig] int RegisterEndpointNotificationCallback(IntPtr client);
            [PreserveSig] int UnregisterEndpointNotificationCallback(IntPtr client);
        }

        [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        public interface IMMDeviceCollection
        {
            [PreserveSig] int GetCount(out int count);
            [PreserveSig] int Item(int index, out IMMDevice device);
        }

        [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        public interface IMMDevice
        {
            [PreserveSig] int Activate(ref Guid iid, int clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
            [PreserveSig] int OpenPropertyStore(int access, out IPropertyStore props);
            [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
            [PreserveSig] int GetState(out int state);
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct PROPERTYKEY { public Guid fmtid; public int pid; }

        [StructLayout(LayoutKind.Sequential)]
        public struct PROPVARIANT { public ushort vt, r1, r2, r3; public IntPtr p; public IntPtr p2; }

        [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        public interface IPropertyStore
        {
            [PreserveSig] int GetCount(out int count);
            [PreserveSig] int GetAt(int index, out PROPERTYKEY key);
            [PreserveSig] int GetValue(ref PROPERTYKEY key, out PROPVARIANT value);
            [PreserveSig] int SetValue(ref PROPERTYKEY key, ref PROPVARIANT value);
            [PreserveSig] int Commit();
        }

        [ComImport, Guid("5CDF2C82-841E-4546-9722-0CF74078229A"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        public interface IAudioEndpointVolume
        {
            [PreserveSig] int RegisterControlChangeNotify(IntPtr notify);
            [PreserveSig] int UnregisterControlChangeNotify(IntPtr notify);
            [PreserveSig] int GetChannelCount(out uint count);
            [PreserveSig] int SetMasterVolumeLevel(float levelDB, ref Guid context);
            [PreserveSig] int SetMasterVolumeLevelScalar(float level, ref Guid context);
            [PreserveSig] int GetMasterVolumeLevel(out float levelDB);
            [PreserveSig] int GetMasterVolumeLevelScalar(out float level);
            [PreserveSig] int SetChannelVolumeLevel(uint channel, float levelDB, ref Guid context);
            [PreserveSig] int SetChannelVolumeLevelScalar(uint channel, float level, ref Guid context);
            [PreserveSig] int GetChannelVolumeLevel(uint channel, out float levelDB);
            [PreserveSig] int GetChannelVolumeLevelScalar(uint channel, out float level);
            [PreserveSig] int SetMute([MarshalAs(UnmanagedType.Bool)] bool mute, ref Guid context);
            [PreserveSig] int GetMute([MarshalAs(UnmanagedType.Bool)] out bool mute);
            [PreserveSig] int GetVolumeStepInfo(out uint step, out uint count);
            [PreserveSig] int VolumeStepUp(ref Guid context);
            [PreserveSig] int VolumeStepDown(ref Guid context);
            [PreserveSig] int QueryHardwareSupport(out uint mask);
            [PreserveSig] int GetVolumeRange(out float minDB, out float maxDB, out float incDB);
        }

        [DllImport("ole32.dll")] static extern int PropVariantClear(ref PROPVARIANT pv);

        static readonly PROPERTYKEY FriendlyName = new PROPERTYKEY { fmtid = new Guid("A45C254E-DF1C-4EFD-8020-67D146A850E0"), pid = 14 };
        public static Guid IID_IAudioEndpointVolume = new Guid("5CDF2C82-841E-4546-9722-0CF74078229A");

        public static IMMDeviceEnumerator Enumerator() { return (IMMDeviceEnumerator) new MMDeviceEnumeratorCo(); }

        public static string Name(IMMDevice d)
        {
            IPropertyStore ps;
            if (d.OpenPropertyStore(0, out ps) != 0 || ps == null) return "";
            try
            {
                var key = FriendlyName;
                PROPVARIANT v;
                if (ps.GetValue(ref key, out v) != 0) return "";
                string s = v.vt == 31 ? Marshal.PtrToStringUni(v.p) : "";
                PropVariantClear(ref v);
                return s ?? "";
            }
            finally { Marshal.ReleaseComObject(ps); }
        }
        public static string Id(IMMDevice d) { string id; return d.GetId(out id) == 0 ? id : null; }

        // ipaudio の wa_find と同じ選び方。見つからなければ null
        public static IMMDevice Find(IMMDeviceEnumerator en, int flow, string needle)
        {
            IMMDevice d;
            if (string.IsNullOrEmpty(needle) || needle == "default")
                return en.GetDefaultAudioEndpoint(flow, eConsole, out d) == 0 ? d : null;
            IMMDeviceCollection col;
            if (en.EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, out col) != 0) return null;
            try
            {
                int n;
                col.GetCount(out n);
                for (int i = 0; i < n; i++)
                {
                    IMMDevice c;
                    if (col.Item(i, out c) != 0) continue;
                    if (Name(c).IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0) return c;
                    Marshal.ReleaseComObject(c);
                }
            }
            finally { Marshal.ReleaseComObject(col); }
            return null;
        }

        public static IAudioEndpointVolume Volume(IMMDevice d)
        {
            object o;
            var iid = IID_IAudioEndpointVolume;
            return d.Activate(ref iid, CLSCTX_ALL, IntPtr.Zero, out o) == 0 ? (IAudioEndpointVolume) o : null;
        }
    }

    // 1 つの出力の機器の今の状態（名前・ID・音量・ミュート）
    class EndpointInfo
    {
        public string Id, Name;
        public float Volume = -1;
        public bool Muted, IsDefault;
        public override string ToString() { return Name + " vol=" + Volume.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture) + (Muted ? " muted" : "") + (IsDefault ? " default" : "") + " id=" + Id; }
    }

    static class Endpoints
    {
        public static EndpointInfo Get(string needle)
        {
            try
            {
                var en = CoreAudio.Enumerator();
                var d = CoreAudio.Find(en, CoreAudio.eRender, needle);
                if (d == null) return null;
                try { return Info(en, d); }
                finally { Marshal.ReleaseComObject(d); Marshal.ReleaseComObject(en); }
            }
            catch (Exception ex) { Program.Log("endpoint " + needle + ": " + ex.Message); return null; }
        }
        public static EndpointInfo GetById(string id)
        {
            try
            {
                var en = CoreAudio.Enumerator();
                CoreAudio.IMMDevice d;
                if (en.GetDevice(id, out d) != 0 || d == null) return null;
                try { return Info(en, d); }
                finally { Marshal.ReleaseComObject(d); Marshal.ReleaseComObject(en); }
            }
            catch (Exception ex) { Program.Log("endpoint id " + id + ": " + ex.Message); return null; }
        }
        static EndpointInfo Info(CoreAudio.IMMDeviceEnumerator en, CoreAudio.IMMDevice d)
        {
            var e = new EndpointInfo { Id = CoreAudio.Id(d), Name = CoreAudio.Name(d) };
            var v = CoreAudio.Volume(d);
            if (v != null)
            {
                float x; bool m;
                if (v.GetMasterVolumeLevelScalar(out x) == 0) e.Volume = x;
                if (v.GetMute(out m) == 0) e.Muted = m;
                Marshal.ReleaseComObject(v);
            }
            CoreAudio.IMMDevice def;
            if (en.GetDefaultAudioEndpoint(CoreAudio.eRender, CoreAudio.eConsole, out def) == 0 && def != null)
            {
                e.IsDefault = CoreAudio.Id(def) == e.Id;
                Marshal.ReleaseComObject(def);
            }
            return e;
        }

        // ミュートを書く。ID で選ぶ（名前が同じ機器を取り違えない）。うまくいけば true
        public static bool SetMute(string id, bool mute)
        {
            try
            {
                var en = CoreAudio.Enumerator();
                CoreAudio.IMMDevice d;
                if (en.GetDevice(id, out d) != 0 || d == null) { Marshal.ReleaseComObject(en); return false; }
                try
                {
                    var v = CoreAudio.Volume(d);
                    if (v == null) return false;
                    var ctx = Guid.Empty;
                    int hr = v.SetMute(mute, ref ctx);
                    Marshal.ReleaseComObject(v);
                    return hr == 0 || hr == 1; // S_FALSE はもうその状態
                }
                finally { Marshal.ReleaseComObject(d); Marshal.ReleaseComObject(en); }
            }
            catch (Exception ex) { Program.Log("mute " + id + ": " + ex.Message); return false; }
        }

        // 音量を書く（試験の道具だけが使う。アプリは音量を変えない）
        public static bool SetVolume(string id, float level)
        {
            try
            {
                var en = CoreAudio.Enumerator();
                CoreAudio.IMMDevice d;
                if (en.GetDevice(id, out d) != 0 || d == null) { Marshal.ReleaseComObject(en); return false; }
                try
                {
                    var v = CoreAudio.Volume(d);
                    if (v == null) return false;
                    var ctx = Guid.Empty;
                    int hr = v.SetMasterVolumeLevelScalar(level, ref ctx);
                    Marshal.ReleaseComObject(v);
                    return hr == 0;
                }
                finally { Marshal.ReleaseComObject(d); Marshal.ReleaseComObject(en); }
            }
            catch (Exception ex) { Program.Log("volume " + id + ": " + ex.Message); return false; }
        }
    }

    // スピーカーとして使う間だけ、音を取る出力をミュートする。止めたら元に戻す。
    // ・もともとミュートだった機器には触らない（あとで勝手に戻さないため）
    // ・ミュートした機器の ID を settings.json の audio.mutedDev に書く。アプリが落ちても、次に開いたときに戻す
    // ・戻すとき、利用者がその間に自分でミュートを外していたら何もしない
    // 試験用：PGPU_TEST_MUTE_ONLY に名前の一部を書くと、それを含む機器しかミュートしない（本物の機器を消音しないため）
    class PcMute
    {
        readonly Settings s;
        public string MutedId { get; private set; }
        public string MutedName { get; private set; }
        public string AlreadyMutedName { get; private set; }   // 利用者が先にミュートしていた機器（触らない）
        public string LastNote = "";

        public PcMute(Settings s)
        {
            this.s = s;
            MutedId = s.Str("audio.mutedDev", "");
            MutedName = s.Str("audio.mutedName", "");
            if (MutedId.Length == 0) MutedId = null;
        }
        public bool Active { get { return MutedId != null; } }

        // 前に落ちたときの残り。起動したときに 1 回呼ぶ
        public void RestoreLeftover()
        {
            if (MutedId == null) return;
            Program.Log("pcmute leftover " + MutedName);
            Restore();
        }

        public void Apply(string needle)
        {
            var e = Endpoints.Get(needle);
            if (e == null) { LastNote = "not found " + needle; return; }
            if (MutedId != null && MutedId == e.Id) return;
            if (MutedId != null) Restore(); // 取る出力が変わった
            string only = Environment.GetEnvironmentVariable("PGPU_TEST_MUTE_ONLY");
            if (!string.IsNullOrEmpty(only) && e.Name.IndexOf(only, StringComparison.OrdinalIgnoreCase) < 0)
            {
                LastNote = "refused (test) " + e.Name;
                Program.Log("pcmute refused by PGPU_TEST_MUTE_ONLY: " + e.Name);
                return;
            }
            if (e.Muted) { AlreadyMutedName = e.Name; LastNote = "already muted " + e.Name; Program.Log("pcmute skip, already muted by the user: " + e); return; }
            AlreadyMutedName = null;
            // 先に覚えてからミュートする（間で落ちても、次に開いたときに戻せる）
            s.Set("audio.mutedDev", e.Id);
            s.Set("audio.mutedName", e.Name);
            if (Endpoints.SetMute(e.Id, true))
            {
                MutedId = e.Id;
                MutedName = e.Name;
                LastNote = "muted " + e.Name;
                Program.Log("pcmute on " + e);
            }
            else
            {
                s.Set("audio.mutedDev", "");
                s.Set("audio.mutedName", "");
                LastNote = "failed " + e.Name;
                Program.Log("pcmute failed " + e);
            }
        }

        public void Restore()
        {
            AlreadyMutedName = null;
            if (MutedId == null) return;
            var e = Endpoints.GetById(MutedId);
            if (e != null && e.Muted) Endpoints.SetMute(MutedId, false);
            Program.Log("pcmute off " + (e != null ? e.ToString() : MutedName + " (gone)"));
            LastNote = "restored " + MutedName;
            MutedId = null;
            MutedName = null;
            s.Set("audio.mutedDev", "");
            s.Set("audio.mutedName", "");
        }
    }
}
