#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""iPhone の画面を Windows の窓に映し、窓のクリックを WebDriverAgent のタップにする試作。

前提
  WDA が iPhone で動いていて、次の二つが PC の localhost に出ていること。
    8100  WDA の HTTP API
    9100  WDA の MJPEG サーバー
  出し方は pymobiledevice3 なら次の二行。それぞれ別の端末で動かす。
    pymobiledevice3 usbmux forward 8100 8100
    pymobiledevice3 usbmux forward 9100 9100

必要なもの
  Python 3.9 以上と tkinter は標準のもので足りる。
  MJPEG は JPEG なので、映すには Pillow が要る。  python -m pip install pillow
  Pillow が無い時は --png を付ける。/screenshot の PNG を tkinter だけで映す。遅い。

操作
  左クリック       タップ
  左を押し続ける   長押し
  左ドラッグ       スワイプ
  右クリック       ホーム
  ホイール         縦スクロール
  H キー           ホーム

倍率について
  WDA のタップは画面の点で指す。MJPEG の画像はピクセルで、窓の大きさとも違う。
  そこで窓上の位置を、画像の中での割合にして、WDA の点の大きさに掛けている。
  点の大きさは GET /session/.../window/size から取る。取れない時は --points 402x874 のように渡す。
  横向きでは画像の縦横が入れ替わるので、画像の向きを見て点の縦横も入れ替える。
