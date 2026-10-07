"""プロセスの見えている窓を画像に撮る。ドライバが窓に直接描いた物（止まったときの帯など）を確かめる用。

  python grab_window.py <プロセス番号> <出す.png>
"""
import ctypes
import ctypes.wintypes as wt
import sys

from PIL import ImageGrab


def main():
    pid, out = int(sys.argv[1]), sys.argv[2]
    u32 = ctypes.windll.user32
    u32.SetProcessDPIAware()
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u32.IsWindowVisible(h):
            found.append(h)
        return True

    u32.EnumWindows(cb, 0)
    if not found:
        print("窓が見つからない")
        return 1
    r = wt.RECT()
    u32.GetWindowRect(found[0], ctypes.byref(r))
    ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom), all_screens=True).save(out)
    print("撮った", out, r.right - r.left, "x", r.bottom - r.top)
    return 0


if __name__ == "__main__":
    sys.exit(main())
