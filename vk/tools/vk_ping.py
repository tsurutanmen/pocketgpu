"""Vulkan の受け手との往復の時間を測る（取り決めの PING を投げるだけ）。

  python vk_ping.py                    # 127.0.0.1:50061（usb_forward.py --local 50061 --remote 50060 の先の iPhone）
  python vk_ping.py 127.0.0.1:50070 500

受け手は 1 本ずつしか相手にしないので、ほかのアプリがつないでいる間は待たされる。
"""
import socket
import statistics
import struct
import sys
import time

VR_HELLO = 1
VR_PING = 46
VR_PROTO_VERSION = 3


def call(s, op, body=b""):
    s.sendall(struct.pack("<II", op, len(body)) + body)
    head = b""
    while len(head) < 8:
        head += s.recv(8 - len(head))
    res, n = struct.unpack("<iI", head)
    left = n
    while left:
        left -= len(s.recv(min(left, 1 << 20)))
    return res


def main():
    addr = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1:50061"
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    host, port = addr.rsplit(":", 1)
    s = socket.create_connection((host, int(port)), timeout=10)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    r = call(s, VR_HELLO, struct.pack("<I", VR_PROTO_VERSION))
    if r != 0:
        raise SystemExit(f"あいさつに失敗 ({r})")
    t = []
    for _ in range(count):
        t0 = time.perf_counter()
        call(s, VR_PING)
        t.append((time.perf_counter() - t0) * 1000)
    t.sort()
    sys.stdout.reconfigure(encoding="utf-8")
    print(f"{addr} の往復 {count} 回：中央 {statistics.median(t):.2f} ms・最小 {t[0]:.2f}・90% {t[int(len(t) * 0.9)]:.2f}・最大 {t[-1]:.2f}")


if __name__ == "__main__":
    main()
