// usbmuxd（Apple のサービス・127.0.0.1:27015）と iPhone のアプリの受け口との話し方。
// pgpu_common.ps1 の Invoke-Mux・Get-MuxDevices・Connect-Phone・Invoke-Hello・Measure-UsbSpeed を C# に移した物。
// 受け手（50060）は一度に 1 人しか相手をしないので、ゲームの実行中にあいさつしない（呼ぶ側で守る）。
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Xml;

namespace PocketGPU
{
    class MuxDevice
    {
        public long Id;
        public string Type = "";   // USB か Network
        public long Speed;         // ConnectionSpeed（480000000 は USB 2、5000000000 以上は USB 3）
        public long ProductId;
        public string Serial = "";
    }

    class HelloResult
    {
        public bool Ok;
        public string Why = "";    // timeout・closed・result・short・mux・refused
        public int Result;
        public string Name = "";
        public long MemMB;
        public uint Caps;
    }

    static class Mux
    {
        public const int RecvPort = 50060;   // app/vk/remote/proto.h の VR_DEFAULT_PORT
        public const int BwPort = 50058;     // USB の速さを測る受け口（vk/remote/bw.c）
        public const int Lockdown = 62078;
        public const int OpHello = 1;
        public const int OpGetStatus = 78;   // VR_GET_STATUS（proto.h の enum の並び）
        public const uint CapStatus = 32768; // VR_CAP_STATUS

        public static int MuxPort
        {
            get
            {
                int p;
                string e = Environment.GetEnvironmentVariable("IPHONE_VK_MUX");
                return !string.IsNullOrEmpty(e) && int.TryParse(e, out p) ? p : 27015;
            }
        }
        // kit.ini の proto=（無ければ 3）
        public static int ProtoVersion
        {
            get
            {
                try
                {
                    string ini = Path.Combine(Path.Combine(ToolPaths.KitDir, "tools"), "kit.ini");
                    if (File.Exists(ini))
                        foreach (string l in File.ReadAllLines(ini))
                            if (l.StartsWith("proto=")) return int.Parse(l.Substring(6).Trim());
                }
                catch { }
                return 3;
            }
        }
        public static string AppVersion
        {
            get
            {
                try
                {
                    string ini = Path.Combine(Path.Combine(ToolPaths.KitDir, "tools"), "kit.ini");
                    if (File.Exists(ini))
                        foreach (string l in File.ReadAllLines(ini))
                            if (l.StartsWith("app=")) return l.Substring(4).Trim();
                }
                catch { }
                return "";
            }
        }

        public static TcpClient ConnectTcp(string host, int port, int ms)
        {
            var c = new TcpClient();
            try
            {
                var ar = c.BeginConnect(host, port, null, null);
                if (!ar.AsyncWaitHandle.WaitOne(ms)) { c.Close(); return null; }
                c.EndConnect(ar);
            }
            catch { c.Close(); return null; }
            c.NoDelay = true;
            return c;
        }

        public static bool Listening()
        {
            var c = ConnectTcp("127.0.0.1", MuxPort, 1500);
            if (c == null) return false;
            c.Close();
            return true;
        }

        static byte[] ReadExact(Stream s, int n)
        {
            var b = new byte[n];
            int got = 0;
            while (got < n)
            {
                int k = s.Read(b, got, n - got);
                if (k <= 0) throw new EndOfStreamException("closed");
                got += k;
            }
            return b;
        }

        // ---- plist（XML）
        static object PlistNode(XmlNode n)
        {
            switch (n.LocalName)
            {
                case "dict":
                    {
                        var h = new Dictionary<string, object>();
                        var kids = new List<XmlNode>();
                        foreach (XmlNode k in n.ChildNodes) if (k.NodeType == XmlNodeType.Element) kids.Add(k);
                        for (int i = 0; i + 1 < kids.Count; i += 2) h[kids[i].InnerText] = PlistNode(kids[i + 1]);
                        return h;
                    }
                case "array":
                    {
                        var a = new List<object>();
                        foreach (XmlNode k in n.ChildNodes) if (k.NodeType == XmlNodeType.Element) a.Add(PlistNode(k));
                        return a;
                    }
                case "integer": { long v; long.TryParse(n.InnerText, out v); return v; }
                case "true": return true;
                case "false": return false;
                default: return n.InnerText;
            }
        }
        static Dictionary<string, object> ParsePlist(string text)
        {
            if (text.StartsWith("bplist")) throw new IOException("binary plist");
            int d = text.IndexOf("<!DOCTYPE", StringComparison.Ordinal);
            if (d >= 0) { int e = text.IndexOf('>', d); if (e > d) text = text.Remove(d, e - d + 1); }
            var doc = new XmlDocument { XmlResolver = null };
            doc.LoadXml(text);
            foreach (XmlNode k in doc.DocumentElement.ChildNodes)
                if (k.NodeType == XmlNodeType.Element) return PlistNode(k) as Dictionary<string, object>;
            return null;
        }
        static string PlistXml(string body)
        {
            return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict>" + body + "</dict></plist>\n";
        }

