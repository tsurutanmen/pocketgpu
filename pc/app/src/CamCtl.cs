// PC から iPhone のカメラを操作する口：webcam_rx の名前付きパイプ \\.\pipe\pocketgpu-camctl（webcam_rx がサーバー、ここがつなぐ側）。
// 中身は 1 行 1 つの UTF-8 の JSON（決まりは work/camctl/PROTOCOL.md の操作の番号と、係どうしで決めた形）。
//   こちら → webcam_rx：{"cmd":"get_caps"}・{"cmd":"get_state"}・{"cmd":"set","id":番号,"value":数}
//   webcam_rx → こちら：status・caps・state・event・error（type で見分ける）。つないだ直後に status・caps・state が来る
// ・webcam_rx を起動した直後はパイプがまだ無いので、0.5 秒ごとに試し直す（30 回をすぎたら 2 秒ごと）。切れたら、またつなぐ。
// ・読むのも書くのも裏の糸。届いた物は UI の糸に戻して Message で知らせる。送るのは UI の糸から Send（並べて裏の糸が書く）。
// 試験用：PGPU_TEST_CTL_PIPE=名前 で別の名前のパイプにつなぐ（本物の webcam_rx のパイプには触らない）。
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Threading;

namespace PocketGPU
{
    class CtlChoice
    {
        public double Value;
        public string Ja = "", En = "";
        public string Label { get { string s = L.Ja ? Ja : En; return string.IsNullOrEmpty(s) ? (string.IsNullOrEmpty(En) ? Ja : En) : s; } }
    }

    // caps の 1 項目
    class CtlItem
    {
        public int Id;
        public string Name = "", Kind = "", Unit = "";
        public double Min, Max = 1, Step;
        public bool Log;                                   // 対数で見せる（"log":true か "scale":"log"。ないときはズームと ISO）
        public readonly List<CtlChoice> Choices = new List<CtlChoice>();
        public readonly List<double> Stops = new List<double>(); // 倍率のボタン（"stops"。あれば）

        // 番号:値 → iPhone のアプリ（CameraBridge.swift）の選択肢の言葉。webcam_rx の表（wcam_ctl.h）と違う物だけ
        static readonly Dictionary<string, string[]> SameAsIphone = new Dictionary<string, string[]>
        {
            { "21:3", new[] { "顔に合わせる", "Follow the face" } },
        };

        public static CtlItem Parse(Dictionary<string, object> d)
        {
            var it = new CtlItem();
            it.Id = (int) CamCtl.Num(d, "id", -1);
            it.Name = CamCtl.Str(d, "name");
            it.Kind = CamCtl.Str(d, "kind").ToLowerInvariant();
            it.Unit = CamCtl.Str(d, "unit");
            it.Min = CamCtl.Num(d, "min", 0);
            it.Max = CamCtl.Num(d, "max", 1);
            it.Step = CamCtl.Num(d, "step", 0);
            object o;
            // 対数：JSON に書いてあればそれ。無ければズーム（10）と ISO（6）（webcam_rx の JSON は対数を書かない。wcam_ctl.h の表の説明と同じ）
            if (d.ContainsKey("log") || d.ContainsKey("scale")) it.Log = (d.TryGetValue("log", out o) && o is bool && (bool) o) || CamCtl.Str(d, "scale") == "log";
            else it.Log = it.Id == 6 || it.Id == 10;
            if (d.TryGetValue("choices", out o) && o is object[])
                foreach (object c in (object[]) o)
                {
                    var cd = c as Dictionary<string, object>;
                    if (cd == null) continue;
                    var ch = new CtlChoice { Value = CamCtl.Num(cd, "value", 0), Ja = CamCtl.Str(cd, "label_ja"), En = CamCtl.Str(cd, "label_en") };
                    // iPhone のアプリと言葉が違う選択肢は、iPhone のアプリの言葉にそろえる
                    string[] same;
                    if (SameAsIphone.TryGetValue(it.Id + ":" + CamCtl.N(ch.Value), out same)) { ch.Ja = same[0]; ch.En = same[1]; }
                    // range の choices は倍率のボタン（webcam_rx の JSON。ズームの 0.5×・1×・2× など）
                    if (it.Kind == "range") it.Stops.Add(ch.Value);
                    else it.Choices.Add(ch);
                }
            if (d.TryGetValue("stops", out o) && o is object[])
                foreach (object s in (object[]) o)
                {
                    double v;
                    if (CamCtl.TryNum(s, out v)) it.Stops.Add(v);
                }
            return it;
        }

