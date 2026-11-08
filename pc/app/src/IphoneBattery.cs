// USB でつないだ iPhone の電池の温度（℃）・残り（%）・充電中かを読むだけ。iPhone の設定や中身は何も変えない。
// app/pc/iphone_battery.py を C# 5（.NET Framework 4.8・csc.exe）に移した物。外の部品は使わない。
//
//   BatteryInfo b = IphoneBattery.Read(null, 5000);   // USB の最初の iPhone。null の UDID なら最初の 1 台
//   if (b.TemperatureC == null) 理由は b.Error（日本語の短い文）。例外は投げない。
//
// 流れ：usbmuxd（127.0.0.1:27015・IPHONE_VK_MUX で変えられる。Mux.cs と同じ）で USB の iPhone を探す
//   → lockdown（62078）に QueryType・StartSession（C:\ProgramData\Apple\Lockdown\<UDID>.plist の HostID と SystemBUID）
//   → EnableSessionSSL なら TLS 1.2（記録の HostCertificate と HostPrivateKey を出す。iPhone の証明書は記録の DeviceCertificate と同じ物だけ通す）
//   → StartService com.apple.mobile.diagnostics_relay → そのポートにつなぐ（EnableServiceSSL なら同じく TLS）
//   → {"Request":"IORegistry","EntryClass":"IOPMPowerSource"} → Diagnostics.IORegistry の Temperature（100 分の 1 度）・CurrentCapacity・IsCharging
//   → Goodbye で閉じる。
// 1 回ごとにつないで読んで閉じる。かかる時間は同じプロセスの 2 回目から 70ms ほど、初回は JIT の分で 130〜210ms（10/12 に測った）。
// 呼んだスレッドで止まって待つので、PC アプリでは UI のスレッドから呼ばない。
// 秘密鍵は TLS の間だけ Windows の鍵の入れ物（今のユーザーの CSP・名前は毎回新しい）に入れ、終わったらすぐ消す。
// SChannel は名前の付いた入れ物の鍵しか使えないため（手本の Python が一時ファイルに書いてすぐ消すのと同じ考え）。
// 呼ぶ間隔は 10 秒に 1 回以下にする（呼ぶ側で守る）。
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;
using System.Xml;

namespace PocketGPU
{
    class BatteryInfo
    {
        public double? TemperatureC;   // 電池の温度（℃）。読めないときは null
        public int? Percent;           // 残り（%）
        public bool? Charging;         // 充電中か（IsCharging）。満充電で電源につないだままなら false
        public string Error;           // 読めないときの短い理由（日本語）。読めたら null
    }

    static class IphoneBattery
    {
        public const string DiagService = "com.apple.mobile.diagnostics_relay";
        const int LockdownPort = 62078;
        const string Label = "PocketGPU";
        const string TimeoutMsg = "時間内に iPhone から応答がありませんでした";

        // ペアリングの記録の場所。試験でだけ変える
        internal static string LockdownDir = @"C:\ProgramData\Apple\Lockdown";
        // 試験用：わざと記録と違う証明書（HostCertificate）と比べて、TLS が通らないことを確かめる。PC アプリでは触らない
        internal static bool TestPinMismatch = false;
        // iPhone の lockdown は TLS 1.2 まで（10/12 に iOS 26.6.2 で、TLS 1.3 も出せる OpenSSL に対して 1.2 を選んだ）。試験でだけ変える
        internal static SslProtocols TlsProtocols = SslProtocols.Tls12;
        // 直前の Read の段ごとの時間（試す道具が出す）
        [ThreadStatic] internal static string LastSteps;
        // 直前の Read で読んだ IORegistry の値の名前（試す道具の --keys が出す）
        [ThreadStatic] internal static List<string> LastKeys;

        static int MuxPort
        {
            get
            {
                int p;
                string e = Environment.GetEnvironmentVariable("IPHONE_VK_MUX");
                return !string.IsNullOrEmpty(e) && int.TryParse(e, out p) ? p : 27015;
            }
        }

        public static BatteryInfo Read(string udidOrNull, int timeoutMs)
        {
            var info = new BatteryInfo();
            if (timeoutMs <= 0) timeoutMs = 5000;
            var ctx = new Ctx(timeoutMs);
            LastKeys = null;
            Pairing pr = null;
            try
            {
                using (var timer = new System.Threading.Timer(delegate { ctx.Expire(); }, null, timeoutMs, Timeout.Infinite))
                {
                    Run(ctx, udidOrNull, info, ref pr);
                }
            }
            catch (Fail f)
            {
                info.Error = ctx.TimedOut ? TimeoutMsg : f.Message;
            }
            catch (Exception e)
            {
                if (ctx.TimedOut) info.Error = TimeoutMsg;
                else if (ctx.PinMismatch) info.Error = "iPhone の証明書がペアリングの記録と一致しません";
                else info.Error = ctx.Stage + "：" + Short(e);
            }
            finally
            {
                ctx.CloseAll();
                if (pr != null) pr.Dispose();
                LastSteps = ctx.Steps.ToString().Trim() + " 合計 " + ctx.Elapsed + "ms";
            }
            if (info.Error != null) info.TemperatureC = null;
            return info;
        }