        // usbmuxd に 1 つ頼む（app/vk/icd/icd.c の mux_call と同じ形）
        static Dictionary<string, object> Call(TcpClient c, string type, string extra)
        {
            byte[] body = Encoding.UTF8.GetBytes(PlistXml("<key>MessageType</key><string>" + type + "</string><key>ClientVersionString</key><string>pocketgpu-app</string><key>ProgName</key><string>pocketgpu-app</string>" + extra));
            var head = new byte[16];
            BitConverter.GetBytes((uint) (16 + body.Length)).CopyTo(head, 0);
            BitConverter.GetBytes(1u).CopyTo(head, 4);
            BitConverter.GetBytes(8u).CopyTo(head, 8);
            BitConverter.GetBytes(1u).CopyTo(head, 12);
            var s = c.GetStream();
            s.ReadTimeout = 3000;
            s.Write(head, 0, 16);
            s.Write(body, 0, body.Length);
            byte[] h = ReadExact(s, 16);
            uint len = BitConverter.ToUInt32(h, 0);
            if (len < 16 || len > 1 << 20) throw new IOException("usbmuxd: bad reply length");
            return ParsePlist(Encoding.UTF8.GetString(ReadExact(s, (int) len - 16)));
        }

        static long Num(object o) { return o is long ? (long) o : 0; }

        // つながっている機器。usbmuxd が答えなければ null
        public static List<MuxDevice> ListDevices()
        {
            var c = ConnectTcp("127.0.0.1", MuxPort, 1500);
            if (c == null) return null;
            try
            {
                var r = Call(c, "ListDevices", "");
                var list = new List<MuxDevice>();
                object dl;
                if (r != null && r.TryGetValue("DeviceList", out dl) && dl is List<object>)
                    foreach (object o in (List<object>) dl)
                    {
                        var d = o as Dictionary<string, object>;
                        if (d == null) continue;
                        object po;
                        var p = d.TryGetValue("Properties", out po) ? po as Dictionary<string, object> : null;
                        if (p == null) p = new Dictionary<string, object>();
                        object v;
                        var md = new MuxDevice();
                        md.Id = d.TryGetValue("DeviceID", out v) ? Num(v) : 0;
                        md.Type = p.TryGetValue("ConnectionType", out v) ? Convert.ToString(v) : "";
                        md.Speed = p.TryGetValue("ConnectionSpeed", out v) ? Num(v) : 0;
                        md.ProductId = p.TryGetValue("ProductID", out v) ? Num(v) : 0;
                        md.Serial = p.TryGetValue("SerialNumber", out v) ? Convert.ToString(v) : "";
                        list.Add(md);
                    }
                return list;
            }
            catch { return null; }
            finally { c.Close(); }
        }

        // iPhone の port 番につなぐ。そのまま USB の通り道になる。失敗は why に
        public static TcpClient ConnectPhone(long dev, int port, out string why)
        {
            why = "";
            var c = ConnectTcp("127.0.0.1", MuxPort, 1500);
            if (c == null) { why = "mux"; return null; }
            int pn = ((port & 0xFF) << 8) | ((port >> 8) & 0xFF);
            try
            {
                var r = Call(c, "Connect", "<key>DeviceID</key><integer>" + dev + "</integer><key>PortNumber</key><integer>" + pn + "</integer>");
                object n;
                if (r == null || !r.TryGetValue("Number", out n) || Num(n) != 0) { c.Close(); why = "refused"; return null; }
                return c;
            }
            catch { c.Close(); why = "mux"; return null; }
        }

        static bool IsClosed(Exception ex)
        {
            while (ex != null)
            {
                if (ex is EndOfStreamException) return true;
                var se = ex as SocketException;
                if (se != null && se.SocketErrorCode != SocketError.TimedOut) return true;
                ex = ex.InnerException;
            }
            return false;
        }

