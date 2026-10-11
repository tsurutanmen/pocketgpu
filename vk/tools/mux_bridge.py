"""usbmuxd の Unix socket（Linux の /run/usbmuxd）を、127.0.0.1 の TCP の口として開く橋渡し。

Steam の入れ物（pressure-vessel。Proton のゲームはこの中で動く）の中からは /run/usbmuxd が見えないことがある。
入れ物の中と外は網が同じなので、外でこれを動かしておき、ゲームの起動オプションで IPHONE_VK_MUX=番号 に向ける。
  python3 mux_bridge.py                      /run/usbmuxd を 127.0.0.1:27015 で開く（Windows の Apple Mobile Device Service と同じ番号）
  python3 mux_bridge.py --unix パス --listen 27016
ゲームの起動オプション：IPHONE_VK=usb IPHONE_VK_MUX=27015 %command%
つなぐ先は 127.0.0.1 だけで待つ（ほかの機械からは使えない）
"""
import argparse
import socket
import threading


def pump(a, b):
    try:
        while True:
            d = a.recv(1 << 20)
            if not d:
                break
            b.sendall(d)
    except OSError:
        pass
    for x in (a, b):
        try:
            x.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--unix", default="/run/usbmuxd", help="usbmuxd の Unix socket")
    ap.add_argument("--listen", type=int, default=27015, help="127.0.0.1 のこの番号で待つ")
    a = ap.parse_args()
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", a.listen))
    srv.listen(16)
    print(f"橋渡し：127.0.0.1:{a.listen} → {a.unix}", flush=True)
    while True:
        c, _ = srv.accept()
        u = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            u.connect(a.unix)
        except OSError as e:
            print(f"{a.unix} につながらない：{e}", flush=True)
            c.close()
            continue
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=pump, args=(c, u), daemon=True).start()
        threading.Thread(target=pump, args=(u, c), daemon=True).start()


if __name__ == "__main__":
    main()