        static void Run(Ctx ctx, string udidOrNull, BatteryInfo info, ref Pairing pr)
        {
            // 1. usbmuxd で USB の iPhone を探す
            ctx.Stage = "Apple のサービスとの通信に失敗しました";
            string udid = null;
            if (udidOrNull != null)
            {
                udid = udidOrNull.Trim();
                // UDID を指定されたときは、先に記録の有無を見る（無い UDID はそこで止まる）
                ctx.Stage = "ペアリングの記録を読めません";
                pr = Pairing.Load(udid);
                ctx.Mark("記録");
                ctx.Stage = "Apple のサービスとの通信に失敗しました";
            }
            long deviceId = -1;
            object listObj;
            using (Stream mux = MuxOpen(ctx))
                listObj = MuxRequest(mux, MuxMsg("ListDevices"));
            var list = Get(listObj, "DeviceList") as List<object>;
            if (list == null) throw new Fail("Apple のサービスの応答が正しくありません");
            bool anyUsb = false;
            foreach (object d in list)
            {
                var props = Get(d, "Properties");
                if (!"USB".Equals(Get(props, "ConnectionType") as string)) continue;
                anyUsb = true;
                string serial = Get(props, "SerialNumber") as string;
                object id = Get(d, "DeviceID");
                if (serial == null || !(id is long)) continue;
                if (udid == null || string.Equals(serial, udid, StringComparison.OrdinalIgnoreCase))
                {
                    udid = serial;
                    deviceId = (long)id;
                    break;
                }
            }
            if (deviceId < 0)
                throw new Fail(udidOrNull != null && anyUsb ? "指定の iPhone が USB で認識されていません" : "iPhone が USB で認識されていません");
            ctx.Mark("探す");

            if (pr == null)
            {
                ctx.Stage = "ペアリングの記録を読めません";
                pr = Pairing.Load(udid);
                ctx.Mark("記録");
            }

            // 2. lockdown でセッションを始める
            ctx.Stage = "iPhone との通信に失敗しました";
            Stream ld = MuxConnect(ctx, deviceId, LockdownPort, "lockdown");
            LdSend(ld, Dict("Label", Label, "Request", "QueryType"));
            object qt = LdRecv(ld);
            string type = Get(qt, "Type") as string;
            if (type != null && type != "com.apple.mobile.lockdown")
                throw new Fail("iPhone の lockdown の応答が正しくありません：" + type);
            LdSend(ld, Dict("Label", Label, "Request", "StartSession", "HostID", pr.HostID, "SystemBUID", pr.SystemBUID));
            object ss = LdRecv(ld);
            string err = Get(ss, "Error") as string;
            if (err != null)
            {
                if (err == "InvalidHostID") throw new Fail("ペアリングが無効です。iPhone でこのコンピューターを信頼し直してください");
                if (err == "PasswordProtected") throw new Fail("iPhone のロックを解除してください");
                throw new Fail("iPhone がセッションを開始しませんでした：" + err);
            }
            ctx.Mark("lockdown");
            if (true.Equals(Get(ss, "EnableSessionSSL")))
            {
                ctx.Stage = "TLS の接続に失敗しました";
                ld = Tls(ctx, ld, pr);
                ctx.Mark("TLS");
                ctx.Stage = "iPhone との通信に失敗しました";
            }

            // 3. 診断のサービスを開始してつなぐ
            LdSend(ld, Dict("Label", Label, "Request", "StartService", "Service", DiagService));
            object sv = LdRecv(ld);
            err = Get(sv, "Error") as string;
            if (err != null)
            {
                if (err == "PasswordProtected") throw new Fail("iPhone のロックを解除してください");
                throw new Fail("診断のサービスを開始できませんでした：" + err);
            }
            object portObj = Get(sv, "Port");
            if (!(portObj is long) || (long)portObj <= 0 || (long)portObj > 65535)
                throw new Fail("診断のサービスのポートが返ってきませんでした");
            ctx.Stage = "診断のサービスとの通信に失敗しました";
            Stream dg = MuxConnect(ctx, deviceId, (int)(long)portObj, "診断のサービス");
            if (true.Equals(Get(sv, "EnableServiceSSL")))
            {
                ctx.Stage = "TLS の接続に失敗しました";
                dg = Tls(ctx, dg, pr);
                ctx.Stage = "診断のサービスとの通信に失敗しました";
            }
            ctx.Mark("サービス");

            // 4. 電源の部品の値を聞く
            LdSend(dg, Dict("Request", "IORegistry", "EntryClass", "IOPMPowerSource"));
            object r = LdRecv(dg);
            string status = Get(r, "Status") as string;
            if (status != null && status != "Success")
                throw new Fail("診断のサービスが値を返しませんでした：" + status);
            var reg = Get(Get(r, "Diagnostics"), "IORegistry") as Dictionary<string, object>;
            if (reg == null) throw new Fail("電池の値が返ってきませんでした");
            ctx.Mark("読む");
            try
            {
                LdSend(dg, Dict("Request", "Goodbye"));
                LdRecv(dg);
            }
            catch (Exception) { }   // 値は読めているので、別れのあいさつの失敗は気にしない

            LastKeys = new List<string>(reg.Keys);
            object t = Get(reg, "Temperature");
            if (t is long) info.TemperatureC = (long)t / 100.0;
            else if (t is double) info.TemperatureC = (double)t / 100.0;
            info.Percent = PercentOf(reg);
            object c = Get(reg, "IsCharging");
            if (c is bool) info.Charging = (bool)c;
            if (info.TemperatureC == null) info.Error = "電池の温度の値がありませんでした";
        }