        // 受け手にあいさつ（VR_HELLO）して、GPU の名前・メモリ・caps を聞く。返事が来ないのは、ほかのアプリが使っている時
        public static HelloResult Hello(TcpClient c, int timeoutMs)
        {
            var r = new HelloResult();
            var s = c.GetStream();
            s.ReadTimeout = timeoutMs;
            s.WriteTimeout = timeoutMs;
            var req = new byte[12];
            BitConverter.GetBytes((uint) OpHello).CopyTo(req, 0);
            BitConverter.GetBytes(4u).CopyTo(req, 4);
            BitConverter.GetBytes((uint) ProtoVersion).CopyTo(req, 8);
            byte[] h;
            try { s.Write(req, 0, 12); h = ReadExact(s, 8); }
            catch (Exception ex) { r.Why = IsClosed(ex) ? "closed" : "timeout"; return r; }
            int res = BitConverter.ToInt32(h, 0);
            uint n = BitConverter.ToUInt32(h, 4);
            if (res != 0) { r.Why = "result"; r.Result = res; return r; }
            if (n < 276 || n > 16 << 20) { r.Why = "short"; return r; }
            byte[] b;
            try { b = ReadExact(s, (int) n); } catch { r.Why = "closed"; return r; }
            // VkPhysicalDeviceProperties の deviceName は 20 バイト目から 256 バイト
            int end = 20;
            while (end < 276 && b[end] != 0) end++;
            r.Name = Encoding.UTF8.GetString(b, 20, end - 20);
            // そのあと VkPhysicalDeviceFeatures（220）と VkPhysicalDeviceMemoryProperties。heap の数は 1304、heap[i] は 1308+16i
            if (n >= 1316)
            {
                uint nh = BitConverter.ToUInt32(b, 1304);
                for (int i = 0; i < Math.Min(nh, 16u); i++)
                {
                    ulong size = BitConverter.ToUInt64(b, 1308 + 16 * i);
                    uint flags = BitConverter.ToUInt32(b, 1316 + 16 * i);
                    if ((flags & 1) != 0) r.MemMB += (long) (size / (1024 * 1024));
                }
            }
            r.Caps = BitConverter.ToUInt32(b, (int) n - 4); // 返事の最後の 4 バイトが caps（server.c の VR_HELLO）
            r.Ok = true;
            return r;
        }

        // 熱の状態（0 ふつう〜3 とても熱い）と、後ろに回っているか。HELLO のあと同じつなぎで聞く
        public static bool GetStatus(TcpClient c, out uint thermal, out uint paused)
        {
            thermal = 0; paused = 0;
            try
            {
                var s = c.GetStream();
                var req = new byte[8];
                BitConverter.GetBytes((uint) OpGetStatus).CopyTo(req, 0);
                s.Write(req, 0, 8);
                byte[] h = ReadExact(s, 8);
                if (BitConverter.ToInt32(h, 0) != 0) return false;
                uint n = BitConverter.ToUInt32(h, 4);
                if (n < 8 || n > 4096) return false;
                byte[] b = ReadExact(s, (int) n);
                thermal = BitConverter.ToUInt32(b, 0);
                paused = BitConverter.ToUInt32(b, 4);
                return true;
            }
            catch { return false; }
        }

        // 受け手が答えるかを 1 回だけ見る（つないで、あいさつして、切る）。wantStatus なら熱も聞く
        public static HelloResult TestReceiver(long dev, int timeoutMs, bool wantStatus, out int thermal)
        {
            thermal = -1;
            string why;
            var c = ConnectPhone(dev, RecvPort, out why);
            if (c == null) return new HelloResult { Why = why };
            try
            {
                var r = Hello(c, timeoutMs);
                if (r.Ok && wantStatus && (r.Caps & CapStatus) != 0)
                {
                    uint t, p;
                    if (GetStatus(c, out t, out p)) thermal = (int) t;
                }
                else if (r.Ok && wantStatus) thermal = -2; // 古いアプリ
                return r;
            }
            finally { c.Close(); }
        }