        // 同じ caps か比べるための文字列
        public string Sig()
        {
            var sb = new StringBuilder();
            sb.Append(Id).Append('|').Append(Kind).Append('|').Append(Name).Append('|').Append(CamCtl.N(Min)).Append('|').Append(CamCtl.N(Max)).Append('|').Append(CamCtl.N(Step)).Append('|').Append(Unit).Append('|').Append(Log);
            foreach (var c in Choices) sb.Append('|').Append(CamCtl.N(c.Value)).Append('=').Append(c.Ja).Append('/').Append(c.En);
            foreach (var s in Stops) sb.Append("|s").Append(CamCtl.N(s));
            return sb.ToString();
        }
    }

    class CamCtl
    {
        public const string RealPipe = "pocketgpu-camctl";
        public static string PipeName
        {
            get
            {
                string t = Environment.GetEnvironmentVariable("PGPU_TEST_CTL_PIPE");
                return string.IsNullOrEmpty(t) ? RealPipe : t;
            }
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool WaitNamedPipe(string name, int timeoutMs);

        readonly Dispatcher ui;
        readonly JavaScriptSerializer js = new JavaScriptSerializer();
        public event Action<Dictionary<string, object>> Message;   // UI の糸
        public event Action<bool> ConnectedChanged;                // UI の糸

        readonly object lk = new object();
        volatile bool want;
        int gen;
        NamedPipeClientStream pipe;
        BlockingCollection<string> outq;

        // UI の糸から読む様子
        public bool Connected;          // パイプが接続中
        public bool Wanted { get { return want; } }
        public DateTime WantSince = DateTime.MinValue;
        public DateTime ConnectedAt = DateTime.MinValue;
        public DateTime LostAt = DateTime.MinValue;
        public int Connects;            // つながった回数（試験の記録用）
        public int SentCount;           // 送った行の数（試験の記録用）

        public CamCtl(Dispatcher ui) { this.ui = ui; }

        public void Start()
        {
            if (want) return;
            want = true;
            WantSince = DateTime.Now;
            int my = Interlocked.Increment(ref gen);
            var t = new Thread(() => Loop(my)) { IsBackground = true, Name = "camctl" };
            t.Start();
        }

        public void Stop()
        {
            if (!want) return;
            want = false;
            Interlocked.Increment(ref gen);
            ClosePipe();
            Connected = false;
        }

        void ClosePipe()
        {
            NamedPipeClientStream p;
            lock (lk) { p = pipe; pipe = null; if (outq != null) outq.CompleteAdding(); outq = null; }
            if (p != null) try { p.Dispose(); } catch { }
        }

        // 1 行送る。つながっていなければ捨てて false
        public bool Send(string json)
        {
            lock (lk)
            {
                if (outq == null || outq.IsAddingCompleted) return false;
                try { outq.Add(json); } catch { return false; }
            }
            SentCount++;
            return true;
        }

        public bool SendSet(int id, double value)
        {
            return Send("{\"cmd\":\"set\",\"id\":" + id.ToString(CultureInfo.InvariantCulture) + ",\"value\":" + N(value) + "}");
        }

        void Loop(int my)
        {
            int fails = 0;
            string full = @"\\.\pipe\" + PipeName;
            while (want && my == gen)
            {
                NamedPipeClientStream p = null;
                try
                {
                    // パイプが無いときは、すぐに false が返る（待たない）
                    if (!WaitNamedPipe(full, 300)) throw new IOException("no pipe");
                    p = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
                    p.Connect(1000);
                }
                catch
                {
                    if (p != null) try { p.Dispose(); } catch { }
                    fails++;
                    Sleep(my, fails < 30 ? 500 : 2000);
                    continue;
                }
                fails = 0;
                var q = new BlockingCollection<string>();
                lock (lk)
                {
                    if (!want || my != gen) { try { p.Dispose(); } catch { } break; }
                    pipe = p;
                    outq = q;
                }
                Program.Log("camctl: connected " + PipeName);
                ui.BeginInvoke(new Action(() =>
                {
                    if (my != gen) return;
                    Connected = true;
                    Connects++;
                    ConnectedAt = DateTime.Now;
                    if (ConnectedChanged != null) ConnectedChanged(true);
                }));
                var writer = new Thread(() => WriteLoop(p, q)) { IsBackground = true, Name = "camctl-w" };
                writer.Start();
                try
                {
                    var r = new StreamReader(p, new UTF8Encoding(false));
                    string line;
                    while (want && my == gen && (line = r.ReadLine()) != null)
                    {
                        line = line.Trim();
                        if (line.Length == 0) continue;
                        Dictionary<string, object> msg = null;
                        try { msg = js.DeserializeObject(line) as Dictionary<string, object>; }
                        catch (Exception ex) { Program.Log("camctl: bad line " + ex.Message + " " + (line.Length > 120 ? line.Substring(0, 120) : line)); }
                        if (msg == null) continue;
                        var m = msg;
                        ui.BeginInvoke(new Action(() => { if (my == gen && Message != null) Message(m); }));
                    }
                }
                catch (Exception ex) { if (want) Program.Log("camctl: read " + ex.Message); }
                lock (lk)
                {
                    if (pipe == p) { pipe = null; if (outq != null) outq.CompleteAdding(); outq = null; }
                }
                try { p.Dispose(); } catch { }
                Program.Log("camctl: disconnected");
                ui.BeginInvoke(new Action(() =>
                {
                    Connected = false;
                    LostAt = DateTime.Now;
                    if (ConnectedChanged != null) ConnectedChanged(false);
                }));
                Sleep(my, 500);
            }
        }

        void Sleep(int my, int ms)
        {
            for (int t = 0; t < ms && want && my == gen; t += 50) Thread.Sleep(50);
        }

        static void WriteLoop(NamedPipeClientStream p, BlockingCollection<string> q)
        {
            try
            {
                var w = new StreamWriter(p, new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" };
                foreach (string s in q.GetConsumingEnumerable()) w.WriteLine(s);
            }
            catch { }
        }

        // ---- JSON の値を読む小道具
        public static string Str(Dictionary<string, object> d, string k)
        {
            object o;
            return d != null && d.TryGetValue(k, out o) && o != null ? Convert.ToString(o, CultureInfo.InvariantCulture) : "";
        }
        public static double Num(Dictionary<string, object> d, string k, double def)
        {
            object o;
            double v;
            if (d != null && d.TryGetValue(k, out o) && TryNum(o, out v)) return v;
            return def;
        }
        public static bool? Bool(Dictionary<string, object> d, string k)
        {
            object o;
            if (d != null && d.TryGetValue(k, out o))
            {
                if (o is bool) return (bool) o;
                double v;
                if (TryNum(o, out v)) return v != 0;
            }
            return null;
        }
        public static bool TryNum(object o, out double v)
        {
            v = 0;
            if (o == null || o is bool) return false;
            if (o is string) return double.TryParse((string) o, NumberStyles.Float, CultureInfo.InvariantCulture, out v);
            try { v = Convert.ToDouble(o, CultureInfo.InvariantCulture); return !double.IsNaN(v) && !double.IsInfinity(v); } catch { return false; }
        }
        public static string N(double v) { return v.ToString("0.######", CultureInfo.InvariantCulture); }
    }
}