        // 今の iOS は CurrentCapacity が % で MaxCapacity が 100。古い形（mAh）なら割る
        static int? PercentOf(Dictionary<string, object> reg)
        {
            object cur = Get(reg, "CurrentCapacity"), max = Get(reg, "MaxCapacity");
            if (cur is long)
            {
                long c = (long)cur;
                if (max is long && (long)max > 0 && (long)max != 100)
                    return (int)Math.Round(c * 100.0 / (long)max);
                if (c >= 0 && c <= 100) return (int)c;
            }
            object rc = Get(reg, "AppleRawCurrentCapacity"), rm = Get(reg, "AppleRawMaxCapacity");
            if (rc is long && rm is long && (long)rm > 0)
                return (int)Math.Round((long)rc * 100.0 / (long)rm);
            object soc = Get(Get(reg, "BatteryData"), "StateOfCharge");
            if (soc is long) return (int)(long)soc;
            return null;
        }

        // ---- usbmuxd ----

        static Stream MuxOpen(Ctx ctx)
        {
            int port = MuxPort;
            if (!ctx.MuxChecked)
            {
                // 閉じた番号への接続は Windows がやり直すので 2 秒かかる。先に待ち受けの表を見て、無ければすぐ返す
                ctx.MuxChecked = true;
                if (!IsListening(port)) throw new Fail("Apple のサービスが実行されていません");
            }
            var s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            ctx.Track(s);
            s.NoDelay = true;
            s.ReceiveTimeout = s.SendTimeout = ctx.Remaining;
            try { s.Connect(IPAddress.Loopback, port); }
            catch (SocketException) { throw new Fail("Apple のサービスが実行されていません"); }
            return new NetworkStream(s, true);
        }

        static bool IsListening(int port)
        {
            try
            {
                foreach (IPEndPoint ep in IPGlobalProperties.GetIPGlobalProperties().GetActiveTcpListeners())
                    if (ep.Port == port && (ep.Address.Equals(IPAddress.Loopback) || ep.Address.Equals(IPAddress.Any)))
                        return true;
                return false;
            }
            catch (Exception) { return true; }   // 表が読めないときは、つないでみて決める
        }

        static Dictionary<string, object> MuxMsg(string kind)
        {
            return Dict("MessageType", kind, "ClientVersionString", "PocketGPU", "ProgName", "PocketGPU");
        }

        static object MuxRequest(Stream s, Dictionary<string, object> msg)
        {
            byte[] body = Plist.ToXml(msg);
            var head = new byte[16];
            PutLE(head, 0, 16 + body.Length);
            PutLE(head, 4, 1);    // 版 1（plist）
            PutLE(head, 8, 8);    // plist の手紙
            PutLE(head, 12, 1);   // 番号
            s.Write(head, 0, 16);
            s.Write(body, 0, body.Length);
            byte[] rh = ReadExact(s, 16);
            int len = rh[0] | rh[1] << 8 | rh[2] << 16 | rh[3] << 24;
            if (len < 16 || len > (16 << 20)) throw new Fail("Apple のサービスの応答が正しくありません");
            return Plist.Parse(ReadExact(s, len - 16));
        }

        // usbmuxd に iPhone のポートへの道を頼む。成功したらこの接続がそのまま USB の通り道になる
        static Stream MuxConnect(Ctx ctx, long deviceId, int port, string what)
        {
            Stream s = MuxOpen(ctx);
            var msg = MuxMsg("Connect");
            msg["DeviceID"] = deviceId;
            msg["PortNumber"] = (long)(((port & 0xff) << 8) | ((port >> 8) & 0xff));   // htons
            object r = MuxRequest(s, msg);
            object n = Get(r, "Number");
            if (!(n is long) || (long)n != 0)
            {
                s.Dispose();
                throw new Fail("iPhone の" + what + "に接続できませんでした：" + (n == null ? "応答なし" : n.ToString()));
            }
            return s;
        }