        // ---- USB の速さ（app/pc/usb_bw.py と同じ約束。iPhone の 50058 番）
        static void BwTransfer(NetworkStream s, int dir, long n, byte[] buf)
        {
            var h = new byte[12];
            BitConverter.GetBytes((uint) dir).CopyTo(h, 0);
            BitConverter.GetBytes((ulong) n).CopyTo(h, 4);
            s.Write(h, 0, 12);
            long left = n;
            if (dir == 0)
            {
                while (left > 0)
                {
                    int k = s.Read(buf, 0, (int) Math.Min(buf.Length, left));
                    if (k <= 0) throw new IOException("closed");
                    left -= k;
                }
            }
            else
            {
                while (left > 0)
                {
                    int k = (int) Math.Min(buf.Length, left);
                    s.Write(buf, 0, k);
                    left -= k;
                }
                byte[] back = ReadExact(s, 8);
                if (BitConverter.ToUInt64(back, 0) != (ulong) n) throw new IOException("count mismatch");
            }
        }
        // 戻り値：null は「測れない」（受け口が無い）。err に文字が入れば途中で切れた
        public static double[] MeasureUsb(long dev, int mb, out string err)
        {
            err = null;
            string why;
            var c = ConnectPhone(dev, BwPort, out why);
            if (c == null) return null;
            try
            {
                c.ReceiveBufferSize = 4 << 20;
                c.SendBufferSize = 4 << 20;
                var s = c.GetStream();
                s.ReadTimeout = 20000;
                s.WriteTimeout = 20000;
                var buf = new byte[1 << 20];
                var t = new List<double>();
                for (int i = 0; i < 20; i++)
                {
                    var sw = Stopwatch.StartNew();
                    BwTransfer(s, 0, 1, buf);
                    t.Add(sw.Elapsed.TotalMilliseconds);
                }
                t.Sort();
                double rtt = t[t.Count / 2];
                BwTransfer(s, 0, 2 << 20, buf);
                BwTransfer(s, 1, 2 << 20, buf);
                long n = (long) mb << 20;
                var w = Stopwatch.StartNew(); BwTransfer(s, 0, n, buf); double down = mb / w.Elapsed.TotalSeconds;
                w = Stopwatch.StartNew(); BwTransfer(s, 1, n, buf); double up = mb / w.Elapsed.TotalSeconds;
                return new[] { down, up, rtt };
            }
            catch (Exception ex) { err = ex.Message; return new double[0]; }
            finally { c.Close(); }
        }

        // ---- lockdownd に、つながりを始めずに値を聞く（ideviceinfo -s と同じ。ProductType など限られた値だけ返る）
        public static string LockdownValue(long dev, string key)
        {
            string why;
            var c = ConnectPhone(dev, Lockdown, out why);
            if (c == null) return null;
            try
            {
                var s = c.GetStream();
                s.ReadTimeout = 2000;
                s.WriteTimeout = 2000;
                byte[] body = Encoding.UTF8.GetBytes(PlistXml("<key>Label</key><string>pocketgpu-app</string><key>Request</key><string>GetValue</string><key>Key</key><string>" + key + "</string>"));
                var len = BitConverter.GetBytes((uint) body.Length);
                Array.Reverse(len);
                s.Write(len, 0, 4);
                s.Write(body, 0, body.Length);
                byte[] lb = ReadExact(s, 4);
                Array.Reverse(lb);
                uint n = BitConverter.ToUInt32(lb, 0);
                if (n == 0 || n > 1 << 20) return null;
                var r = ParsePlist(Encoding.UTF8.GetString(ReadExact(s, (int) n)));
                object v;
                return r != null && r.TryGetValue("Value", out v) ? Convert.ToString(v) : null;
            }
            catch { return null; }
            finally { c.Close(); }
        }

        // ProductType → 名前。知らない型は null（画面では「iPhone」）
        public static string ModelName(string productType)
        {
            if (productType == null) return null;
            var t = new Dictionary<string, string>
            {
                { "iPhone14,2", "iPhone 13 Pro" }, { "iPhone14,3", "iPhone 13 Pro Max" }, { "iPhone14,4", "iPhone 13 mini" }, { "iPhone14,5", "iPhone 13" },
                { "iPhone14,6", "iPhone SE" }, { "iPhone14,7", "iPhone 14" }, { "iPhone14,8", "iPhone 14 Plus" },
                { "iPhone15,2", "iPhone 14 Pro" }, { "iPhone15,3", "iPhone 14 Pro Max" }, { "iPhone15,4", "iPhone 15" }, { "iPhone15,5", "iPhone 15 Plus" },
                { "iPhone16,1", "iPhone 15 Pro" }, { "iPhone16,2", "iPhone 15 Pro Max" },
                { "iPhone17,1", "iPhone 16 Pro" }, { "iPhone17,2", "iPhone 16 Pro Max" }, { "iPhone17,3", "iPhone 16" }, { "iPhone17,4", "iPhone 16 Plus" }, { "iPhone17,5", "iPhone 16e" },
                { "iPhone18,1", "iPhone 17 Pro" }, { "iPhone18,2", "iPhone 17 Pro Max" }, { "iPhone18,3", "iPhone 17" }, { "iPhone18,4", "iPhone Air" }
            };
            string n;
            return t.TryGetValue(productType, out n) ? n : (productType.StartsWith("iPad") ? "iPad" : null);
        }
    }
}
