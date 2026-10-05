"""USB でつないだ iPhone の空き容量と、アプリごとの大きさを読むだけ（何も消さない）。

iTunes が作ったペアリングの記録（C:/ProgramData/Apple/Lockdown）で lockdownd と話す。
"""
import os
import plistlib
import socket
import ssl
import struct
import tempfile

import usb_forward as uf

LOCKDOWN = 62078


def send_plist(s, obj):
    body = plistlib.dumps(obj)
    s.sendall(struct.pack(">I", len(body)) + body)


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("切れた")
        buf += chunk
    return buf


def recv_plist(s):
    (n,) = struct.unpack(">I", recv_exact(s, 4))
    return plistlib.loads(recv_exact(s, n))


def wrap_tls(sock, rec):
    # 鍵は一時ファイルに書いて読ませたらすぐ消す
    fd, path = tempfile.mkstemp(suffix=".pem")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(rec["HostCertificate"] + b"\n" + rec["HostPrivateKey"])
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        ctx.set_ciphers("ALL:@SECLEVEL=0")
        ctx.load_cert_chain(path)
    finally:
        os.remove(path)
    return ctx.wrap_socket(sock)


def main():
    devs = [d for d in uf.list_devices() if d.get("Properties", {}).get("ConnectionType") == "USB"]
    dev = devs[0]
    did, udid = dev["DeviceID"], dev["Properties"]["SerialNumber"]
    with open(f"C:/ProgramData/Apple/Lockdown/{udid}.plist", "rb") as f:
        rec = plistlib.load(f)

    s = uf.connect_device(did, LOCKDOWN)
    send_plist(s, {"Label": "storage", "Request": "QueryType"})
    recv_plist(s)
    send_plist(s, {"Label": "storage", "Request": "StartSession", "HostID": rec["HostID"], "SystemBUID": rec["SystemBUID"]})
    r = recv_plist(s)
    if r.get("EnableSessionSSL"):
        s = wrap_tls(s, rec)

    send_plist(s, {"Label": "storage", "Request": "GetValue", "Domain": "com.apple.disk_usage.factory"})
    du = recv_plist(s).get("Value", {})
    send_plist(s, {"Label": "storage", "Request": "GetValue", "Key": "ProductType"})
    model = recv_plist(s).get("Value")
    gb = lambda v: f"{v / 1e9:.1f} GB"
    print(f"機種 {model}")
    for k in ("TotalDiskCapacity", "TotalDataCapacity", "AmountDataAvailable", "TotalDataAvailable", "AmountRestoreAvailable"):
        if k in du:
            print(f"  {k}: {gb(du[k])}")

    send_plist(s, {"Label": "storage", "Request": "StartService", "Service": "com.apple.mobile.installation_proxy"})
    svc = recv_plist(s)
    p = uf.connect_device(did, svc["Port"])
    if svc.get("EnableServiceSSL"):
        p = wrap_tls(p, rec)
    send_plist(p, {"Command": "Browse", "ClientOptions": {"ApplicationType": "Any", "ReturnAttributes": ["CFBundleIdentifier", "CFBundleDisplayName", "StaticDiskUsage", "DynamicDiskUsage"]}})
    apps = []
    while True:
        r = recv_plist(p)
        apps += r.get("CurrentList", [])
        if r.get("Status") == "Complete":
            break
    apps.sort(key=lambda a: a.get("StaticDiskUsage", 0) + a.get("DynamicDiskUsage", 0), reverse=True)
    print("大きいアプリ（本体＋データ）")
    for a in apps[:30]:
        tot = a.get("StaticDiskUsage", 0) + a.get("DynamicDiskUsage", 0)
        if tot <= 0:
            continue
        print(f"  {gb(tot):>9}  本体 {gb(a.get('StaticDiskUsage', 0)):>8}  データ {gb(a.get('DynamicDiskUsage', 0)):>8}  {a.get('CFBundleDisplayName', '')}  {a['CFBundleIdentifier']}")


if __name__ == "__main__":
    main()