        // ---- lockdown と診断のサービス（4 バイトのビッグエンディアンの長さ＋plist）----

        static void LdSend(Stream s, Dictionary<string, object> msg)
        {
            byte[] body = Plist.ToXml(msg);
            var buf = new byte[4 + body.Length];
            buf[0] = (byte)(body.Length >> 24); buf[1] = (byte)(body.Length >> 16); buf[2] = (byte)(body.Length >> 8); buf[3] = (byte)body.Length;
            Buffer.BlockCopy(body, 0, buf, 4, body.Length);
            s.Write(buf, 0, buf.Length);
            s.Flush();
        }

        static object LdRecv(Stream s)
        {
            byte[] h = ReadExact(s, 4);
            int len = h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3];
            if (len <= 0 || len > (32 << 20)) throw new Fail("iPhone の応答の長さが正しくありません");
            return Plist.Parse(ReadExact(s, len));
        }

        // ---- TLS ----

        static Stream Tls(Ctx ctx, Stream inner, Pairing pr)
        {
            byte[] expect = TestPinMismatch ? pr.HostCert.GetRawCertData() : pr.DeviceCertDer;
            var ssl = new SslStream(inner, false,
                delegate(object sender, X509Certificate cert, X509Chain chain, SslPolicyErrors errors)
                {
                    // 自己署名で名前も合わないので、OS の判定は使わない。記録の DeviceCertificate と同じ物だけ通す
                    bool ok = cert != null && SameBytes(cert.GetRawCertData(), expect);
                    if (!ok) ctx.PinMismatch = true;
                    return ok;
                },
                delegate(object sender, string host, X509CertificateCollection local, X509Certificate remote, string[] issuers)
                {
                    return pr.HostCert;
                });
            var mine = new X509CertificateCollection();
            mine.Add(pr.HostCert);
            ssl.AuthenticateAsClient("iPhone", mine, TlsProtocols, false);
            return ssl;
        }

        static bool SameBytes(byte[] a, byte[] b)
        {
            if (a == null || b == null || a.Length != b.Length) return false;
            int diff = 0;
            for (int i = 0; i < a.Length; i++) diff |= a[i] ^ b[i];
            return diff == 0;
        }

        // ---- ペアリングの記録 ----

        sealed class Pairing : IDisposable
        {
            public string HostID, SystemBUID;
            public X509Certificate2 HostCert;
            public byte[] DeviceCertDer;
            RSACryptoServiceProvider key;

            public static Pairing Load(string udid)
            {
                if (udid.Length == 0 || udid.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || udid.Contains(".."))
                    throw new Fail("UDID の形が正しくありません");
                string path = Path.Combine(LockdownDir, udid + ".plist");
                if (!File.Exists(path)) throw new Fail("この iPhone のペアリングの記録がありません");
                byte[] raw;
                try { raw = File.ReadAllBytes(path); }
                catch (Exception) { throw new Fail("ペアリングの記録を読めません：アクセスが拒否されました"); }
                object rec;
                try { rec = Plist.Parse(raw); }
                catch (Exception) { throw new Fail("ペアリングの記録の形が正しくありません"); }
                var p = new Pairing();
                try
                {
                    p.HostID = Need<string>(rec, "HostID");
                    p.SystemBUID = Get(rec, "SystemBUID") as string ?? SystemBuidFromConfig();
                    if (p.SystemBUID == null) throw new Fail("ペアリングの記録に SystemBUID がありません");
                    string label;
                    byte[] certDer = Pem.Decode(Need<byte[]>(rec, "HostCertificate"), out label);
                    p.DeviceCertDer = Pem.Decode(Need<byte[]>(rec, "DeviceCertificate"), out label);
                    byte[] keyDer = Pem.Decode(Need<byte[]>(rec, "HostPrivateKey"), out label);
                    p.HostCert = new X509Certificate2(certDer);
                    p.key = Rsa.Load(label, keyDer);
                    Array.Clear(keyDer, 0, keyDer.Length);
                    p.HostCert.PrivateKey = p.key;
                    return p;
                }
                catch
                {
                    p.Dispose();
                    throw;
                }
            }

            static T Need<T>(object rec, string name) where T : class
            {
                T v = Get(rec, name) as T;
                if (v == null) throw new Fail("ペアリングの記録に " + name + " がありません");
                return v;
            }

            static string SystemBuidFromConfig()
            {
                try
                {
                    string p = Path.Combine(LockdownDir, "SystemConfiguration.plist");
                    return File.Exists(p) ? Get(Plist.Parse(File.ReadAllBytes(p)), "SystemBUID") as string : null;
                }
                catch (Exception) { return null; }
            }

