"""PC の 127.0.0.1:<local> を、USB でつないだ iPhone の <remote> 番につなぐ。

iTunes と一緒に入る Apple Mobile Device Service が 127.0.0.1:27015 で待っている（usbmuxd）。
そこに「この iPhone のこの番号につないで」と頼むと、その接続がそのまま USB の通り道になる。

  python usb_forward.py            # 127.0.0.1:50053 → iPhone の 50052
  python usb_forward.py --list     # つながっている iPhone を出すだけ
"""
import argparse
import plistlib
import socket
import struct
import sys
import threading

MUX = ("127.0.0.1", 27015)


def mux_request(sock, msg, tag=1):
    body = plistlib.dumps(msg)
    sock.sendall(struct.pack("<IIII", 16 + len(body), 1, 8, tag) + body)
    head = b""
    while len(head) < 16:
        chunk = sock.recv(16 - len(head))
        if not chunk:
            raise ConnectionError("usbmuxd が切れた")
        head += chunk
    length = struct.unpack("<IIII", head)[0]
    body = b""
    while len(body) < length - 16:
        chunk = sock.recv(length - 16 - len(body))
        if not chunk:
            raise ConnectionError("usbmuxd が切れた")
        body += chunk
    return plistlib.loads(body)


def base_msg(kind):
    return {"MessageType": kind, "ClientVersionString": "usb_forward", "ProgName": "usb_forward"}


def list_devices():
    with socket.create_connection(MUX) as s:
        res = mux_request(s, base_msg("ListDevices"))
    return res.get("DeviceList", [])


def usb_device_id():
    for d in list_devices():
        if d.get("Properties", {}).get("ConnectionType") == "USB":
            return d["DeviceID"]
    raise SystemExit("USB でつながった iPhone が見つからない")


def connect_device(device_id, port):
    s = socket.create_connection(MUX)
    msg = base_msg("Connect")
    msg["DeviceID"] = device_id
    msg["PortNumber"] = socket.htons(port)
    res = mux_request(s, msg)
    if res.get("Number") != 0:
        s.close()
        raise ConnectionError(f"iPhone の {port} 番につながらない（usbmuxd の返事 {res.get('Number')}）")
    return s


def pump(src, dst):
    try:
        while True:
            data = src.recv(1 << 20)
            if not data:
                break
            dst.sendall(data)
    except OSError:
        pass
    finally:
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def serve(local, remote):
    device_id = usb_device_id()
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", local))
    srv.listen(8)
    print(f"127.0.0.1:{local} → USB の iPhone（{device_id}）の {remote} 番", flush=True)
    while True:
        client, _ = srv.accept()
        try:
            phone = connect_device(device_id, remote)
        except ConnectionError as e:
            print(e, flush=True)
            client.close()
            continue
        for s in (client, phone):
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=pump, args=(client, phone), daemon=True).start()
        threading.Thread(target=pump, args=(phone, client), daemon=True).start()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--local", type=int, default=50053)
    ap.add_argument("--remote", type=int, default=50052)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--probe", action="store_true", help="iPhone の remote 番につながるかだけ見る")
    a = ap.parse_args()
    if a.list:
        for d in list_devices():
            p = d.get("Properties", {})
            print(d["DeviceID"], p.get("ConnectionType"), p.get("ProductID"))
    elif a.probe:
        try:
            connect_device(usb_device_id(), a.remote).close()
            print(f"{a.remote} 番：つながった")
        except ConnectionError as e:
            print(e)
            sys.exit(1)
    else:
        serve(a.local, a.remote)
