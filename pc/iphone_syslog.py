"""USB でつないだ iPhone の記録（syslog）を読む。既定では「計算の受け手」の行だけ出す。

0.7.0 から、受け手は ggml と Vulkan の記録を os_log に出している（行の頭が ggml: か vk:）。

  python iphone_syslog.py                # RpcServer の行だけ
  python iphone_syslog.py --all          # 全部
  python iphone_syslog.py --grep vk:     # この文字を含む行だけ
  python iphone_syslog.py --seconds 30   # 30 秒で止める
"""
import argparse
import plistlib
import re
import sys
import time

import iphone_storage as st
import usb_forward as uf


def unvis(s):
    """syslog_relay は日本語などを vis(3) の書き方（\\M-x・\\M^x・\\^x）で送ってくるので、元のバイトに戻す。"""
    out = bytearray()
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            if s.startswith("\\M-", i) and i + 3 < len(s):
                out.append(ord(s[i + 3]) | 0x80)
                i += 4
                continue
            if s.startswith("\\M^", i) and i + 3 < len(s):
                ch = s[i + 3]
                out.append(0xFF if ch == "?" else ((ord(ch) ^ 0x40) | 0x80))
                i += 4
                continue
            if s.startswith("\\^", i) and i + 2 < len(s):
                ch = s[i + 2]
                out.append(0x7F if ch == "?" else (ord(ch) ^ 0x40))
                i += 3
                continue
            if s[i + 1] == "\\":
                out.append(0x5C)
                i += 2
                continue
            m = re.match(r"\\([0-7]{3})", s[i:])
            if m:
                out.append(int(m.group(1), 8) & 0xFF)
                i += 4
                continue
        out += c.encode("utf-8")
        i += 1
    return out.decode("utf-8", errors="replace")


def open_relay():
    devs = [d for d in uf.list_devices() if d.get("Properties", {}).get("ConnectionType") == "USB"]
    if not devs:
        raise SystemExit("USB の iPhone が見えない")
    did, udid = devs[0]["DeviceID"], devs[0]["Properties"]["SerialNumber"]
    with open(f"C:/ProgramData/Apple/Lockdown/{udid}.plist", "rb") as f:
        rec = plistlib.load(f)
    s = uf.connect_device(did, st.LOCKDOWN)
    st.send_plist(s, {"Label": "syslog", "Request": "QueryType"})
    st.recv_plist(s)
    st.send_plist(s, {"Label": "syslog", "Request": "StartSession", "HostID": rec["HostID"], "SystemBUID": rec["SystemBUID"]})
    if st.recv_plist(s).get("EnableSessionSSL"):
        s = st.wrap_tls(s, rec)
    st.send_plist(s, {"Label": "syslog", "Request": "StartService", "Service": "com.apple.syslog_relay"})
    svc = st.recv_plist(s)
    if "Port" not in svc:
        raise SystemExit(f"syslog_relay を開けない: {svc}")
    p = uf.connect_device(did, svc["Port"])
    if svc.get("EnableServiceSSL"):
        p = st.wrap_tls(p, rec)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--grep", default=None)
    ap.add_argument("--seconds", type=float, default=0)
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    p = open_relay()
    p.settimeout(1.0)
    end = time.time() + a.seconds if a.seconds else None
    buf = b""
    while end is None or time.time() < end:
        try:
            chunk = p.recv(65536)
        except TimeoutError:
            continue
        except OSError as e:
            if "timed out" in str(e):
                continue
            raise
        if not chunk:
            break
        buf += chunk.replace(b"\x00", b"")
        *lines, buf = buf.split(b"\n")
        for raw in lines:
            line = unvis(raw.decode("utf-8", errors="replace"))
            if a.grep is not None:
                if a.grep not in line:
                    continue
            elif not a.all and "RpcServer" not in line:
                continue
            print(line, flush=True)


if __name__ == "__main__":
    main()