            public void Dispose()
            {
                if (HostCert != null) { HostCert.Reset(); HostCert = null; }
                if (key != null)
                {
                    try { key.PersistKeyInCsp = false; } catch (Exception) { }
                    key.Dispose();   // PersistKeyInCsp が false なので、入れ物ごと消える
                    key = null;
                }
            }
        }

        static class Pem
        {
            public static byte[] Decode(byte[] pem, out string label)
            {
                string s = Encoding.ASCII.GetString(pem);
                int a = s.IndexOf("-----BEGIN ", StringComparison.Ordinal);
                if (a < 0) throw new Fail("ペアリングの記録の証明書の形が正しくありません");
                int la = a + 11, lb = s.IndexOf("-----", la, StringComparison.Ordinal);
                if (lb < 0) throw new Fail("ペアリングの記録の証明書の形が正しくありません");
                label = s.Substring(la, lb - la);
                int body = lb + 5, end = s.IndexOf("-----END ", body, StringComparison.Ordinal);
                if (end < 0) throw new Fail("ペアリングの記録の証明書の形が正しくありません");
                var sb = new StringBuilder(end - body);
                for (int i = body; i < end; i++) if (!char.IsWhiteSpace(s[i])) sb.Append(s[i]);
                return Convert.FromBase64String(sb.ToString());
            }
        }

        // PEM の RSA の秘密鍵（PKCS#1 の RSA PRIVATE KEY か、PKCS#8 の PRIVATE KEY）を ASN.1 から読んで CSP の鍵にする
        static class Rsa
        {
            const string RsaOid = "1.2.840.113549.1.1.1";

            public static RSACryptoServiceProvider Load(string label, byte[] der)
            {
                byte[] pkcs1;
                if (label == "RSA PRIVATE KEY") pkcs1 = der;
                else if (label == "PRIVATE KEY") pkcs1 = FromPkcs8(der);
                else throw new Fail("ペアリングの記録の鍵の形に対応していません：" + label);

                var seq = new Der(pkcs1, 0, pkcs1.Length).Sequence();
                seq.Integer();   // 版
                byte[] n = Trim(seq.Integer()), e = Trim(seq.Integer()), d = seq.Integer(), p = seq.Integer(), q = seq.Integer(),
                       dp = seq.Integer(), dq = seq.Integer(), qi = seq.Integer();
                int half = (n.Length + 1) / 2;
                var rp = new RSAParameters
                {
                    Modulus = n, Exponent = e, D = Pad(d, n.Length),
                    P = Pad(p, half), Q = Pad(q, half), DP = Pad(dp, half), DQ = Pad(dq, half), InverseQ = Pad(qi, half)
                };
                foreach (byte[] x in new[] { d, p, q, dp, dq, qi }) Array.Clear(x, 0, x.Length);

                // SChannel は鍵を入れ物の名前で開くので、名前の付いた入れ物に入れる。TLS 1.2 の SHA-256 の署名のため AES の CSP（型 24）
                var cp = new CspParameters(24, "Microsoft Enhanced RSA and AES Cryptographic Provider", "PocketGPU-battery-" + Guid.NewGuid().ToString("N"));
                cp.KeyNumber = (int)KeyNumber.Exchange;
                cp.Flags = CspProviderFlags.NoPrompt;
                var rsa = new RSACryptoServiceProvider(cp);
                try
                {
                    rsa.PersistKeyInCsp = false;   // Dispose で入れ物ごと消す
                    rsa.ImportParameters(rp);
                }
                catch
                {
                    rsa.Dispose();
                    throw;
                }
                finally
                {
                    foreach (byte[] x in new[] { rp.D, rp.P, rp.Q, rp.DP, rp.DQ, rp.InverseQ }) Array.Clear(x, 0, x.Length);
                }
                return rsa;
            }

            static byte[] FromPkcs8(byte[] der)
            {
                var seq = new Der(der, 0, der.Length).Sequence();
                seq.Integer();   // 版
                var alg = seq.Sequence();
                if (alg.Oid() != RsaOid) throw new Fail("ペアリングの記録の鍵が RSA ではありません");
                return seq.Octets();
            }

            static byte[] Trim(byte[] x)
            {
                int i = 0;
                while (i < x.Length - 1 && x[i] == 0) i++;
                if (i == 0) return x;
                var r = new byte[x.Length - i];
                Buffer.BlockCopy(x, i, r, 0, r.Length);
                return r;
            }

            static byte[] Pad(byte[] x, int len)
            {
                int i = 0;
                while (i < x.Length - 1 && x[i] == 0) i++;
                int n = x.Length - i;
                if (n > len) throw new Fail("ペアリングの記録の鍵の形が正しくありません");
                var r = new byte[len];
                Buffer.BlockCopy(x, i, r, len - n, n);
                return r;
            }
        }

        // DER の読み手（使う所だけ）
        sealed class Der
        {
            readonly byte[] b;
            int pos;
            readonly int end;

            public Der(byte[] b, int start, int end) { this.b = b; pos = start; this.end = end; }

