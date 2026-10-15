"""USB でつないだ iPhone のバッテリーの温度・電圧・電流を読むだけ（何も変えない）。

iTunes が作ったペアリングの記録で lockdownd と話し、診断のサービス（com.apple.mobile.diagnostics_relay）に
電源の部品（IOPMPowerSource）の値を聞く。iOS のアプリからは温度が 4 段階でしか取れないので、PC から度とワットで出す。
  python iphone_battery.py            1 回だけ
  python iphone_battery.py 5 60       5 秒ごとに 60 回（出した電力量の合計も出す）
"""
import sys
import time

import iphone_storage as st
import usb_forward as uf

import plistlib


def open_diag():
    dev = uf.pick_device()  # 環境変数 IPHONE_VK_UDID があればその UDID の iPhone、無ければ USB の最初の 1 台
    did, udid = dev["DeviceID"], dev["Properties"]["SerialNumber"]
    with open(f"C:/ProgramData/Apple/Lockdown/{udid}.plist", "rb") as f:
        rec = plistlib.load(f)
    s = uf.connect_device(did, st.LOCKDOWN)
    st.send_plist(s, {"Label": "battery", "Request": "QueryType"})
    st.recv_plist(s)
    st.send_plist(s, {"Label": "battery", "Request": "StartSession", "HostID": rec["HostID"], "SystemBUID": rec["SystemBUID"]})
    if st.recv_plist(s).get("EnableSessionSSL"):
        s = st.wrap_tls(s, rec)
    st.send_plist(s, {"Label": "battery", "Request": "StartService", "Service": "com.apple.mobile.diagnostics_relay"})
    svc = st.recv_plist(s)
    p = uf.connect_device(did, svc["Port"])
    if svc.get("EnableServiceSSL"):
        p = st.wrap_tls(p, rec)
    return p


def read(p):
    st.send_plist(p, {"Request": "IORegistry", "EntryClass": "IOPMPowerSource"})
    r = st.recv_plist(p)
    return r.get("Diagnostics", {}).get("IORegistry", {})


def summary(b):
    t = b.get("Temperature")                     # 100 分の 1 度
    v = b.get("Voltage") or b.get("AppleRawBatteryVoltage")  # mV
    a = b.get("InstantAmperage", b.get("Amperage"))           # mA（放電で負）
    if isinstance(a, int) and a >= 2 ** 63:
        a -= 2 ** 64
    w = (v or 0) / 1000 * (a or 0) / 1000
    pt = b.get("PowerTelemetryData", {}) or {}
    ad = b.get("AdapterDetails", {}) or {}
    return {
        "全体W": pt.get("SystemLoad", 0) / 1000,          # iPhone 全体が使っている電力（mW）
        "USBから W": pt.get("SystemPowerIn", 0) / 1000,   # 電源（USB）から入ってくる電力
        "電源の上限W": ad.get("Watts"),
        "温度": t / 100 if isinstance(t, int) else None,
        "電圧V": (v or 0) / 1000,
        "電流mA": a,
        "電力W": w,
        "充電中": b.get("IsCharging"),
        "外の電源": b.get("ExternalConnected"),
        "残り%": b.get("CurrentCapacity"),
    }


def main():
    every = float(sys.argv[1]) if len(sys.argv) > 1 else 0
    times = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    p = open_diag()
    wh = 0.0
    last = None
    for i in range(times):
        b = read(p)
        s = summary(b)
        now = time.time()
        if last is not None:
            wh += s["全体W"] * (now - last) / 3600
        last = now
        print(f"{time.strftime('%H:%M:%S')} iPhone 全体 {s['全体W']:.2f} W・USB から {s['USBから W']:.2f} W（上限 {s['電源の上限W']} W）・"
              f"電池 {s['電力W']:+.2f} W（負は放電）・温度 {s['温度']}℃・残り {s['残り%']}%・充電中 {s['充電中']}", flush=True)
        if i + 1 < times:
            time.sleep(every)
    if times > 1:
        print(f"この間に iPhone 全体が使った電力量 {wh:.3f} Wh（電気代 31 円/kWh なら {wh / 1000 * 31:.4f} 円）")
    if times == 1 and "-v" in sys.argv:
        for k in sorted(b):
            if not isinstance(b[k], (dict, list, bytes)):
                print(f"  {k}: {b[k]}")
    st.send_plist(p, {"Request": "Goodbye"})


if __name__ == "__main__":
    main()
