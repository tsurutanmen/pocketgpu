"""USB の通り道そのものの速さを測る。iPhone のアプリの 50058 番（vk/remote/bw.c）とバイトを流し合う。

どこで詰まっているかを分けるため、次を組み合わせて測る。
  通り道：Apple のサービス（usbmuxd）に直接 ／ usb_forward.py と同じ Python の中継を通す
  つなぎ：1 本・2 本・4 本を同時に
  向き　：iPhone → PC ／ PC → iPhone

  python usb_bw.py                       # USB の iPhone で全部
  python usb_bw.py --addr 127.0.0.1:50058 # 代役（PC の bw.exe）や、ほかの中継の先で測る
  python usb_bw.py --link                 # インターネット共有の口（USB を有線のネットワークとして使う）と usbmux を比べる
  python usb_bw.py --link --link-boot 127.0.0.1:50070  # 代役で：受け手に住所と合言葉を聞く所を host:port にする
"""
import os
import re
import argparse
import socket
import struct
import threading
import time

import usb_forward as uf

MB = 1 << 20
CHUNK = 1 << 20


def tune(s):
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    for opt in (socket.SO_SNDBUF, socket.SO_RCVBUF):
        s.setsockopt(socket.SOL_SOCKET, opt, 4 << 20)


def recv_exact(s, n, buf):
    view = memoryview(buf)
    got = 0
    while got < n:
        k = s.recv_into(view[:min(CHUNK, n - got)])
        if not k:
            raise ConnectionError("途中で切れた")
        got += k
    return got


def one_transfer(s, direction, n, buf):
    s.sendall(struct.pack("<IQ", direction, n))
    if direction == 0:
        recv_exact(s, n, buf)
    else:
        left = n
        while left:
            k = min(CHUNK, left)
            s.sendall(buf[:k])
            left -= k
        back = bytearray(8)
        recv_exact(s, 8, back)
        if struct.unpack("<Q", back)[0] != n:
            raise ConnectionError("受け手が受け取った数が合わない")


def measure(opener, conns, direction, total):
    socks = [opener() for _ in range(conns)]
    for s in socks:
        tune(s)
    bufs = [bytearray(CHUNK) for _ in socks]
    # 一度少し流して、つなぎを温める
    for s, b in zip(socks, bufs):
        one_transfer(s, direction, 4 * MB, b)
    per = total // conns
    errs = []

    def run(s, b):
        try:
            one_transfer(s, direction, per, b)
        except Exception as e:
            errs.append(e)

    ts = [threading.Thread(target=run, args=(s, b)) for s, b in zip(socks, bufs)]
    t0 = time.perf_counter()
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    dt = time.perf_counter() - t0
    for s in socks:
        s.close()
    if errs:
        raise errs[0]
    return per * conns / MB / dt