"""

from __future__ import annotations

import argparse
import base64
import http.client
import io
import json
import math
import queue
import re
import struct
import sys
import threading
import time
import urllib.error
import urllib.request

import tkinter as tk

try:
    from PIL import Image, ImageTk

    HAVE_PIL = True
    RESAMPLE = getattr(Image, "Resampling", Image).BILINEAR
except Exception:
    HAVE_PIL = False
    RESAMPLE = None


# ---------------------------------------------------------------------------
# 変換と組み立て。画面を使わないので、そのまま検算できる。
# ---------------------------------------------------------------------------

def device_size_for_frame(size_pt, frame_wh):
    """画像の向きに合わせて、点の縦横を入れ替える。"""
    w, h = size_pt
    fw, fh = frame_wh
    if (fw > fh) != (w > h):
        return h, w
    return w, h


def view_to_device(vx, vy, rect, size_pt, clamp=False):
    """窓上の位置を、デバイスの点にする。画像の外なら None。clamp なら縁に寄せる。"""
    ox, oy, dw, dh = rect
    if dw <= 0 or dh <= 0:
        return None
    fx = (vx - ox) / dw
    fy = (vy - oy) / dh
    if clamp:
        fx = min(max(fx, 0.0), 1.0)
        fy = min(max(fy, 0.0), 1.0)
    elif not (0.0 <= fx < 1.0 and 0.0 <= fy < 1.0):
        return None
    w, h = size_pt
    # 右端と下端ちょうどは画面の外になるので、少しだけ内側に止める。
    x = min(fx * w, w - 0.1)
    y = min(fy * h, h - 0.1)
    return round(x, 1), round(y, 1)


def _pointer(actions):
    return {
        "actions": [
            {
                "type": "pointer",
                "id": "finger1",
                "parameters": {"pointerType": "touch"},
                "actions": actions,
            }
        ]
    }


def build_tap(x, y, hold_ms):
    return _pointer(
        [
            {"type": "pointerMove", "duration": 0, "x": x, "y": y},
            {"type": "pointerDown", "button": 0},
            {"type": "pause", "duration": int(hold_ms)},
            {"type": "pointerUp", "button": 0},
        ]
    )


def build_swipe(x1, y1, x2, y2, duration_ms, pre_hold_ms=40):
    return _pointer(
        [
            {"type": "pointerMove", "duration": 0, "x": x1, "y": y1},
            {"type": "pointerDown", "button": 0},
            {"type": "pause", "duration": int(pre_hold_ms)},
            {"type": "pointerMove", "duration": int(duration_ms), "x": x2, "y": y2},
            {"type": "pointerUp", "button": 0},
        ]
    )


_CONTENT_LENGTH = re.compile(rb"content-length:\s*(\d+)", re.I)
_MAX_FRAME = 32 * 1024 * 1024


def iter_mjpeg_frames(read):
    """read は read(n) で bytes を返し、終わりで b'' を返す関数。JPEG を一枚ずつ出す。

    WDA は一枚ごとに次の形で送る。
      --BoundaryString CRLF Content-type: image/jpeg CRLF Content-Length: N CRLF CRLF 本体 CRLF CRLF
    境界の文字列には頼らず、Content-Length で切る。
    """
    buf = bytearray()
    while True:
        chunk = read(65536)
        if not chunk:
            return
        buf += chunk
        while True:
            end = buf.find(b"\r\n\r\n")
            if end < 0:
                if len(buf) > _MAX_FRAME:
                    buf.clear()
                break
            m = _CONTENT_LENGTH.search(bytes(buf[:end]))
            if not m:
                del buf[: end + 4]
                continue
            n = int(m.group(1))
            start = end + 4
            if n > _MAX_FRAME:
                del buf[:start]
                continue
            if len(buf) < start + n:
                break
            frame = bytes(buf[start : start + n])
            del buf[: start + n]
            yield frame


def jpeg_size(data):
    """JPEG の幅と高さを、デコードせずに読む。"""
    if data[:2] != b"\xff\xd8":
        return None
    i = 2
    n = len(data)
    while i + 9 < n:
        if data[i] != 0xFF:
            i += 1
            continue
        m = data[i + 1]
        if m == 0xFF:
            i += 1
            continue
        if m == 0x01 or 0xD0 <= m <= 0xD8:
            i += 2
            continue
        if 0xC0 <= m <= 0xCF and m not in (0xC4, 0xC8, 0xCC):
            h, w = struct.unpack(">HH", data[i + 5 : i + 9])
            return w, h
        seg = struct.unpack(">H", data[i + 2 : i + 4])[0]
        i += 2 + seg
    return None


def png_size(raw):
    if raw[:8] != b"\x89PNG\r\n\x1a\n" or len(raw) < 24:
        return None
    return struct.unpack(">II", raw[16:24])


# ---------------------------------------------------------------------------
# WDA の HTTP クライアント
# ---------------------------------------------------------------------------

class WdaError(Exception):
    def __init__(self, status, message):
        super().__init__(f"HTTP {status}: {message}" if status else message)
        self.status = status


class Wda:
    def __init__(self, host, port, timeout=20.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.session = None
        self.settings = {}
        self.lock = threading.RLock()
        # 環境変数のプロキシ設定を使わない。
        self._opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def call(self, method, path, body=None, timeout=None):
        url = f"http://{self.host}:{self.port}{path}"
        headers = {}
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with self._opener.open(req, timeout=timeout or self.timeout) as resp:
                raw = resp.read()
        except urllib.error.HTTPError as e:
            text = e.read()[:300].decode("utf-8", "replace")
            raise WdaError(e.code, text) from None
        except (urllib.error.URLError, OSError) as e:
            raise WdaError(0, str(getattr(e, "reason", e))) from None
        if not raw:
            return {}
        try:
            return json.loads(raw.decode("utf-8"))
        except ValueError:
            raise WdaError(0, "JSON ではない応答") from None

    def ensure_session(self, force_new=False):
        with self.lock:
            if self.session and not force_new:
                return self.session
            sid = None
            if not force_new:
                # WDA の持てるセッションは一つ。あればそれを使う。
                status = self.call("GET", "/status", timeout=10)
                sid = status.get("sessionId")
                if sid:
                    try:
                        self.call("GET", f"/session/{sid}/window/size", timeout=10)
                    except WdaError:
                        sid = None
            if not sid:
                res = self.call(
                    "POST", "/session", {"capabilities": {"alwaysMatch": {}}}, timeout=60
                )
                value = res.get("value") or {}
                sid = value.get("sessionId") or res.get("sessionId")
            if not sid:
                raise WdaError(0, "セッションを作れなかった")
            self.session = sid
            if self.settings:
                try:
                    self.call(
                        "POST", f"/session/{sid}/appium/settings", {"settings": self.settings}
                    )
                except WdaError:
                    pass
            return sid

    def session_call(self, method, sub, body=None, timeout=None):
        last = None
        for attempt in (0, 1):
            sid = self.ensure_session(force_new=(attempt == 1))
            try:
                return self.call(method, f"/session/{sid}{sub}", body, timeout)
            except WdaError as e:
                last = e
                if e.status not in (404, 410, 500):
                    raise
        raise last

    def window_size(self):
        value = self.session_call("GET", "/window/size").get("value") or {}
        return float(value["width"]), float(value["height"])


# ---------------------------------------------------------------------------
# 映像の取り込み
# ---------------------------------------------------------------------------

def put_latest(q, item):
    """古いものを捨てて、最新の一つだけ置く。"""
    try:
        q.put_nowait(item)
    except queue.Full:
        try:
            q.get_nowait()
        except queue.Empty:
            pass
        try:
            q.put_nowait(item)
        except queue.Full:
            pass


def mjpeg_loop(host, port, frame_q, say, stop):
    wait = 0.5
    while not stop.is_set():
        conn = None
        try:
            conn = http.client.HTTPConnection(host, port, timeout=10)
            conn.request("GET", "/")
            resp = conn.getresponse()
            say("video", "受信中")
            wait = 0.5
            for frame in iter_mjpeg_frames(resp.read1):
                if stop.is_set():
                    return
                put_latest(frame_q, ("jpeg", frame))
            say("video", "切れた。つなぎ直す")
        except Exception as e:
            say("video", f"つながらない {e}")
        finally:
            if conn is not None:
                try:
                    conn.close()
                except Exception:
                    pass
        stop.wait(wait)
        wait = min(wait * 2, 5.0)


def png_loop(wda, frame_q, say, stop, interval):
    warned = False
    while not stop.is_set():
        try:
            res = wda.call("GET", "/screenshot", timeout=20)
            b64 = res.get("value")
            raw = base64.b64decode(b64) if b64 else b""
            if png_size(raw) is None:
                if not warned:
                    warned = True
                    say("video", "PNG ではない。screenshotQuality を 0 にする")
                    try:
                        wda.settings["screenshotQuality"] = 0
                        sid = wda.ensure_session()
                        wda.call(
                            "POST", f"/session/{sid}/appium/settings",
                            {"settings": {"screenshotQuality": 0}},
                        )
                    except WdaError:
                        pass
                stop.wait(1.0)
                continue
            put_latest(frame_q, ("png", b64))
            say("video", "受信中")
        except Exception as e:
            say("video", f"取れない {e}")
            stop.wait(2.0)
        stop.wait(interval)


# ---------------------------------------------------------------------------
# 窓
# ---------------------------------------------------------------------------

class Viewer:
    TAP_SLOP_PX = 6
    LONG_PRESS_MS = 450

    def __init__(self, args):
        self.args = args
        self.wda = Wda(args.host, args.wda_port)
        settings = {}
        if args.fps:
            settings["mjpegServerFramerate"] = args.fps
        if args.quality:
            settings["mjpegServerScreenshotQuality"] = args.quality
        if args.mjpeg_scale:
            settings["mjpegScalingFactor"] = args.mjpeg_scale
        self.wda.settings = settings

        self.frame_q = queue.Queue(maxsize=1)
        self.msg_q = queue.Queue()
        self.act_q = queue.Queue()
        self.stop = threading.Event()

        self.base_size = args.points
        self.frame_wh = None
        self.rect = None
        self.press = None
        self.photo = None
        self.png_sub = None
        self.sized = False
        self.st = {"video": "待機", "wda": "接続待ち", "act": ""}
        self.shown = 0
        self.fps_mark = time.monotonic()
        self.fps = 0.0
        self._title = ""
        self.poll_id = None

        self.root = tk.Tk()
        self.root.title("WDA viewer")
        self.root.configure(bg="#111111")
        self.canvas = tk.Canvas(
            self.root, bg="#111111", highlightthickness=0, width=360, height=640,
            cursor="crosshair",
        )
        self.canvas.pack(fill="both", expand=True)
        self.item = self.canvas.create_image(0, 0, anchor="nw")
        self.canvas.bind("<ButtonPress-1>", self.on_down)
        self.canvas.bind("<B1-Motion>", self.on_move)
        self.canvas.bind("<ButtonRelease-1>", self.on_up)
        self.canvas.bind("<ButtonPress-3>", self.on_home)
        self.canvas.bind("<MouseWheel>", self.on_wheel)
        self.root.bind("<KeyPress-h>", self.on_home)
        self.root.bind("<KeyPress-H>", self.on_home)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    # 状態表示。他のスレッドから呼べる。
    def say(self, key, text):
        self.msg_q.put((key, text))

    def update_title(self):
        title = (
            f"WDA viewer  映像 {self.st['video']}"
            + (f" {self.fps:.0f}fps" if self.fps else "")
            + f"  WDA {self.st['wda']}"
            + (f"  {self.st['act']}" if self.st["act"] else "")
        )
        if title != self._title:
            self._title = title
            self.root.title(title)

    # 描画
    def poll(self):
        if self.stop.is_set():
            return
        try:
            while True:
                key, text = self.msg_q.get_nowait()
                self.st[key] = text
        except queue.Empty:
            pass
        try:
            kind, data = self.frame_q.get_nowait()
        except queue.Empty:
            pass
        else:
            try:
                self.show(kind, data)
                self.shown += 1
            except Exception as e:
                self.st["video"] = f"表示に失敗 {e}"
        now = time.monotonic()
        if now - self.fps_mark >= 1.0:
            self.fps = self.shown / (now - self.fps_mark)
            self.shown = 0
            self.fps_mark = now
        self.update_title()
        self.poll_id = self.root.after(15, self.poll)

    def max_height(self):
        return self.args.max_height or int(self.root.winfo_screenheight() * 0.85)

    def first_size(self, width, height):
        self.root.geometry(f"{max(math.ceil(width), 100)}x{max(math.ceil(height), 100)}")
        self.root.update_idletasks()
        self.sized = True

    def show(self, kind, data):
        if kind == "jpeg":
            img = Image.open(io.BytesIO(data))
            fw, fh = img.size
            if not self.sized:
                scale = min(1.0, self.max_height() / fh)
                self.first_size(fw * scale, fh * scale)
            cw = max(self.canvas.winfo_width(), 2)
            ch = max(self.canvas.winfo_height(), 2)
            scale = min(cw / fw, ch / fh)
            dw = max(int(fw * scale), 1)
            dh = max(int(fh * scale), 1)
            img.draft("RGB", (dw, dh))
            img = img.convert("RGB").resize((dw, dh), RESAMPLE)
            photo = ImageTk.PhotoImage(img)
        else:
            photo = tk.PhotoImage(data=data)
            fw, fh = photo.width(), photo.height()
            if self.png_sub is None:
                self.png_sub = max(1, math.ceil(fh / self.max_height()))
            if not self.sized:
                self.first_size(fw / self.png_sub, fh / self.png_sub)
            if self.png_sub > 1:
                photo = photo.subsample(self.png_sub)
            cw = max(self.canvas.winfo_width(), 2)
            ch = max(self.canvas.winfo_height(), 2)
            dw, dh = photo.width(), photo.height()
        ox = (cw - dw) // 2
        oy = (ch - dh) // 2
        self.canvas.coords(self.item, ox, oy)
        self.canvas.itemconfig(self.item, image=photo)
        self.photo = photo
        self.rect = (ox, oy, dw, dh)
        self.frame_wh = (fw, fh)

    # 座標の変換
    def current_size(self):
        if self.base_size is None or self.frame_wh is None:
            return None
        return device_size_for_frame(self.base_size, self.frame_wh)

    def to_device(self, vx, vy, clamp=False):
        size = self.current_size()
        if size is None or self.rect is None:
            return None
        return view_to_device(vx, vy, self.rect, size, clamp)

    # 見た目の手がかり
    def flash(self, x, y, r=14):
        oid = self.canvas.create_oval(
            x - r, y - r, x + r, y + r, outline="#ffffff", width=2, tags="fx"
        )
        self.root.after(250, lambda: self.canvas.delete(oid) if not self.stop.is_set() else None)

    # マウス
    def on_down(self, e):
        self.press = (e.x, e.y, time.monotonic())
        self.canvas.delete("trail")

    def on_move(self, e):
        if self.press is None:
            return
        x0, y0, _ = self.press
        self.canvas.delete("trail")
        self.canvas.create_line(x0, y0, e.x, e.y, fill="#ffffff", width=2, tags=("fx", "trail"))

    def on_up(self, e):
        if self.press is None:
            return
        x0, y0, t0 = self.press
        self.press = None
        self.canvas.delete("trail")
        dt_ms = int((time.monotonic() - t0) * 1000)
        start = self.to_device(x0, y0)
        if start is None:
            self.st["act"] = "画像の外か、サイズ未取得"
            return
        if math.hypot(e.x - x0, e.y - y0) < self.TAP_SLOP_PX:
            hold = self.args.tap_hold_ms
            if dt_ms >= self.LONG_PRESS_MS:
                hold = min(dt_ms, 3000)
            self.flash(x0, y0)
            self.act_q.put(("tap", start[0], start[1], hold))
        else:
            end = self.to_device(e.x, e.y, clamp=True)
            self.act_q.put(
                ("swipe", start[0], start[1], end[0], end[1], min(max(dt_ms, 60), 2000))
            )

    def on_home(self, _event=None):
        self.act_q.put(("home",))

    def on_wheel(self, e):
        if self.act_q.qsize() > 1:
            return
        p = self.to_device(e.x, e.y)
        size = self.current_size()
        if p is None or size is None:
            return
        half = size[1] * 0.12
        sign = 1 if e.delta > 0 else -1
        y1 = min(max(p[1] - sign * half, 1.0), size[1] - 1.0)
        y2 = min(max(p[1] + sign * half, 1.0), size[1] - 1.0)
        self.act_q.put(("swipe", p[0], round(y1, 1), p[0], round(y2, 1), 250))

    # 別スレッド
    def init_loop(self):
        while not self.stop.is_set():
            try:
                self.wda.ensure_session()
                if self.base_size is None:
                    self.base_size = self.wda.window_size()
                self.say("wda", f"接続済み {self.base_size[0]:.0f}x{self.base_size[1]:.0f}pt")
                return
            except Exception as e:
                self.say("wda", f"つながらない {e}")
                self.stop.wait(3.0)

    def action_loop(self):
        while True:
            item = self.act_q.get()
            if item is None or self.stop.is_set():
                return
            try:
                kind = item[0]
                if kind == "tap":
                    _, x, y, hold = item
                    self.wda.session_call("POST", "/actions", build_tap(x, y, hold))
                    self.say("act", f"tap {x:.0f},{y:.0f}")
                elif kind == "swipe":
                    _, x1, y1, x2, y2, dur = item
                    self.wda.session_call("POST", "/actions", build_swipe(x1, y1, x2, y2, dur))
                    self.say("act", f"swipe {x1:.0f},{y1:.0f} to {x2:.0f},{y2:.0f}")
                elif kind == "home":
                    self.wda.call("POST", "/wda/homescreen", {})
                    self.say("act", "home")
            except Exception as e:
                self.say("act", f"操作に失敗 {e}")

    def run(self):
        threading.Thread(target=self.init_loop, daemon=True).start()
        threading.Thread(target=self.action_loop, daemon=True).start()
        if self.args.png:
            target = png_loop
            targs = (self.wda, self.frame_q, self.say, self.stop, self.args.png_interval)
        else:
            target = mjpeg_loop
            targs = (self.args.host, self.args.mjpeg_port, self.frame_q, self.say, self.stop)
        threading.Thread(target=target, args=targs, daemon=True).start()
        self.poll_id = self.root.after(15, self.poll)
        self.root.mainloop()

    def close(self):
        self.stop.set()
        self.act_q.put(None)
        if self.poll_id is not None:
            try:
                self.root.after_cancel(self.poll_id)
            except tk.TclError:
                pass
        self.root.destroy()


# ---------------------------------------------------------------------------
# つながるかだけを見る
# ---------------------------------------------------------------------------

def run_check(args):
    wda = Wda(args.host, args.wda_port)
    ok = True
    try:
        status = wda.call("GET", "/status", timeout=10)
        value = status.get("value") or {}
        print(f"/status  sessionId={status.get('sessionId')}  ios={value.get('os', {}).get('version')}")
        sid = wda.ensure_session()
        print(f"session  {sid}")
        w, h = wda.window_size()
        print(f"window   {w:.0f} x {h:.0f} pt")
    except Exception as e:
        ok = False
        print(f"WDA  NG  {e}")
    try:
        if args.png:
            raw = base64.b64decode(wda.call("GET", "/screenshot", timeout=20).get("value") or "")
            print(f"screenshot  {png_size(raw)} px  {len(raw)} bytes")
        else:
            conn = http.client.HTTPConnection(args.host, args.mjpeg_port, timeout=10)
            conn.request("GET", "/")
            resp = conn.getresponse()
            for frame in iter_mjpeg_frames(resp.read1):
                print(f"mjpeg    {jpeg_size(frame)} px  {len(frame)} bytes")
                break
            conn.close()
    except Exception as e:
        ok = False
        print(f"映像  NG  {e}")
    return 0 if ok else 1


# ---------------------------------------------------------------------------

def enable_dpi_awareness():
    if sys.platform != "win32":
        return
    try:
        import ctypes

        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass


def parse_points(text):
    m = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*[xX,]\s*(\d+(?:\.\d+)?)\s*", text)
    if not m:
        raise argparse.ArgumentTypeError("402x874 の形で渡す")
    return float(m.group(1)), float(m.group(2))


def build_parser():
    p = argparse.ArgumentParser(
        description="WDA の MJPEG を窓に映し、クリックをタップにする。"
    )
    p.add_argument("--host", default="127.0.0.1", help="WDA を出している PC 側のアドレス")
    p.add_argument("--wda-port", type=int, default=8100, help="WDA の HTTP API のポート")
    p.add_argument("--mjpeg-port", type=int, default=9100, help="WDA の MJPEG のポート")
    p.add_argument("--png", action="store_true",
                   help="Pillow を使わず /screenshot の PNG を映す。遅い")
    p.add_argument("--png-interval", type=float, default=0.2, help="--png の取得間隔の秒")
    p.add_argument("--points", type=parse_points, default=None,
                   help="画面の大きさを点で渡す。例 402x874。省略すると WDA から取る")
    p.add_argument("--max-height", type=int, default=0,
                   help="最初に開く窓の高さの上限。0 なら画面の 85 パーセント")
    p.add_argument("--tap-hold-ms", type=int, default=80,
                   help="タップで指を置いておく時間。短すぎると取りこぼされる")
    p.add_argument("--fps", type=int, default=None,
                   help="WDA の mjpegServerFramerate を変える。既定は 10")
    p.add_argument("--quality", type=int, default=None,
                   help="WDA の mjpegServerScreenshotQuality を変える。既定は 25")
    p.add_argument("--mjpeg-scale", type=int, default=None,
                   help="WDA の mjpegScalingFactor を変える。百分率で既定は 100")
    p.add_argument("--check", action="store_true",
                   help="窓を出さず、つながるかだけを見て終わる")
    return p


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except Exception:
            pass
    args = build_parser().parse_args(argv)
    if args.check:
        return run_check(args)
    if not args.png and not HAVE_PIL:
        print(
            "Pillow が入っていない。MJPEG を映すには要る。\n"
            "  python -m pip install pillow\n"
            "入れずに試すなら --png を付ける。",
            file=sys.stderr,
        )
        return 2
    enable_dpi_awareness()
    Viewer(args).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