            int Header(int want, out int len)
            {
                if (pos + 2 > end) throw new Fail("鍵の形が正しくありません");
                int tag = b[pos++];
                int l = b[pos++];
                if ((l & 0x80) != 0)
                {
                    int k = l & 0x7f;
                    if (k < 1 || k > 3 || pos + k > end) throw new Fail("鍵の形が正しくありません");
                    l = 0;
                    for (int i = 0; i < k; i++) l = (l << 8) | b[pos++];
                }
                if (tag != want || l < 0 || pos + l > end) throw new Fail("鍵の形が正しくありません");
                len = l;
                return pos;
            }

            byte[] Take(int tag)
            {
                int len, at = Header(tag, out len);
                var r = new byte[len];
                Buffer.BlockCopy(b, at, r, 0, len);
                pos += len;
                return r;
            }

            public Der Sequence()
            {
                int len, at = Header(0x30, out len);
                pos += len;
                return new Der(b, at, at + len);
            }

            public byte[] Integer() { return Take(0x02); }
            public byte[] Octets() { return Take(0x04); }

            public string Oid()
            {
                byte[] o = Take(0x06);
                if (o.Length == 0) return "";
                var sb = new StringBuilder();
                sb.Append(o[0] / 40).Append('.').Append(o[0] % 40);
                long v = 0;
                for (int i = 1; i < o.Length; i++)
                {
                    v = (v << 7) | (uint)(o[i] & 0x7f);
                    if ((o[i] & 0x80) == 0) { sb.Append('.').Append(v); v = 0; }
                }
                return sb.ToString();
            }
        }

        // ---- plist（XML で書き、XML かバイナリを読む）----
        // 型：Dictionary<string,object>・List<object>・string・long・double・bool・byte[]・DateTime・null

        static class Plist
        {
            public static byte[] ToXml(object o)
            {
                var sb = new StringBuilder();
                sb.Append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
                sb.Append("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n");
                sb.Append("<plist version=\"1.0\">\n");
                Write(sb, o);
                sb.Append("</plist>\n");
                return new UTF8Encoding(false).GetBytes(sb.ToString());
            }

            static void Write(StringBuilder sb, object o)
            {
                if (o is string) sb.Append("<string>").Append(Esc((string)o)).Append("</string>\n");
                else if (o is bool) sb.Append((bool)o ? "<true/>\n" : "<false/>\n");
                else if (o is long || o is int) sb.Append("<integer>").Append(Convert.ToInt64(o).ToString(CultureInfo.InvariantCulture)).Append("</integer>\n");
                else if (o is double) sb.Append("<real>").Append(((double)o).ToString("R", CultureInfo.InvariantCulture)).Append("</real>\n");
                else if (o is byte[]) sb.Append("<data>").Append(Convert.ToBase64String((byte[])o)).Append("</data>\n");
                else if (o is Dictionary<string, object>)
                {
                    sb.Append("<dict>\n");
                    foreach (var kv in (Dictionary<string, object>)o)
                    {
                        sb.Append("<key>").Append(Esc(kv.Key)).Append("</key>\n");
                        Write(sb, kv.Value);
                    }
                    sb.Append("</dict>\n");
                }
                else if (o is List<object>)
                {
                    sb.Append("<array>\n");
                    foreach (object x in (List<object>)o) Write(sb, x);
                    sb.Append("</array>\n");
                }
                else throw new ArgumentException("plist に書けない型");
            }

            static string Esc(string s)
            {
                return s.Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;");
            }

            public static object Parse(byte[] b)
            {
                if (b.Length >= 8 && b[0] == 'b' && b[1] == 'p' && b[2] == 'l' && b[3] == 'i' && b[4] == 's' && b[5] == 't')
                    return new Binary(b).Top();
                var set = new XmlReaderSettings();
                set.DtdProcessing = DtdProcessing.Ignore;
                set.XmlResolver = null;
                set.IgnoreComments = true;
                set.IgnoreProcessingInstructions = true;
                var doc = new XmlDocument();
                doc.XmlResolver = null;
                using (var r = XmlReader.Create(new MemoryStream(b, 0, TrimNul(b)), set)) doc.Load(r);
                XmlElement root = doc.DocumentElement;
                if (root == null || root.Name != "plist") throw new FormatException("plist ではない");
                foreach (XmlNode n in root.ChildNodes)
                    if (n is XmlElement) return FromXml((XmlElement)n, 0);
                return null;
            }

            static int TrimNul(byte[] b)
            {
                int n = b.Length;
                while (n > 0 && b[n - 1] == 0) n--;
                return n;
            }