def rtt_ms(opener, n=200):
    """1 バイトを頼んで受け取る往復を n 回。中央値と最大（ミリ秒）"""
    s = opener()
    tune(s)
    b = bytearray(16)
    ts = []
    for _ in range(n):
        t0 = time.perf_counter()
        one_transfer(s, 0, 1, b)
        ts.append((time.perf_counter() - t0) * 1000)
    s.close()
    ts.sort()
    return ts[len(ts) // 2], ts[-1]


def op_number(name):
    """vk/remote/proto.h の enum vr_op から、命令の番号を数える"""
    here = os.path.dirname(os.path.abspath(__file__))
    src = open(os.path.join(here, "..", "vk", "remote", "proto.h"), encoding="utf-8").read()
    body = src.split("enum vr_op {", 1)[1].split("};", 1)[0]
    n = 0
    for line in body.splitlines():
        m = re.match(r"\s+(VR_[A-Z0-9_]+)\s*(?:=\s*(\d+))?\s*,", line)
        if not m:
            continue
        n = int(m.group(2)) if m.group(2) else n + 1
        if m.group(1) == name:
            return n
    raise KeyError(name)


def ask_link(boot):
    """受け手（50060 番）に VR_GET_LINK で、インターネット共有の口の住所と合言葉を聞く"""
    if boot:
        h, p = boot.rsplit(":", 1)
        s = socket.create_connection((h, int(p)), timeout=5)
    else:
        s = uf.connect_device(uf.usb_device_id(), 50060)
    s.settimeout(5)
    s.sendall(struct.pack("<II", op_number("VR_GET_LINK"), 0))
    head = bytearray(8)
    recv_exact(s, 8, head)
    res, n = struct.unpack("<iI", head)
    body = bytearray(max(n, 1))
    if n:
        recv_exact(s, n, body)
    s.close()
    if res != 0 or n != 28 or not struct.unpack("<I", body[:4])[0]:
        raise ConnectionError("受け手がインターネット共有の口を教えない（共有が切れている・アプリの「インターネット共有でも接続を受け付ける」が切・古い受け手）")
    token = bytes(body[4:20])
    ip = socket.inet_ntoa(bytes(body[20:24]))
    return ip, token


def start_relay(remote):
    """usb_forward.py と同じ中継を、空いている番号で立てる"""
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(16)
    port = srv.getsockname()[1]

    def loop():
        while True:
            client, _ = srv.accept()
            phone = uf.connect_device(uf.usb_device_id(), remote)
            for s in (client, phone):
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            threading.Thread(target=uf.pump, args=(client, phone), daemon=True).start()
            threading.Thread(target=uf.pump, args=(phone, client), daemon=True).start()

    threading.Thread(target=loop, daemon=True).start()
    return port


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=50058, help="iPhone のアプリの番号")
    ap.add_argument("--addr", help="USB を使わずに、この番地へ直接つなぐ（代役など）")
    ap.add_argument("--mb", type=int, default=256, help="1 回に流す量（MB）")
    ap.add_argument("--conns", default="1,2,4")
    ap.add_argument("--mux", type=int, help="試験用：usbmuxd の番号（偽物の fake_usbmuxd.py に向ける）")
    ap.add_argument("--link", action="store_true", help="インターネット共有の口でも測り、usbmux と比べる")
    ap.add_argument("--link-boot", help="試験用：住所と合言葉を聞く受け手を host:port にする（ふつうは USB の 50060 番）")
    a = ap.parse_args()
    if a.mux:
        uf.MUX = ("127.0.0.1", a.mux)
    conns = [int(x) for x in a.conns.split(",")]
    total = a.mb * MB

    paths = []
    if a.link:
        ip, token = ask_link(a.link_boot)
        print(f"インターネット共有の口：{ip}:{a.port}")

        def link_open():
            s = socket.create_connection((ip, a.port), timeout=5)
            s.settimeout(None)
            s.sendall(struct.pack("<I", 0x41524B56) + token)
            return s

        paths.append(("インターネット共有の口", link_open))
        if not a.link_boot:
            dev = uf.usb_device_id()
            paths.append(("Apple のサービスに直接", lambda: uf.connect_device(dev, a.port)))
    elif a.addr:
        h, p = a.addr.rsplit(":", 1)
        paths.append((a.addr, lambda: socket.create_connection((h, int(p)))))
    else:
        dev = uf.usb_device_id()
        paths.append(("Apple のサービスに直接", lambda: uf.connect_device(dev, a.port)))
        rp = start_relay(a.port)
        paths.append(("Python の中継を通す", lambda: socket.create_connection(("127.0.0.1", rp))))

    for name, opener in paths:
        med, mx = rtt_ms(opener)
        print(f"{name:<18} 往復 中央値 {med:6.3f} ms   最大 {mx:6.3f} ms", flush=True)
    print(f"1 回に {a.mb} MB。数字は MB/s")
    for name, opener in paths:
        for c in conns:
            down = measure(opener, c, 0, total)
            up = measure(opener, c, 1, total)
            print(f"{name:<18} つなぎ {c} 本   iPhone→PC {down:7.1f}   PC→iPhone {up:7.1f}", flush=True)


if __name__ == "__main__":
    main()
