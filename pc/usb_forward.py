"""PC の 127.0.0.1:<local> を、USB でつないだ iPhone の <remote> 番につなぐ。

iTunes と一緒に入る Apple Mobile Device Service が 127.0.0.1:27015 で待っている（usbmuxd）。
そこに「この iPhone のこの番号につないで」と頼むと、その接続がそのまま USB の通り道になる。

  python usb_forward.py            # 127.0.0.1:50053 → iPhone の 50052
  python usb_forward.py --list     # つながっている iPhone を出すだけ（番号・USB か Wi-Fi か・UDID・名前）
  python usb_forward.py --udid <UDID> --local 50061 --remote 50060   # UDID の iPhone だけにつなぐ（何台もつなぐとき）
UDID は環境変数 IPHONE_VK_UDID でも決められる（ドライバ icd.c と同じ名前）。決めなければ、USB でつながった最初の 1 台。
usbmuxd の口は IPHONE_VK_MUX（番号か host:port。Linux は /パス）で変えられる（偽の usbmuxd の試験用。ドライバと同じ名前）。
名前は iPhone に聞く。ペアリングしていない iPhone は、名前が「?」になる。
"""
import argparse
import os
import plistlib
import socket
import struct
import sys
import threading

MUX = ("127.0.0.1", 27015)


def mux_connect():
    """usbmuxd につなぐ。IPHONE_VK_MUX=番号（127.0.0.1 の番号）・host:port・/パス（Unix socket）で口を変えられる"""
    m = os.environ.get("IPHONE_VK_MUX", "")
    if m.startswith("unix:"):
        m = m[5:]
    if m.startswith("/") and hasattr(socket, "AF_UNIX"):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(m)
        return s
    if m:
        host, _, port = m.rpartition(":")
        return socket.create_connection((host or "127.0.0.1", int(port)))
    return socket.create_connection(MUX)


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
    with mux_connect() as s:
        res = mux_request(s, base_msg("ListDevices"))
    return res.get("DeviceList", [])


def same_udid(a, b):
    """大文字小文字と '-' を無視して比べる（ドライバ icd.c の udid_same と同じ）"""
    n = lambda x: (x or "").replace("-", "").upper()
    return n(a) == n(b)


def usb_devices():
    """USB でつながった機器（Wi-Fi は含めない）。usbmuxd の並びのまま"""
    return [d for d in list_devices() if d.get("Properties", {}).get("ConnectionType") == "USB"]


def pick_device(udid=None):
    """udid（無ければ環境変数 IPHONE_VK_UDID）に合う USB の機器。決めなければ最初の 1 台。合う物が無ければ UDID の一覧を出して終わる"""
    udid = udid or os.environ.get("IPHONE_VK_UDID") or None
    devs = usb_devices()
    for d in devs:
        if not udid or same_udid(d["Properties"].get("SerialNumber"), udid):
            return d
    if udid:
        have = ", ".join(d["Properties"].get("SerialNumber", "?") for d in devs) or "なし"
        raise SystemExit(f"UDID {udid} の USB の iPhone が見つからない。USB でつながっている機器：{have}")
    raise SystemExit("USB でつながった iPhone が見つからない")


def usb_device_id(udid=None):
    return pick_device(udid)["DeviceID"]


def device_name(dev_id):
    """iPhone の名前（lockdownd の DeviceName）。聞けなければ None（ペアリングが要る iPhone もある）"""
    try:
        import iphone_storage as st  # iphone_storage も usb_forward を読むので、使うときに読む
        s = connect_device(dev_id, st.LOCKDOWN)
        s.settimeout(3)
        st.send_plist(s, {"Label": "usb_forward", "Request": "QueryType"})
        st.recv_plist(s)
        st.send_plist(s, {"Label": "usb_forward", "Request": "GetValue", "Key": "DeviceName"})
        v = st.recv_plist(s).get("Value")
        s.close()
        return v
    except Exception:
        return None


def connect_device(device_id, port):
    s = mux_connect()
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


def serve(local, remote, udid=None):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", local))
    srv.listen(8)
    print(f"127.0.0.1:{local} → USB の iPhone の {remote} 番" + (f"（UDID {udid}）" if udid else ""), flush=True)
    while True:
        client, _ = srv.accept()
        try:
            # 挿し直すと番号が変わるので、つなぐたびに引き直す
            phone = connect_device(usb_device_id(udid), remote)
        except (ConnectionError, SystemExit) as e:
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
    ap.add_argument("--udid", help="この UDID の iPhone につなぐ（無ければ IPHONE_VK_UDID、それも無ければ USB の最初の 1 台）")
    ap.add_argument("--no-name", action="store_true", help="--list で名前を聞かない（iPhone に話しかけない）")
    a = ap.parse_args()
    if a.list:
        # 番号・つなぎ方・UDID・名前。UDID は USB の機器を選ぶ鍵（IPHONE_VK=usb:<UDID>）
        for d in list_devices():
            p = d.get("Properties", {})
            kind = p.get("ConnectionType")
            name = "-" if a.no_name or kind != "USB" else (device_name(d["DeviceID"]) or "?")
            print(d["DeviceID"], kind, p.get("ProductID"), p.get("SerialNumber", "?"), name)
    elif a.probe:
        try:
            connect_device(usb_device_id(a.udid), a.remote).close()
            print(f"{a.remote} 番：つながった")
        except ConnectionError as e:
            print(e)
            sys.exit(1)
    else:
        serve(a.local, a.remote, a.udid)