            static object FromXml(XmlElement e, int depth)
            {
                if (depth > 64) throw new FormatException("plist が深すぎる");
                switch (e.Name)
                {
                    case "dict":
                        {
                            var d = new Dictionary<string, object>();
                            string key = null;
                            foreach (XmlNode n in e.ChildNodes)
                            {
                                var c = n as XmlElement;
                                if (c == null) continue;
                                if (c.Name == "key") key = c.InnerText;
                                else if (key != null) { d[key] = FromXml(c, depth + 1); key = null; }
                            }
                            return d;
                        }
                    case "array":
                        {
                            var l = new List<object>();
                            foreach (XmlNode n in e.ChildNodes)
                                if (n is XmlElement) l.Add(FromXml((XmlElement)n, depth + 1));
                            return l;
                        }
                    case "string": return e.InnerText;
                    case "integer":
                        {
                            string t = e.InnerText.Trim();
                            long v; ulong u; double f;
                            if (long.TryParse(t, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out v)) return v;
                            if (ulong.TryParse(t, NumberStyles.None, CultureInfo.InvariantCulture, out u)) return unchecked((long)u);   // 2^64 で回った負の数
                            if (double.TryParse(t, NumberStyles.Float, CultureInfo.InvariantCulture, out f)) return f;
                            return null;
                        }
                    case "real":
                        {
                            double f;
                            return double.TryParse(e.InnerText.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out f) ? (object)f : null;
                        }
                    case "true": return true;
                    case "false": return false;
                    case "data":
                        {
                            var sb = new StringBuilder();
                            foreach (char ch in e.InnerText) if (!char.IsWhiteSpace(ch)) sb.Append(ch);
                            return Convert.FromBase64String(sb.ToString());
                        }
                    case "date":
                        {
                            DateTime dt;
                            return DateTime.TryParse(e.InnerText.Trim(), CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out dt) ? (object)dt : null;
                        }
                    default: return null;
                }
            }

            // バイナリの plist（bplist00）
            sealed class Binary
            {
                readonly byte[] b;
                readonly int offSize, refSize;
                readonly long[] offsets;
                readonly long top;

                public Binary(byte[] b)
                {
                    this.b = b;
                    if (b.Length < 8 + 32) throw new FormatException("bplist が短い");
                    int t = b.Length - 32;
                    offSize = b[t + 6];
                    refSize = b[t + 7];
                    long num = BE(t + 8, 8);
                    top = BE(t + 16, 8);
                    long table = BE(t + 24, 8);
                    if (offSize < 1 || offSize > 8 || refSize < 1 || refSize > 8 || num < 1 || num > b.Length || top < 0 || top >= num
                        || table < 8 || table + num * offSize > t)
                        throw new FormatException("bplist の終わりが正しくない");
                    offsets = new long[num];
                    for (long i = 0; i < num; i++)
                    {
                        offsets[i] = BE((int)(table + i * offSize), offSize);
                        if (offsets[i] < 8 || offsets[i] >= table) throw new FormatException("bplist の位置が正しくない");
                    }
                }

                public object Top() { return Obj(top, 0); }

                long BE(long at, int n)
                {
                    if (at < 0 || at + n > b.Length) throw new FormatException("bplist の範囲の外");
                    long v = 0;
                    for (int i = 0; i < n; i++) v = (v << 8) | b[at + i];
                    return v;
                }

                int Count(int lo, ref long p)
                {
                    if (lo != 15) return lo;
                    if (p >= b.Length) throw new FormatException("bplist の範囲の外");
                    int m = b[p++];
                    if ((m >> 4) != 1) throw new FormatException("bplist の数が正しくない");
                    int n = 1 << (m & 15);
                    if (n > 8) throw new FormatException("bplist の数が大きすぎる");
                    long v = BE(p, n);
                    p += n;
                    if (v < 0 || v > b.Length) throw new FormatException("bplist の数が大きすぎる");
                    return (int)v;
                }

                byte[] Bytes(long p, long n)
                {
                    if (p < 0 || n < 0 || p + n > b.Length) throw new FormatException("bplist の範囲の外");
                    var r = new byte[n];
                    Buffer.BlockCopy(b, (int)p, r, 0, (int)n);
                    return r;
                }

