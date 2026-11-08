"""試験用の偽の usbmuxd と偽の iPhone の受け手。PC の中だけで動く（本物の iPhone も Apple のサービスも使わない）。

  python fake_mux.py --port 28015 --devices usb --recv ok
  --devices usb|none|network   ListDevices の答え
  --recv ok|version|refuse|silent   iPhone の受け手（50060 番）の答え。version は版が違う（-9）、refuse はつながらない、silent は答えない
  --busy-pid N                 PID N のプロセスが生きている間は、受け手が答えない（ゲームなどが握っているふり）
  --listen-after FILE          このファイルができるまで口を開かない（Apple のサービスが止まっているふり）
  --wait-port-free             口が別のプログラムに使われているとき、空くまで 0.2 秒ごとに取り直す（Apple のサービスのふり）
  --name NAME                  受け手が返す GPU の名前
"""
import argparse
import ctypes
import os
import socket
import struct
import sys
import threading
import time

RECV_PORT = 50060


def pid_alive(pid):
    k = ctypes.windll.kernel32
    h = k.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    code = ctypes.c_ulong()
    ok = k.GetExitCodeProcess(h, ctypes.byref(code))
    k.CloseHandle(h)
    return bool(ok) and code.value == 259  # STILL_ACTIVE


def recv_exact(conn, n):
    buf = b''
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf


def plist(body):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
            '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0">' + body + '</plist>\n').encode()


def send_plist(conn, tag, body):
    data = plist(body)
    conn.sendall(struct.pack('<IIII', 16 + len(data), 1, 8, tag) + data)


def device_list(kind):
    if kind == 'none':
        return '<dict><key>DeviceList</key><array/></dict>'
    ctype = 'Network' if kind == 'network' else 'USB'
    return ('<dict><key>DeviceList</key><array><dict><key>DeviceID</key><integer>1</integer><key>MessageType</key><string>Attached</string>'
            '<key>Properties</key><dict><key>ConnectionType</key><string>' + ctype + '</string><key>ConnectionSpeed</key><integer>480000000</integer>'
            '<key>DeviceID</key><integer>1</integer><key>ProductID</key><integer>4776</integer></dict></dict></array></dict>')


def hello_reply(name):
    payload = bytearray(1400)
    nb = name.encode()
    payload[20:20 + len(nb)] = nb
    struct.pack_into('<I', payload, 1304, 1)
    struct.pack_into('<Q', payload, 1308, 8 * 1024 ** 3)
    struct.pack_into('<I', payload, 1316, 1)
    return struct.pack('<iI', 0, len(payload)) + bytes(payload)


def serve_receiver(conn, a):
    conn.settimeout(0.5)
    try:
        recv_exact(conn, 12)
    except (EOFError, OSError):
        return
    mode = a.recv
    if a.busy_pid and pid_alive(a.busy_pid):
        mode = 'silent'
    if mode == 'ok':
        conn.sendall(hello_reply(a.name))
    elif mode == 'version':
        conn.sendall(struct.pack('<iI', -9, 0))
    # silent は答えない。どれも、相手が切るまで待つ
    while True:
        try:
            if not conn.recv(1):
                break
        except socket.timeout:
            continue
        except OSError:
            break


def handle(conn, a):
    try:
        conn.settimeout(10)
        while True:
            head = recv_exact(conn, 16)
            length, _ver, _req, tag = struct.unpack('<IIII', head)
            body = recv_exact(conn, length - 16).decode('utf-8', 'replace')
            if '<string>ListDevices</string>' in body:
                send_plist(conn, tag, device_list(a.devices))
            elif '<string>Connect</string>' in body:
                import re
                m = re.search(r'<key>PortNumber</key>\s*<integer>(\d+)</integer>', body)
                n = int(m.group(1)) if m else 0
                port = ((n & 0xFF) << 8) | ((n >> 8) & 0xFF)
                if port == RECV_PORT and a.recv != 'refuse':
                    send_plist(conn, tag, '<dict><key>MessageType</key><string>Result</string><key>Number</key><integer>0</integer></dict>')
                    serve_receiver(conn, a)
                    return
                send_plist(conn, tag, '<dict><key>MessageType</key><string>Result</string><key>Number</key><integer>3</integer></dict>')
            else:
                send_plist(conn, tag, '<dict><key>MessageType</key><string>Result</string><key>Number</key><integer>1</integer></dict>')
    except (EOFError, OSError):
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--devices', default='usb')
    ap.add_argument('--recv', default='ok')
    ap.add_argument('--busy-pid', type=int, default=0)
    ap.add_argument('--listen-after', default='')
    ap.add_argument('--wait-port-free', action='store_true')
    ap.add_argument('--name', default='Apple A19 Pro GPU (fake)')
    a = ap.parse_args()
    if a.listen_after:
        while not os.path.exists(a.listen_after):
            time.sleep(0.1)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # SO_REUSEADDR は付けない：Windows では付けると、同じ口を二重に開けてしまう
    while True:
        try:
            srv.bind(('127.0.0.1', a.port))
            break
        except OSError:
            if not a.wait_port_free:
                print('bind failed', file=sys.stderr)
                sys.exit(3)
            time.sleep(0.2)
    srv.listen(8)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle, args=(conn, a), daemon=True).start()


if __name__ == '__main__':
    main()
