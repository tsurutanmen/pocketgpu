"""Apple Mobile Device Service（usbmuxd）の偽物。iPhone が無いときに、ドライバの「USB に直接つなぐ」を試すため。

ListDevices には USB の機器を 1 台あると答え、Connect を頼まれたら、その番号を代役の番地に読み替えてつなぐ。
  python fake_usbmuxd.py --listen 27115 --map 50060=127.0.0.1:50070
  IPHONE_VK=usb IPHONE_VK_MUX=27115 でドライバを向ける
2 台以上に見せるときは --device を台の数だけ（DeviceID は 7 から順に振る。--map はあれば 1 台目の既定の機器になる）
  python fake_usbmuxd.py --listen 27115 \
      --device 00008150-00000000000000AA,name=テストA,50060=127.0.0.1:47441 \
      --device 00008150-00000000000000BB,name=テストB,50060=127.0.0.1:47442
  IPHONE_VK=usb:00008150-00000000000000BB IPHONE_VK_MUX=27115 でドライバを 2 台目に向ける
  62078 番（lockdownd）には、機器の名前だけ答える小さな偽物が付く（usb_forward.py --list の名前の試験用）
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


def read_lockdown(c):
    head = b""
    while len(head) < 4:
        d = c.recv(4 - len(head))
        if not d:
            return None
        head += d
    n = struct.unpack(">I", head)[0]
    body = b""
    while len(body) < n:
        d = c.recv(n - len(body))
        if not d:
            return None
        body += d
    return plistlib.loads(body)


def lockdown(c, dev):
    """lockdownd の偽物。QueryType と GetValue DeviceName にだけ答える（ペアリングは無い）"""
    while True:
        try:
            req = read_lockdown(c)
            if req is None:
                break
            k = req.get("Request")
            if k == "QueryType":
                res = {"Request": k, "Type": "com.apple.mobile.lockdown"}
            elif k == "GetValue" and req.get("Key") == "DeviceName":
                res = {"Request": k, "Key": "DeviceName", "Value": dev["name"]}
            else:
                res = {"Request": k, "Error": "InvalidRequest"}
            body = plistlib.dumps(res)
            c.sendall(struct.pack(">I", len(body)) + body)
        except OSError:
            break
    c.close()


def handle(c, devices):
    msg = read_msg(c)
    if not msg:
        return c.close()
    kind = msg.get("MessageType")
    print("頼まれた：", kind, {k: v for k, v in msg.items() if k in ("DeviceID", "PortNumber")}, flush=True)
    if kind == "ListDevices":
        # 本物と同じく、Wi-Fi の機器（番号 3）を先に、USB の機器（番号 7 から）をあとに並べる。Wi-Fi の機器は 1 台目の UDID を名乗る（選ぶ側が読み飛ばす試験）
        dev = lambda i, t, udid: {"DeviceID": i, "MessageType": "Attached", "Properties": {
            "ConnectionSpeed": 480000000, "ConnectionType": t, "DeviceID": i, "LocationID": 0,
            "ProductID": 4776, "SerialNumber": udid, "USBSerialNumber": udid.replace("-", "")}}
        lst = [dev(3, "Network", devices[0]["udid"])] + [dev(d["id"], "USB", d["udid"]) for d in devices]
        send_msg(c, {"DeviceList": lst})
        return c.close()
    if kind == "Connect":
        port = socket.ntohs(msg["PortNumber"])
        dev = next((d for d in devices if d["id"] == msg.get("DeviceID")), None)
        if dev is None or (port not in dev["map"] and port != 62078):
            send_msg(c, {"MessageType": "Result", "Number": 3})
            return c.close()
        if port == 62078 and port not in dev["map"]:
            send_msg(c, {"MessageType": "Result", "Number": 0})
            return lockdown(c, dev)
        h, p = dev["map"][port]
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
    ap.add_argument("--map", action="append", default=[], help="iPhone の番号=host:port（既定の 1 台目の機器の分）")
    ap.add_argument("--device", action="append", default=[],
                    help="機器を 1 台足す：UDID[,name=名前],番号=host:port[,番号=host:port ...]。足した順に DeviceID を 7 から振る")
    a = ap.parse_args()
    devices = []
    mapping = {}
    for m in a.map:
        k, v = m.split("=")
        h, p = v.rsplit(":", 1)
        mapping[int(k)] = (h, int(p))
    if mapping or not a.device:
        devices.append({"udid": "00008150-000000000000001E", "name": "fake-iphone", "map": mapping})
    for spec in a.device:
        parts = spec.split(",")
        d = {"udid": parts[0], "name": "fake-" + parts[0][-4:], "map": {}}
        for x in parts[1:]:
            if x.startswith("name="):
                d["name"] = x[5:]
            else:
                k, v = x.split("=")
                h, p = v.rsplit(":", 1)
                d["map"][int(k)] = (h, int(p))
        devices.append(d)
    for i, d in enumerate(devices):
        d["id"] = 7 + i
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
    print(f"偽の usbmuxd：{where}", [(d["id"], d["udid"], d["map"]) for d in devices], flush=True)
    while True:
        c, _ = srv.accept()
        threading.Thread(target=handle, args=(c, devices), daemon=True).start()


if __name__ == "__main__":
    main()
