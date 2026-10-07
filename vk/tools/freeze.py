"""プロセスを一時停止・再開する。iPhone で受け手のアプリが後ろに回ったときの「つながったまま返事が無い」を、PC の代役で真似する。

  python freeze.py stop <プロセス番号>
  python freeze.py go <プロセス番号>
"""
import ctypes
import sys

PROCESS_SUSPEND_RESUME = 0x0800


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("stop", "go"):
        print(__doc__)
        return 2
    pid = int(sys.argv[2])
    k32 = ctypes.windll.kernel32
    nt = ctypes.windll.ntdll
    h = k32.OpenProcess(PROCESS_SUSPEND_RESUME, False, pid)
    if not h:
        print("開けない", k32.GetLastError())
        return 1
    r = nt.NtSuspendProcess(h) if sys.argv[1] == "stop" else nt.NtResumeProcess(h)
    k32.CloseHandle(h)
    print("止めた" if sys.argv[1] == "stop" else "動かした", pid, "結果", r)
    return 0 if r == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
