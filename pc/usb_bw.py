"""USB の通り道そのものの速さを測る。iPhone のアプリの 50058 番（vk/remote/bw.c）とバイトを流し合う。

どこで詰まっているかを分けるため、次を組み合わせて測る。
  通り道：Apple のサービス（usbmuxd）に直接 ／ usb_forward.py と同じ Python の中継を通す
  つなぎ：1 本・2 本・4 本を同時に
  向き　：iPhone → PC ／ PC → iPhone

  python usb_bw.py                       # USB の iPhone で全部
  python usb_bw.py --addr 127.0.0.1:50058 # 代役（PC の bw.exe）や、ほかの中継の先で測る
"""
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
    a = ap.parse_args()
    if a.mux:
        uf.MUX = ("127.0.0.1", a.mux)
    conns = [int(x) for x in a.conns.split(",")]
    total = a.mb * MB

    paths = []
    if a.addr:
        h, p = a.addr.rsplit(":", 1)
        paths.append((a.addr, lambda: socket.create_connection((h, int(p)))))
    else:
        dev = uf.usb_device_id()
        paths.append(("Apple のサービスに直接", lambda: uf.connect_device(dev, a.port)))
        rp = start_relay(a.port)
        paths.append(("Python の中継を通す", lambda: socket.create_connection(("127.0.0.1", rp))))

    print(f"1 回に {a.mb} MB。数字は MB/s")
    for name, opener in paths:
        for c in conns:
            down = measure(opener, c, 0, total)
            up = measure(opener, c, 1, total)
            print(f"{name:<18} つなぎ {c} 本   iPhone→PC {down:7.1f}   PC→iPhone {up:7.1f}", flush=True)


if __name__ == "__main__":
    main()
