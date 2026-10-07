"""プロセスの窓を、最小化・元に戻す・大きさを変える。ゲームでよく起きる窓の操作で、ドライバが落ちないかを試す用。

  python poke_window.py <プロセス番号> min|restore|size <幅> <高さ>
"""
import ctypes
import ctypes.wintypes as wt
import sys

SW_MINIMIZE, SW_RESTORE = 6, 9
SWP_NOMOVE, SWP_NOZORDER, SWP_NOACTIVATE = 0x2, 0x4, 0x10


def find(pid):
    u32 = ctypes.windll.user32
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u32.IsWindowVisible(h):
            found.append(h)
        return True

    u32.EnumWindows(cb, 0)
    return found[0] if found else None


def main():
    pid, what = int(sys.argv[1]), sys.argv[2]
    u32 = ctypes.windll.user32
    h = find(pid)
    if not h:
        print("窓が見つからない")
        return 1
    if what == "min":
        u32.ShowWindow(h, SW_MINIMIZE)
    elif what == "restore":
        u32.ShowWindow(h, SW_RESTORE)
    elif what == "size":
        u32.SetWindowPos(h, None, 0, 0, int(sys.argv[3]), int(sys.argv[4]), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)
    print("した", what)
    return 0


if __name__ == "__main__":
    sys.exit(main())
