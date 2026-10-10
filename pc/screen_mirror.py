"""PC の画面を、USB でつないだ iPhone のアプリ「計算の受け手」に映す。

決めたモニターか窓を GDI で撮り、JPEG に縮めて、usbmuxd 越しに iPhone の 50062 番へ送る。
先にアプリの「状態」で「PC の画面を映す」をオンにしておく（Sources/ScreenMirror.swift）。
送り方：[u32 長さ・リトルエンディアン][JPEG] の繰り返し。iPhone は最新の 1 枚だけを出す。
送るのが追いつかないときは、PC の側でも古いコマを捨てて最新だけを送る。
撮る・縮めて JPEG にする・送るは別々の糸で重ねる。フル HD を 1 枚撮る BitBlt に 15〜20 ms かかるので、
毎秒のコマ数はそこで止まる。10/10 にこの PC で測ったら、--fps 60 を頼んでも毎秒 47 コマだった。

  python screen_mirror.py                       # 主のモニターを、毎秒 30 コマまで
  python screen_mirror.py --list                # モニターの番号を出すだけ
  python screen_mirror.py --monitor 2 --fps 60 --quality 80 --max-width 1920
  python screen_mirror.py --window "メモ帳"      # 題名にこの文字を含む窓。上に重なった物も一緒に映る
  python screen_mirror.py --addr 127.0.0.1:50162 --seconds 30   # 試験：USB を使わず、この番地へ直接送る
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import io
import socket
import struct
import sys
import threading
import time

from PIL import Image

import usb_forward as uf

PORT = 50062
REPORT_EVERY = 5.0

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
dwmapi = ctypes.WinDLL("dwmapi")

SRCCOPY = 0x00CC0020
DI_NORMAL = 0x0003
CURSOR_SHOWING = 0x0001
MONITORINFOF_PRIMARY = 0x0001
DWMWA_EXTENDED_FRAME_BOUNDS = 9


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wt.DWORD * 3)]


class CURSORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("flags", wt.DWORD), ("hCursor", wt.HANDLE), ("ptScreenPos", wt.POINT)]


class ICONINFO(ctypes.Structure):
    _fields_ = [("fIcon", wt.BOOL), ("xHotspot", wt.DWORD), ("yHotspot", wt.DWORD),
                ("hbmMask", wt.HBITMAP), ("hbmColor", wt.HBITMAP)]


class MONITORINFOEXW(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", wt.RECT), ("rcWork", wt.RECT), ("dwFlags", wt.DWORD),
                ("szDevice", wt.WCHAR * 32)]


MONITORENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HMONITOR, wt.HDC, ctypes.POINTER(wt.RECT), wt.LPARAM)
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def _sig(dll, name, res, *args):
    f = getattr(dll, name)
    f.restype = res
    f.argtypes = args
    return f


# 64 ビットでは取っ手が 8 バイトなので、戻り値の型を決めておかないと上が切れる
_sig(user32, "GetDC", wt.HDC, wt.HWND)
_sig(user32, "ReleaseDC", ctypes.c_int, wt.HWND, wt.HDC)
_sig(gdi32, "CreateCompatibleDC", wt.HDC, wt.HDC)
_sig(gdi32, "DeleteDC", wt.BOOL, wt.HDC)
_sig(gdi32, "CreateDIBSection", wt.HBITMAP, wt.HDC, ctypes.POINTER(BITMAPINFO), wt.UINT,
     ctypes.POINTER(ctypes.c_void_p), wt.HANDLE, wt.DWORD)
_sig(gdi32, "SelectObject", wt.HGDIOBJ, wt.HDC, wt.HGDIOBJ)
_sig(gdi32, "DeleteObject", wt.BOOL, wt.HGDIOBJ)
_sig(gdi32, "BitBlt", wt.BOOL, wt.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
     wt.HDC, ctypes.c_int, ctypes.c_int, wt.DWORD)
_sig(gdi32, "GdiFlush", wt.BOOL)
_sig(user32, "GetCursorInfo", wt.BOOL, ctypes.POINTER(CURSORINFO))
_sig(user32, "GetIconInfo", wt.BOOL, wt.HANDLE, ctypes.POINTER(ICONINFO))
_sig(user32, "DrawIconEx", wt.BOOL, wt.HDC, ctypes.c_int, ctypes.c_int, wt.HANDLE, ctypes.c_int, ctypes.c_int,
     wt.UINT, wt.HBRUSH, wt.UINT)
_sig(user32, "EnumDisplayMonitors", wt.BOOL, wt.HDC, ctypes.POINTER(wt.RECT), MONITORENUMPROC, wt.LPARAM)
_sig(user32, "GetMonitorInfoW", wt.BOOL, wt.HMONITOR, ctypes.POINTER(MONITORINFOEXW))
_sig(user32, "EnumWindows", wt.BOOL, WNDENUMPROC, wt.LPARAM)
_sig(user32, "GetWindowTextLengthW", ctypes.c_int, wt.HWND)
_sig(user32, "GetWindowTextW", ctypes.c_int, wt.HWND, wt.LPWSTR, ctypes.c_int)
_sig(user32, "IsWindowVisible", wt.BOOL, wt.HWND)
_sig(user32, "IsWindow", wt.BOOL, wt.HWND)
_sig(user32, "IsIconic", wt.BOOL, wt.HWND)
_sig(user32, "GetWindowRect", wt.BOOL, wt.HWND, ctypes.POINTER(wt.RECT))
_sig(dwmapi, "DwmGetWindowAttribute", ctypes.c_long, wt.HWND, wt.DWORD, ctypes.c_void_p, wt.DWORD)


def dpi_aware():
    """画面の拡大率が 100% でなくても、本当の画素で撮る"""
    try:
        user32.SetProcessDpiAwarenessContext.restype = wt.BOOL
        user32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        if user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):  # PER_MONITOR_AWARE_V2
            return
    except AttributeError:
        pass
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except (AttributeError, OSError):
        user32.SetProcessDPIAware()


def monitors():
    """[{x, y, w, h, primary, name}]。並びは Windows が数えた順"""
    out = []

    def cb(hmon, hdc, rect, lparam):
        mi = MONITORINFOEXW()
        mi.cbSize = ctypes.sizeof(mi)
        if user32.GetMonitorInfoW(hmon, ctypes.byref(mi)):
            r = mi.rcMonitor
            out.append({"x": r.left, "y": r.top, "w": r.right - r.left, "h": r.bottom - r.top,
                        "primary": bool(mi.dwFlags & MONITORINFOF_PRIMARY), "name": mi.szDevice})
        return True

    user32.EnumDisplayMonitors(None, None, MONITORENUMPROC(cb), 0)
    return out


def find_windows(part):
    """題名に part を含む、見えている窓。上に出ている順。この道具を動かしている窓は除く"""
    low = part.lower()
    hits = []

    def cb(hwnd, lparam):
        if user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            if n:
                buf = ctypes.create_unicode_buffer(n + 1)
                user32.GetWindowTextW(hwnd, buf, n + 1)
                title = buf.value
                if low in title.lower() and "screen_mirror" not in title:
                    hits.append((hwnd, title))
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return hits


def window_rect(hwnd):
    """窓の見えている枠（影を除く）。しまわれているときは None。窓が閉じたら LookupError"""
    if not user32.IsWindow(hwnd):
        raise LookupError("窓が閉じた")
    if user32.IsIconic(hwnd):
        return None
    r = wt.RECT()
    if dwmapi.DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, ctypes.byref(r), ctypes.sizeof(r)) != 0:
        user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        return None
    return r.left, r.top, w, h


class Grabber:
    """画面の決めた四角を GDI の BitBlt で撮る。置き場は大きさが変わるまで使い回す"""

    def __init__(self, cursor=True):
        self.screen = user32.GetDC(None)
        self.mem = gdi32.CreateCompatibleDC(self.screen)
        if not self.screen or not self.mem:
            raise OSError("画面の DC を取れない")
        self.bmp = None
        self.old = None
        self.bits = ctypes.c_void_p()
        self.size = (0, 0)
        self.buf = None
        self.cursor = cursor
        self.hotspots = {}  # 矢印の形ごとの、指している点

    def _alloc(self, w, h):
        self._free_bmp()
        bi = BITMAPINFO()
        bh = bi.bmiHeader
        bh.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bh.biWidth = w
        bh.biHeight = -h  # 上から下へ並べる
        bh.biPlanes = 1
        bh.biBitCount = 32
        bh.biCompression = 0  # BI_RGB
        self.bmp = gdi32.CreateDIBSection(self.screen, ctypes.byref(bi), 0, ctypes.byref(self.bits), None, 0)
        if not self.bmp or not self.bits.value:
            self.bmp = None
            raise OSError(f"{w}x{h} の置き場を作れない")
        self.old = gdi32.SelectObject(self.mem, self.bmp)
        self.size = (w, h)
        self.buf = (ctypes.c_char * (w * h * 4)).from_address(self.bits.value)

    def _free_bmp(self):
        if self.bmp:
            gdi32.SelectObject(self.mem, self.old)
            gdi32.DeleteObject(self.bmp)
            self.bmp = None
            self.buf = None

    def _draw_cursor(self, x0, y0):
        ci = CURSORINFO()
        ci.cbSize = ctypes.sizeof(ci)
        if not user32.GetCursorInfo(ctypes.byref(ci)) or not (ci.flags & CURSOR_SHOWING) or not ci.hCursor:
            return
        hot = self.hotspots.get(ci.hCursor)
        if hot is None:
            ii = ICONINFO()
            if not user32.GetIconInfo(ci.hCursor, ctypes.byref(ii)):
                return
            hot = (ii.xHotspot, ii.yHotspot)
            for b in (ii.hbmMask, ii.hbmColor):
                if b:
                    gdi32.DeleteObject(b)
            self.hotspots[ci.hCursor] = hot
        user32.DrawIconEx(self.mem, ci.ptScreenPos.x - x0 - hot[0], ci.ptScreenPos.y - y0 - hot[1],
                          ci.hCursor, 0, 0, 0, None, DI_NORMAL)

    def grab(self, x, y, w, h):
        """(x, y) から w×h を撮って、RGB の Image で返す（写しなので、置き場はすぐ使い回せる）"""
        if (w, h) != self.size:
            self._alloc(w, h)
        if not gdi32.BitBlt(self.mem, 0, 0, w, h, self.screen, x, y, SRCCOPY):
            raise OSError(f"BitBlt が失敗（{ctypes.get_last_error()}）")
        if self.cursor:
            self._draw_cursor(x, y)
        gdi32.GdiFlush()  # GDI が書き終えてから読む
        return Image.frombuffer("RGB", (w, h), self.buf, "raw", "BGRX", 0, 1)

    def close(self):
        self._free_bmp()
        if self.mem:
            gdi32.DeleteDC(self.mem)
            self.mem = None
        if self.screen:
            user32.ReleaseDC(None, self.screen)
            self.screen = None


def shrink(img, max_width):
    if not max_width or img.width <= max_width:
        return img
    h = max(1, round(img.height * max_width / img.width))
    return img.resize((max_width, h), Image.Resampling.BILINEAR)


def to_jpeg(img, quality):
    bio = io.BytesIO()
    img.save(bio, "JPEG", quality=quality)
    return bio.getvalue()


class Latest(threading.Thread):
    """最新の 1 つだけを持ち、後ろの糸で work に渡す。work をしている間に次が来たら、古い方は捨てる。
    work は数の組を返し、それを足していく（かかった秒数・バイト数など）。
    撮る（BitBlt）・縮める・JPEG・送るはどれも GIL を外すので、糸を分けると重なって速くなる"""

    def __init__(self, work, n_sums):
        super().__init__(daemon=True)
        self.work = work
        self.cv = threading.Condition()
        self.slot = None
        self.stopping = False
        self.error = None
        self.done = 0
        self.dropped = 0
        self.sums = [0.0] * n_sums

    def put(self, item):
        with self.cv:
            if self.slot is not None:
                self.dropped += 1
            self.slot = item
            self.cv.notify()

    def stop(self):
        """持っている 1 つを片づけてから終わる"""
        with self.cv:
            self.stopping = True
            self.cv.notify()

    def run(self):
        while True:
            with self.cv:
                while self.slot is None and not self.stopping:
                    self.cv.wait()
                if self.slot is None:
                    return
                item, self.slot = self.slot, None
            try:
                r = self.work(item)
            except Exception as e:  # noqa: BLE001 止まった訳を前の糸に伝える
                self.error = e
                return
            with self.cv:
                self.done += 1
                for i, v in enumerate(r):
                    self.sums[i] += v

    def snapshot(self):
        with self.cv:
            return self.done, self.dropped, list(self.sums)


def open_link(a):
    """iPhone の 50062 番（--addr のときはその番地）へのつなぎ。つなげないときは訳を出して終わる"""
    if a.addr:
        host, port = a.addr.rsplit(":", 1)
        try:
            s = socket.create_connection((host, int(port)), timeout=5)
        except OSError as e:
            sys.exit(f"{a.addr} につながらない：{e}")
        where = a.addr
    else:
        if a.mux:
            uf.MUX = ("127.0.0.1", a.mux)
        try:
            dev = uf.usb_device_id()
        except SystemExit:
            sys.exit("USB に iPhone が見つからない")
        except OSError:
            sys.exit("USB に iPhone が見つからない：Apple Mobile Device Service（usbmuxd）につながらない")
        try:
            s = uf.connect_device(dev, a.port)
        except OSError as e:
            sys.exit(f"{e}。アプリの「状態」で「PC の画面を映す」をオンにする")
        where = f"USB の iPhone の {a.port} 番"
    s.settimeout(None)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s, where


def pick_target(a):
    """撮る四角を返す関数と、その名前"""
    if a.window:
        hits = find_windows(a.window)
        if not hits:
            sys.exit(f"題名に「{a.window}」を含む窓が無い")
        hwnd, title = hits[0]
        return (lambda: window_rect(hwnd)), f"窓「{title}」"
    mons = monitors()
    if not mons:
        sys.exit("モニターが見つからない")
    if a.monitor:
        if not 1 <= a.monitor <= len(mons):
            sys.exit(f"モニターは 1〜{len(mons)} 番。--list で見る")
        m = mons[a.monitor - 1]
        n = a.monitor
    else:
        n, m = next(((i + 1, m) for i, m in enumerate(mons) if m["primary"]), (1, mons[0]))
    rect = (m["x"], m["y"], m["w"], m["h"])
    return (lambda: rect), f"モニター {n}（{m['w']}x{m['h']}）"


def main():
    ap = argparse.ArgumentParser(description="PC の画面を USB の iPhone に映す")
    ap.add_argument("--monitor", type=int, help="何番のモニターか（--list で見る）。既定は主のモニター")
    ap.add_argument("--window", help="題名にこの文字を含む窓を撮る")
    ap.add_argument("--fps", type=float, default=30, help="毎秒のコマ数の上限")
    ap.add_argument("--quality", type=int, default=75, help="JPEG の品質 1〜95")
    ap.add_argument("--max-width", type=int, default=1280, help="これより広いときは、この幅に縮める。0 で縮めない")
    ap.add_argument("--no-cursor", action="store_true", help="マウスの矢印を描かない")
    ap.add_argument("--seconds", type=float, help="この秒数で止める。既定は Ctrl+C まで")
    ap.add_argument("--port", type=int, default=PORT, help="iPhone のアプリの番号")
    ap.add_argument("--addr", help="試験用：USB を使わず、この host:port へ直接つなぐ（偽物の受け口など）")
    ap.add_argument("--mux", type=int, help="試験用：usbmuxd の番号（偽物の usbmuxd に向ける）")
    ap.add_argument("--list", action="store_true", help="モニターの番号を出すだけ")
    a = ap.parse_args()
    if a.fps <= 0 or not 1 <= a.quality <= 95 or a.max_width < 0:
        sys.exit("--fps は 0 より大きく、--quality は 1〜95、--max-width は 0 以上")

    dpi_aware()
    if a.list:
        for i, m in enumerate(monitors(), 1):
            print(f"{i}: {m['w']}x{m['h']}  位置 ({m['x']}, {m['y']})  {m['name']}{'  主' if m['primary'] else ''}")
        return

    target, what = pick_target(a)
    sock, where = open_link(a)
    grabber = Grabber(cursor=not a.no_cursor)
    out_size = [0, 0]

    # 3 本の糸：撮る（ここ）→ 縮めて JPEG にする → 送る。間ではどれも最新の 1 つだけを渡す
    def send(item):
        t_shot, data = item
        t0 = time.perf_counter()
        sock.sendall(struct.pack("<I", len(data)) + data)
        t1 = time.perf_counter()
        return t1 - t0, len(data), t1 - t_shot  # 送った秒・バイト・撮り始めてから送り終えるまで

    def encode(item):
        t_shot, img = item
        t0 = time.perf_counter()
        small = shrink(img, a.max_width)
        t1 = time.perf_counter()
        data = to_jpeg(small, a.quality)
        t2 = time.perf_counter()
        out_size[:] = small.size
        sender.put((t_shot, data))
        return t1 - t0, t2 - t1

    sender = Latest(send, 3)
    encoder = Latest(encode, 2)
    sender.start()
    encoder.start()
    print(f"{what} を {where} へ。毎秒 {a.fps:g} コマまで・品質 {a.quality}・幅 {a.max_width or '縮めない'}", flush=True)

    interval = 1.0 / a.fps
    t_start = time.perf_counter()
    next_t = t_start
    last_report = t_start
    shot = total_shot = 0
    grab_s = 0.0
    prev_e = encoder.snapshot()
    prev_s = sender.snapshot()
    reason = "止めた"
    try:
        while True:
            now = time.perf_counter()
            if a.seconds and now - t_start >= a.seconds:
                reason = f"{a.seconds:g} 秒たった"
                break
            if sender.error:
                reason = f"iPhone 側が切った：{sender.error}"
                break
            if encoder.error:
                reason = f"JPEG にできない：{encoder.error}"
                break
            try:
                rect = target()
            except LookupError as e:
                reason = str(e)
                break
            if rect:
                t0 = time.perf_counter()
                img = grabber.grab(*rect)
                grab_s += time.perf_counter() - t0
                encoder.put((t0, img))
                shot += 1
                total_shot += 1
            now = time.perf_counter()
            if now - last_report >= REPORT_EVERY:
                e = encoder.snapshot()
                s = sender.snapshot()
                dropped = (e[1] - prev_e[1]) + (s[1] - prev_s[1])
                shrink_s, jpeg_s = (c - p for c, p in zip(e[2], prev_e[2]))
                send_s, nbytes, lat_s = (c - p for c, p in zip(s[2], prev_s[2]))
                encoded, sent = e[0] - prev_e[0], s[0] - prev_s[0]
                dt = now - last_report
                ke, ks = max(encoded, 1), max(sent, 1)
                print(f"{dt:4.1f} 秒：撮った {shot}・送った {sent}・捨てた {dropped}・毎秒 {sent / dt:5.1f} コマ・"
                      f"1 コマ 撮る {grab_s / max(shot, 1) * 1000:5.1f} ms・縮める {shrink_s / ke * 1000:4.1f} ms・"
                      f"JPEG {jpeg_s / ke * 1000:4.1f} ms・送る {send_s / ks * 1000:4.1f} ms・"
                      f"撮ってから送り終えるまで {lat_s / ks * 1000:5.1f} ms・"
                      f"平均 {nbytes / ks / 1024:6.1f} KB・{nbytes / dt / 1e6:5.2f} MB/s・"
                      f"{out_size[0]}x{out_size[1]}", flush=True)
                prev_e, prev_s = e, s
                last_report = now
                shot = 0
                grab_s = 0.0
            # 決めた間隔で回す。遅れたときは追いつこうとせず、そこから数え直す
            next_t += interval
            now = time.perf_counter()
            if next_t > now:
                time.sleep(next_t - now)
            else:
                next_t = now
    except KeyboardInterrupt:
        pass
    finally:
        encoder.stop()
        encoder.join(timeout=5)
        sender.stop()
        sender.join(timeout=5)
        grabber.close()
        try:
            sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        sock.close()
    total = time.perf_counter() - t_start
    e = encoder.snapshot()
    s = sender.snapshot()
    ks = max(s[0], 1)
    print(f"終わり（{reason}）：{total:.1f} 秒で 撮った {total_shot}・送った {s[0]}・捨てた {e[1] + s[1]}・"
          f"毎秒 {s[0] / total:.1f} コマ・平均 {s[2][1] / ks / 1024:.1f} KB・"
          f"撮ってから送り終えるまで 平均 {s[2][2] / ks * 1000:.1f} ms", flush=True)
    if sender.error:
        sys.exit(1)


if __name__ == "__main__":
    main()