                object Obj(long idx, int depth)
                {
                    if (depth > 64) throw new FormatException("bplist が深すぎる");
                    if (idx < 0 || idx >= offsets.Length) throw new FormatException("bplist の参照が正しくない");
                    long p = offsets[idx];
                    int m = b[p++];
                    int hi = m >> 4, lo = m & 15;
                    switch (hi)
                    {
                        case 0x0:
                            if (lo == 8) return false;
                            if (lo == 9) return true;
                            return null;
                        case 0x1:
                            {
                                int n = 1 << lo;
                                if (n == 16) return BE(p + 8, 8);   // 128 ビットは下の 64 ビット
                                if (n > 8) throw new FormatException("bplist の整数が大きすぎる");
                                return BE(p, n);   // 8 バイトは符号つき（シフトでそのまま負になる）
                            }
                        case 0x2:
                            {
                                int n = 1 << lo;
                                if (n == 4) { var f = Bytes(p, 4); Array.Reverse(f); return (double)BitConverter.ToSingle(f, 0); }
                                if (n == 8) { var f = Bytes(p, 8); Array.Reverse(f); return BitConverter.ToDouble(f, 0); }
                                throw new FormatException("bplist の実数の長さが正しくない");
                            }
                        case 0x3:
                            {
                                var f = Bytes(p, 8); Array.Reverse(f);
                                return new DateTime(2001, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddSeconds(BitConverter.ToDouble(f, 0));
                            }
                        case 0x4: { int n = Count(lo, ref p); return Bytes(p, n); }
                        case 0x5: { int n = Count(lo, ref p); return Encoding.ASCII.GetString(Bytes(p, n)); }
                        case 0x6: { int n = Count(lo, ref p); return Encoding.BigEndianUnicode.GetString(Bytes(p, (long)n * 2)); }
                        case 0x8: return BE(p, lo + 1);   // UID
                        case 0xA:
                        case 0xC:
                            {
                                int n = Count(lo, ref p);
                                var l = new List<object>(Math.Min(n, 4096));
                                for (int i = 0; i < n; i++) l.Add(Obj(BE(p + (long)i * refSize, refSize), depth + 1));
                                return l;
                            }
                        case 0xD:
                            {
                                int n = Count(lo, ref p);
                                var d = new Dictionary<string, object>();
                                for (int i = 0; i < n; i++)
                                {
                                    object k = Obj(BE(p + (long)i * refSize, refSize), depth + 1);
                                    object v = Obj(BE(p + ((long)n + i) * refSize, refSize), depth + 1);
                                    d[k as string ?? Convert.ToString(k, CultureInfo.InvariantCulture)] = v;
                                }
                                return d;
                            }
                        default: throw new FormatException("bplist の知らない型");
                    }
                }
            }
        }

        // ---- 小物 ----

        static object Get(object dict, string key)
        {
            var d = dict as Dictionary<string, object>;
            object v;
            return d != null && d.TryGetValue(key, out v) ? v : null;
        }

        static Dictionary<string, object> Dict(params object[] kv)
        {
            var d = new Dictionary<string, object>();
            for (int i = 0; i + 1 < kv.Length; i += 2) d[(string)kv[i]] = kv[i + 1];
            return d;
        }

        static void PutLE(byte[] b, int at, int v)
        {
            b[at] = (byte)v; b[at + 1] = (byte)(v >> 8); b[at + 2] = (byte)(v >> 16); b[at + 3] = (byte)(v >> 24);
        }

        static byte[] ReadExact(Stream s, int n)
        {
            var buf = new byte[n];
            int got = 0;
            while (got < n)
            {
                int k = s.Read(buf, got, n - got);
                if (k <= 0) throw new Fail("iPhone との接続が切れました");
                got += k;
            }
            return buf;
        }

        static string Short(Exception e)
        {
            while (e.InnerException != null) e = e.InnerException;
            string m = (e.Message ?? e.GetType().Name).Trim();
            int nl = m.IndexOfAny(new[] { '\r', '\n' });
            if (nl > 0) m = m.Substring(0, nl);
            return m.Length > 80 ? m.Substring(0, 80) + "…" : m;
        }

        sealed class Fail : Exception
        {
            public Fail(string m) : base(m) { }
        }

        // 1 回の Read の持ち物：締め切り・開いた接続・段の時間
        sealed class Ctx
        {
            readonly List<Socket> socks = new List<Socket>();
            readonly System.Diagnostics.Stopwatch sw = System.Diagnostics.Stopwatch.StartNew();
            readonly int limit;
            long lastMark;
            public volatile bool TimedOut;
            public volatile bool PinMismatch;
            public bool MuxChecked;
            public string Stage = "";
            public readonly StringBuilder Steps = new StringBuilder();

            public Ctx(int limit) { this.limit = limit; }

            public long Elapsed { get { return sw.ElapsedMilliseconds; } }
            public int Remaining { get { return (int)Math.Max(1, limit - sw.ElapsedMilliseconds); } }

            public void Mark(string name)
            {
                long now = sw.ElapsedMilliseconds;
                Steps.Append(name).Append(' ').Append(now - lastMark).Append("ms ");
                lastMark = now;
            }

            public void Track(Socket s)
            {
                lock (socks)
                {
                    if (TimedOut) { s.Close(); throw new Fail(TimeoutMsg); }
                    socks.Add(s);
                }
            }

            // 締め切りで全部の接続を閉じる。止まっている読み書きはそこで例外になる
            public void Expire()
            {
                lock (socks)
                {
                    TimedOut = true;
                    foreach (Socket s in socks) try { s.Close(); } catch (Exception) { }
                }
            }

            public void CloseAll()
            {
                lock (socks)
                {
                    foreach (Socket s in socks) try { s.Close(); } catch (Exception) { }
                    socks.Clear();
                }
            }
        }
    }
}
