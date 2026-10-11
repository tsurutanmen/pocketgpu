"""Apple Mobile Device Service（usbmuxd）の偽物。iPhone が無いときに、ドライバの「USB に直接つなぐ」を試すため。

ListDevices には USB の機器を 1 台あると答え、Connect を頼まれたら、その番号を代役の番地に読み替えてつなぐ。
  python fake_usbmuxd.py --listen 27115 --map 50060=127.0.0.1:50070
  IPHONE_VK=usb IPHONE_VK_MUX=27115 でドライバを向ける
Linux の usbmuxd と同じく Unix socket で待つときは --unix パス（ドライバは IPHONE_VK_MUX=パス か USBMUXD_SOCKET_ADDRESS=UNIX:パス）
  python3 fake_usbmuxd.py --unix /tmp/usbmuxd --map 50060=127.0.0.1:50330
"""
import os
import argparse
import plistlib
import socket
import struct
import threading


def read_msg(s):
    head = b""
    while len(head) < 16:
        c = s.recv(16 - len(head))
        if not c:
            return None
        head += c
    n = struct.unpack("<IIII", head)[0]
    body = b""
    while len(body) < n - 16:
        c = s.recv(n - 16 - len(body))
        if not c:
            return None
        body += c
    return plistlib.loads(body)


def send_msg(s, msg):
    body = plistlib.dumps(msg)
    s.sendall(struct.pack("<IIII", 16 + len(body), 1, 8, 1) + body)


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


def handle(c, mapping):
    msg = read_msg(c)
    if not msg:
        return c.close()
    kind = msg.get("MessageType")
    print("頼まれた：", kind, {k: v for k, v in msg.items() if k in ("DeviceID", "PortNumber")}, flush=True)
    if kind == "ListDevices":
        # 本物と同じく、Wi-Fi の機器（番号 3）を先に、USB の機器（番号 7）をあとに並べる
        dev = lambda i, t: {"DeviceID": i, "MessageType": "Attached", "Properties": {
            "ConnectionSpeed": 480000000, "ConnectionType": t, "DeviceID": i, "LocationID": 0,
            "ProductID": 4776, "SerialNumber": "00008150-000000000000001E", "USBSerialNumber": "000081500000000000000001E"}}
        send_msg(c, {"DeviceList": [dev(3, "Network"), dev(7, "USB")]})
        return c.close()
    if kind == "Connect":
        port = socket.ntohs(msg["PortNumber"])
        if msg.get("DeviceID") != 7 or port not in mapping:
            send_msg(c, {"MessageType": "Result", "Number": 3})
            return c.close()
        h, p = mapping[port]
        try:
            t = socket.create_connection((h, p))
        except OSError:
            send_msg(c, {"MessageType": "Result", "Number": 3})
            return c.close()
        send_msg(c, {"MessageType": "Result", "Number": 0})
        threading.Thread(target=pump, args=(c, t), daemon=True).start()
        threading.Thread(target=pump, args=(t, c), daemon=True).start()
        return
    c.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", type=int, default=27115)
    ap.add_argument("--unix", help="この Unix socket で待つ（Linux の /var/run/usbmuxd の代わり）")
    ap.add_argument("--map", action="append", default=[], help="iPhone の番号=host:port")
    a = ap.parse_args()
    mapping = {}
    for m in a.map:
        k, v = m.split("=")
        h, p = v.rsplit(":", 1)
        mapping[int(k)] = (h, int(p))
    if a.unix:
        if os.path.exists(a.unix):
            os.unlink(a.unix)
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(a.unix)
        where = a.unix
    else:
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", a.listen))
        where = f"127.0.0.1:{a.listen}"
    srv.listen(16)
    print(f"偽の usbmuxd：{where}", mapping, flush=True)
    while True:
        c, _ = srv.accept()
        threading.Thread(target=handle, args=(c, mapping), daemon=True).start()


if __name__ == "__main__":
    main()
