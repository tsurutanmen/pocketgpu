// 子プロセス（webcam_rx・ipaudio・livecap など）を起動し、出力を読み、止める。
// ・起動した子は「ジョブ」に入れる。ジョブは閉じると中のプロセスを全部止める設定なので、アプリが落ちても子は残らない。
// ・止めるときは、まず Ctrl+C を送って片づけさせ（livecap は議事録を書き終える・webcam_rx は共有メモリに終わりを書く）、
//   決めた時間で終わらなければ PID で止める。
// ・Want が立っている間に子が終わったら、3 秒あけて起動し直す（iPhone を抜いた・アプリを閉じた）。引数の誤り（終了コード 2）は直さない限り同じなので、やめる。
// ゲームはここに入れない（このアプリを閉じてもゲームは止めない）。
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Threading;

namespace PocketGPU
{
    static class Job
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateJobObject(IntPtr a, string name);
        [DllImport("kernel32.dll")] static extern bool SetInformationJobObject(IntPtr job, int cls, ref EXT info, int len);
        [DllImport("kernel32.dll")] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr proc);

        [StructLayout(LayoutKind.Sequential)]
        struct BASIC { public long PerProcessUserTimeLimit, PerJobUserTimeLimit; public uint LimitFlags; public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize; public uint ActiveProcessLimit; public UIntPtr Affinity; public uint PriorityClass, SchedulingClass; }
        [StructLayout(LayoutKind.Sequential)]
        struct IOC { public ulong a, b, c, d, e, f; }
        [StructLayout(LayoutKind.Sequential)]
        struct EXT { public BASIC Basic; public IOC Io; public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed; }

        static IntPtr handle;
        static readonly object lk = new object();

        public static void Add(Process p)
        {
            lock (lk)
            {
                if (handle == IntPtr.Zero)
                {
                    handle = CreateJobObject(IntPtr.Zero, null);
                    var info = new EXT();
                    info.Basic.LimitFlags = 0x2000; // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                    SetInformationJobObject(handle, 9, ref info, Marshal.SizeOf(typeof(EXT)));
                }
                try { AssignProcessToJobObject(handle, p.Handle); } catch { }
            }
        }
    }

    static class ConsoleCtrl
    {
        [DllImport("kernel32.dll")] static extern bool AttachConsole(uint pid);
        [DllImport("kernel32.dll")] static extern bool FreeConsole();
        [DllImport("kernel32.dll")] static extern bool SetConsoleCtrlHandler(IntPtr h, bool add);
        [DllImport("kernel32.dll")] static extern bool GenerateConsoleCtrlEvent(uint ev, uint group);
        static readonly object lk = new object();

        // 子のコンソールに入って Ctrl+C を送る。自分は無視する設定にしておく。終わるのを少し待ってから外す
        public static bool SendCtrlC(Process p, int waitMs)
        {
            lock (lk)
            {
                FreeConsole();
                if (!AttachConsole((uint) p.Id)) return false;
                SetConsoleCtrlHandler(IntPtr.Zero, true);
                bool ok = GenerateConsoleCtrlEvent(0, 0);
                try { p.WaitForExit(waitMs); } catch { }
                FreeConsole();
                SetConsoleCtrlHandler(IntPtr.Zero, false);
                return ok;
            }
        }
    }

    enum ToolState { Off, Starting, Running, Waiting, Missing, Failed }

    // 1 つの道具。UI の糸から使う。出力の行は Dispatcher に戻して Line で知らせる
    class ChildTool
    {
        public readonly string Name;
        readonly Func<ToolCmd> build;
        readonly Dispatcher ui;
        public int StopWaitMs = 2500;
        public bool AutoRestart = true;

        Process proc;
        DispatcherTimer restartTimer;
        public bool Want { get; private set; }
        public ToolState State { get; private set; }
        public string LastError = "";
        public string LastCommand = "";
        public DateTime StartedAt;
        public int Starts;
        public readonly List<string> Recent = new List<string>();
        public event Action<string> Line;
        public event Action StateChanged;

        public ChildTool(string name, Func<ToolCmd> build, Dispatcher ui)
        {
            Name = name;
            this.build = build;
            this.ui = ui;
        }

        public bool Running { get { try { return proc != null && !proc.HasExited; } catch { return false; } } }
        public int Pid { get { try { return proc != null ? proc.Id : 0; } catch { return 0; } } }

        void SetState(ToolState s) { State = s; if (StateChanged != null) StateChanged(); }

        public void Start()
        {
            Want = true;
            if (Running) return;
            Launch();
        }

        // 設定を変えたときに、動いていれば起動し直す
        public void Restart()
        {
            if (!Want) return;
            Process p = proc;
            proc = null;
            if (p == null) { Launch(); return; }
            SetState(ToolState.Starting);
            int ms = StopWaitMs;
            // 前の子が終わってから起動する（共有メモリの書き手が 2 つにならないように）
            ThreadPool.QueueUserWorkItem(_ =>
            {
                StopNow(p, ms);
                ui.BeginInvoke(new Action(() => { if (Want && !Running) Launch(); }));
            });
        }

        void Launch()
        {
            if (restartTimer != null) { restartTimer.Stop(); restartTimer = null; }
            ToolCmd c;
            try { c = build(); }
            catch (Exception ex) { LastError = ex.Message; SetState(ToolState.Failed); return; }
            if (c == null) { SetState(ToolState.Failed); return; }
            if (c.Exe == null) { LastError = Name + ".exe"; SetState(ToolState.Missing); Want = false; return; }
            var psi = new ProcessStartInfo(c.Exe, c.Line)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                RedirectStandardInput = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
                WorkingDirectory = c.WorkDir ?? System.IO.Path.GetDirectoryName(c.Exe)
            };
            foreach (var kv in c.Env) psi.EnvironmentVariables[kv.Key] = kv.Value;
            LastCommand = "\"" + c.Exe + "\" " + c.Line;
            Process p;
            try { p = Process.Start(psi); }
            catch (Exception ex) { LastError = ex.Message; SetState(ToolState.Failed); Want = false; return; }
            Job.Add(p);
            proc = p;
            Starts++;
            StartedAt = DateTime.Now;
            lock (Recent) Recent.Clear();
            AddLine("$ " + LastCommand);
            p.EnableRaisingEvents = true;
            p.OutputDataReceived += (s, e) => { if (e.Data != null) Post(e.Data); };
            p.ErrorDataReceived += (s, e) => { if (e.Data != null) Post(e.Data); };
            p.Exited += (s, e) => ui.BeginInvoke(new Action(() => OnExit(p)));
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            SetState(ToolState.Starting);
        }

        void Post(string line) { ui.BeginInvoke(new Action(() => AddLine(line))); }

        void AddLine(string line)
        {
            lock (Recent)
            {
                Recent.Add(line);
                if (Recent.Count > 300) Recent.RemoveRange(0, 100);
            }
            if (Line != null) Line(line);
        }

        // 止めたあとに届いた出力の行（livecap の残りの文など）で、状態を「動作中」に戻さない
        public void MarkRunning() { if (Want && State != ToolState.Running) SetState(ToolState.Running); }
        public void MarkWaiting() { if (Want && State != ToolState.Waiting) SetState(ToolState.Waiting); }

        void OnExit(Process p)
        {
            if (p != proc) return;
            int code = -1;
            try { code = p.ExitCode; } catch { }
            AddLine("[exit " + code + "]");
            proc = null;
            if (!Want) { SetState(ToolState.Off); return; }
            if (code == 2) { Want = false; LastError = LastLines(3); SetState(ToolState.Failed); return; }
            if (!AutoRestart) { Want = false; SetState(ToolState.Off); return; }
            SetState(ToolState.Waiting);
            restartTimer = new DispatcherTimer(DispatcherPriority.Background, ui) { Interval = TimeSpan.FromSeconds(3) };
            restartTimer.Tick += (s, e) => { restartTimer.Stop(); restartTimer = null; if (Want && !Running) Launch(); };
            restartTimer.Start();
        }

        public string LastLines(int n)
        {
            lock (Recent)
            {
                var sb = new StringBuilder();
                for (int i = Math.Max(0, Recent.Count - n); i < Recent.Count; i++)
                {
                    if (Recent[i].StartsWith("[exit ") || Recent[i].StartsWith("$ ")) continue;
                    if (sb.Length > 0) sb.Append(' ');
                    sb.Append(Recent[i]);
                }
                return sb.ToString();
            }
        }

        // wait が false なら、止める仕事は裏の糸でする（Ctrl+C のあと最大 StopWaitMs 待つので、画面を止めない）
        public void Stop(bool wait = false)
        {
            Want = false;
            if (restartTimer != null) { restartTimer.Stop(); restartTimer = null; }
            StopProcess(wait);
            SetState(ToolState.Off);
        }

        void StopProcess(bool wait = true)
        {
            Process p = proc;
            proc = null;
            if (p == null) return;
            int ms = StopWaitMs;
            if (wait) StopNow(p, ms);
            else ThreadPool.QueueUserWorkItem(_ => StopNow(p, ms));
        }

        public static void StopNow(Process p, int waitMs)
        {
            try
            {
                if (p.HasExited) return;
                ConsoleCtrl.SendCtrlC(p, waitMs);
                if (!p.HasExited) { p.Kill(); p.WaitForExit(2000); }
            }
            catch { }
        }

        // 1 回だけ走らせて、終わるまでの出力を返す（--list・status など）。UI の糸では呼ばない
        public static string RunOnce(ToolCmd c, int timeoutMs, out int exitCode)
        {
            exitCode = -1;
            if (c.Exe == null) return null;
            var psi = new ProcessStartInfo(c.Exe, c.Line)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
                WorkingDirectory = c.WorkDir ?? System.IO.Path.GetDirectoryName(c.Exe)
            };
            foreach (var kv in c.Env) psi.EnvironmentVariables[kv.Key] = kv.Value;
            try
            {
                using (var p = Process.Start(psi))
                {
                    Job.Add(p);
                    var o = p.StandardOutput.ReadToEndAsync();
                    var e = p.StandardError.ReadToEndAsync();
                    if (!p.WaitForExit(timeoutMs)) { try { p.Kill(); } catch { } return null; }
                    p.WaitForExit();
                    exitCode = p.ExitCode;
                    return o.Result + e.Result;
                }
            }
            catch { return null; }
        }
    }
}
