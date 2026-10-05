"""USB でつないだ iPhone から、アプリが落ちたときの記録（クラッシュレポート）をコピーする。

  python iphone_crashlogs.py RpcServer      # 名前に RpcServer を含むものを crashlogs/ へ
"""
import pathlib
import struct
import sys

import iphone_storage as st
import usb_forward as uf

AFC_MAGIC = b"CFA6LPAA"
OP_STATUS, OP_DATA, OP_READ_DIR, OP_FILE_OPEN, OP_FILE_READ, OP_FILE_CLOSE = 1, 2, 3, 13, 15, 20


class Afc:
    def __init__(self, sock):
        self.s = sock
        self.n = 0

    def call(self, op, data=b"", payload=b""):
        head_len = 40 + len(data)
        self.s.sendall(AFC_MAGIC + struct.pack("<QQQQ", head_len + len(payload), head_len, self.n, op) + data + payload)
        self.n += 1
        head = st.recv_exact(self.s, 40)
        _, entire, this, _, rop = struct.unpack("<8sQQQQ", head)
        data = st.recv_exact(self.s, this - 40)
        payload = st.recv_exact(self.s, entire - this)
        if rop == OP_STATUS:
            code = struct.unpack("<Q", data[:8])[0]
            if code:
                raise OSError(f"AFC エラー {code}")
        return data, payload

    def listdir(self, path):
        data, payload = self.call(OP_READ_DIR, path.encode() + b"\0")
        names = (data + payload).split(b"\0")
        return [n.decode() for n in names if n and n not in (b".", b"..")]

    def read(self, path):
        data, _ = self.call(OP_FILE_OPEN, struct.pack("<Q", 1) + path.encode() + b"\0")
        fh = struct.unpack("<Q", data[:8])[0]
        out = b""
        while True:
            d, p = self.call(OP_FILE_READ, struct.pack("<QQ", fh, 1 << 20))
            chunk = d + p
            if not chunk:
                break
            out += chunk
        self.call(OP_FILE_CLOSE, struct.pack("<Q", fh))
        return out


def lockdown_service(name):
    d = [x for x in uf.list_devices() if x.get("Properties", {}).get("ConnectionType") == "USB"][0]
    did, udid = d["DeviceID"], d["Properties"]["SerialNumber"]
    rec = st.plistlib.load(open(f"C:/ProgramData/Apple/Lockdown/{udid}.plist", "rb"))
    s = uf.connect_device(did, st.LOCKDOWN)
    st.send_plist(s, {"Label": "crash", "Request": "StartSession", "HostID": rec["HostID"], "SystemBUID": rec["SystemBUID"]})
    if st.recv_plist(s).get("EnableSessionSSL"):
        s = st.wrap_tls(s, rec)
    st.send_plist(s, {"Label": "crash", "Request": "StartService", "Service": name})
    svc = st.recv_plist(s)
    p = uf.connect_device(did, svc["Port"])
    if svc.get("EnableServiceSSL"):
        p = st.wrap_tls(p, rec)
    return p


def main():
    key = sys.argv[1] if len(sys.argv) > 1 else "RpcServer"
    # 記録を取り出せる場所に移してもらう（終わると 1 文字返ってくる）
    mover = lockdown_service("com.apple.crashreportmover")
    mover.settimeout(60)
    try:
        mover.recv(4)
    except OSError:
        pass
    afc = Afc(lockdown_service("com.apple.crashreportcopymobile"))
    out = pathlib.Path(__file__).with_name("crashlogs")
    out.mkdir(exist_ok=True)

    def walk(path):
        for n in afc.listdir(path):
            full = f"{path}/{n}" if path != "/" else f"/{n}"
            if "." not in n:
                try:
                    walk(full)
                except OSError:
                    pass
            elif key.lower() in n.lower():
                (out / n).write_bytes(afc.read(full))
                print("コピー", full)

    walk("/")


if __name__ == "__main__":
    main()
