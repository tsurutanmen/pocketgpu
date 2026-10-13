"""試験用の偽の usbmuxd（Linux）。UNIX ソケットで待ち、本物の bin/pgpu-probe と話す。本物の iPhone も usbmuxd も使わない。
  python3 fake_usbmuxd.py --socket FAKE_DIR/usbmuxd.sock --state FAKE_DIR
  python3 fake_usbmuxd.py --stale FAKE_DIR/old.sock     つないでも答えない、古いソケットのファイルだけを作って終わる
状態は偽の pgpu-probe（tests/fakes/pgpu-probe）と同じファイルを読む：
  paired（あればペアリング済み）・hello（ok・version・silent）・holders（生きている PID があれば答えない）
答えるのは ListDevices・ReadPairRecord・Connect（50060 番の受け手）だけ。
"""
import argparse
import os
import socket
import struct
import threading

RECV_PORT = 50060
UDID = '00008110-001234567890ABCD'


def alive(pid):
    try:
        with open('/proc/%d/status' % pid) as f:
            for line in f:
                if line.startswith('State:'):
                    return 'Z' not in line.split(':', 1)[1].split()[0]
    except OSError:
        return False
    return False


def recv_exact(c, n):
    b = b''
    while len(b) < n:
        x = c.recv(n - len(b))
        if not x:
            raise EOFError
        b += x
    return b


def plist(body):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0">' + body + '</plist>\n').encode()


def send_plist(c, tag, body):
    d = plist(body)
    c.sendall(struct.pack('<IIII', 16 + len(d), 1, 8, tag) + d)


def result(c, tag, n):
    send_plist(c, tag, '<dict><key>MessageType</key><string>Result</string><key>Number</key><integer>%d</integer></dict>' % n)


def state(a, name, default=''):
    try:
        with open(os.path.join(a.state, name)) as f:
            return f.read().strip()
    except OSError:
        return default


def hello_mode(a):
    for p in state(a, 'holders').split():
        if p.isdigit() and alive(int(p)):
            return 'silent'
    return state(a, 'hello', 'ok')


def serve_receiver(c, a):
    c.settimeout(0.5)
    try:
        recv_exact(c, 12)
    except (EOFError, OSError):
        return
    mode = hello_mode(a)
    if mode == 'ok':
        payload = bytearray(1400)
        nb = b'Apple A17 Pro GPU (fake)'
        payload[20:20 + len(nb)] = nb
        struct.pack_into('<I', payload, 1304, 1)
        struct.pack_into('<Q', payload, 1308, 8 * 1024 ** 3)
        struct.pack_into('<I', payload, 1316, 1)
        c.sendall(struct.pack('<iI', 0, len(payload)) + bytes(payload))
    elif mode == 'version':
        c.sendall(struct.pack('<iI', -9, 0))
    while True:
        try:
            if not c.recv(1):
                break
        except socket.timeout:
            continue
        except OSError:
            break


def handle(c, a):
    try:
        c.settimeout(10)
        while True:
            _len, _v, _r, tag = struct.unpack('<IIII', recv_exact(c, 16))
            body = recv_exact(c, _len - 16).decode('utf-8', 'replace')
            if '<string>ListDevices</string>' in body:
                send_plist(c, tag, '<dict><key>DeviceList</key><array><dict><key>DeviceID</key><integer>1</integer>'
                           '<key>MessageType</key><string>Attached</string><key>Properties</key><dict>'
                           '<key>ConnectionType</key><string>USB</string><key>ConnectionSpeed</key><integer>480000000</integer>'
                           '<key>DeviceID</key><integer>1</integer><key>ProductID</key><integer>4776</integer>'
                           '<key>SerialNumber</key><string>' + UDID + '</string></dict></dict></array></dict>')
            elif '<string>ReadPairRecord</string>' in body:
                if os.path.exists(os.path.join(a.state, 'paired')):
                    send_plist(c, tag, '<dict><key>PairRecordData</key><data>ZmFrZQ==</data></dict>')
                else:
                    result(c, tag, 2)
            elif '<string>Connect</string>' in body:
                import re
                m = re.search(r'<key>PortNumber</key>\s*<integer>(\d+)</integer>', body)
                n = int(m.group(1)) if m else 0
                port = ((n & 0xFF) << 8) | ((n >> 8) & 0xFF)
                if port == RECV_PORT:
                    result(c, tag, 0)
                    serve_receiver(c, a)
                    return
                result(c, tag, 3)
            else:
                result(c, tag, 1)
    except (EOFError, OSError, struct.error):
        pass
    finally:
        try:
            c.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--socket')
    ap.add_argument('--state')
    ap.add_argument('--stale')
    a = ap.parse_args()
    if a.stale:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.bind(a.stale)
        s.close()  # ファイルは残る。待つ者はいない（usbmuxd が落ちたあとの形）
        return
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(a.socket)
    s.listen(8)
    while True:
        c, _ = s.accept()
        threading.Thread(target=handle, args=(c, a), daemon=True).start()


if __name__ == '__main__':
    main()
