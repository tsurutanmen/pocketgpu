"""USB でつないだ iPhone に入っている「計算の受け手」の版を読むだけ。

AltStore で入れると bundle id の後ろにチームの ID が付く（com.tsurutanmen.iphonerpc.XXXXXXXXXX）ので、前の方で探す。
"""
import plistlib
import sys

import iphone_storage as st
import usb_forward as uf

PREFIX = sys.argv[1] if len(sys.argv) > 1 else "com.tsurutanmen.iphonerpc"


def main():
    devs = [d for d in uf.list_devices() if d.get("Properties", {}).get("ConnectionType") == "USB"]
    if not devs:
        print("USB の iPhone が見えない")
        return
    did, udid = devs[0]["DeviceID"], devs[0]["Properties"]["SerialNumber"]
    with open(f"C:/ProgramData/Apple/Lockdown/{udid}.plist", "rb") as f:
        rec = plistlib.load(f)
    s = uf.connect_device(did, st.LOCKDOWN)
    st.send_plist(s, {"Label": "ver", "Request": "QueryType"})
    st.recv_plist(s)
    st.send_plist(s, {"Label": "ver", "Request": "StartSession", "HostID": rec["HostID"], "SystemBUID": rec["SystemBUID"]})
    if st.recv_plist(s).get("EnableSessionSSL"):
        s = st.wrap_tls(s, rec)
    st.send_plist(s, {"Label": "ver", "Request": "StartService", "Service": "com.apple.mobile.installation_proxy"})
    svc = st.recv_plist(s)
    p = uf.connect_device(did, svc["Port"])
    if svc.get("EnableServiceSSL"):
        p = st.wrap_tls(p, rec)
    st.send_plist(p, {"Command": "Browse", "ClientOptions": {"ApplicationType": "User", "ReturnAttributes": [
        "CFBundleIdentifier", "CFBundleShortVersionString", "CFBundleVersion", "Entitlements"]}})
    found = False
    while True:
        r = st.recv_plist(p)
        for a in r.get("CurrentList", []):
            if a["CFBundleIdentifier"].startswith(PREFIX):
                found = True
                mem = a.get("Entitlements", {}).get("com.apple.developer.kernel.increased-memory-limit")
                print(f"{a['CFBundleIdentifier']}: {a.get('CFBundleShortVersionString')} ({a.get('CFBundleVersion')})  メモリを増やす権限 {mem}")
        if r.get("Status") == "Complete":
            break
    if not found:
        print(f"{PREFIX} は入っていない")


if __name__ == "__main__":
    main()
