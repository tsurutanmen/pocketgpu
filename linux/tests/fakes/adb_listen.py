"""試験用：偽の adb の「adb forward」が開ける PC のポート。プロセスの名前を adb にして、127.0.0.1 のそのポートで待つだけ。
  python3 adb_listen.py PORT [状態のフォルダ]
本物の adb と同じく、ポートが使われていれば終わりのコード 3 で終わる。つないできた相手は、すぐに閉じる。

状態のフォルダ（偽の adb の $FAKE_DIR/adb）を渡したときだけ、スマホの nc のふり（test-android-speed.sh の試験）もする：
  つながったとき、フォルダに nc.port（この PORT と同じ）と nc.mode があれば、その形で答える。無ければ、これまでどおりすぐに閉じる。
    nc.mode = sink     全部受けて捨てる。相手が閉じたら（受け終わったら）nc.done を作って閉じる。nc_no_close があれば nc.done を作らず、閉じない
    nc.mode = source   nc.bytes バイトの 0 を送って閉じ、nc.done を作る
    nc.mode = echo     1 バイト受けるたびに、rtt_ms ミリ秒待って同じ物を返す。相手が閉じたら nc.done を作る。rtt_die_after に数があれば、その回数だけ返して閉じる
  limit_to（sink）・limit_from（source）に MB/s の数があれば、その速さを超えないように待つ（1 MB = 1048576 バイト）
"""
import ctypes
import os
import socket
import sys
import time

CHUNK = 65536


def read(state, name, default=''):
    try:
        with open(os.path.join(state, name), encoding='ascii') as f:
            return f.read().strip()
    except OSError:
        return default


def limit_of(state, name):
    try:
        v = float(read(state, name))
        return v if v > 0 else 0.0
    except ValueError:
        return 0.0


def pace(n, t0, limit):
    # n バイト流した時点で、limit MB/s を超えていれば、超えない分だけ待つ
    if limit <= 0:
        return
    due = n / (limit * 1048576.0)
    left = due - (time.monotonic() - t0)
    if left > 0:
        time.sleep(left)


def done(state):
    open(os.path.join(state, 'nc.done'), 'w').close()


def serve(c, port, state):
    mode = read(state, 'nc.mode')
    if read(state, 'nc.port') != str(port) or not mode:
        c.close()
        return
    keep = False
    try:
        if mode == 'sink':
            limit = limit_of(state, 'limit_to')
            t0 = time.monotonic()
            n = 0
            while True:
                b = c.recv(CHUNK)
                if not b:
                    break
                n += len(b)
                pace(n, t0, limit)
            if os.path.exists(os.path.join(state, 'nc_no_close')):
                keep = True  # 全部受けたのに、nc.done を作らず閉じもしない（スマホの nc が終わらないふり）
                return
            done(state)
        elif mode == 'source':
            limit = limit_of(state, 'limit_from')
            total = int(read(state, 'nc.bytes', '0') or 0)
            t0 = time.monotonic()
            sent = 0
            zero = bytes(CHUNK)
            while sent < total:
                k = min(CHUNK, total - sent)
                c.sendall(zero[:k])
                sent += k
                pace(sent, t0, limit)
            done(state)
        elif mode == 'echo':
            try:
                rtt = float(read(state, 'rtt_ms', '0')) / 1000.0
            except ValueError:
                rtt = 0.0
            die = read(state, 'rtt_die_after')  # 数があれば、その回数だけ返したところで、閉じる（途中で切れるスマホのふり）
            n = 0
            while True:
                b = c.recv(1)
                if not b:
                    break
                if die.isdigit() and n >= int(die):
                    break
                if rtt > 0:
                    time.sleep(rtt)
                c.sendall(b)
                n += 1
            done(state)
    except OSError:
        pass
    finally:
        if not keep:
            c.close()


def main():
    port = int(sys.argv[1])
    state = sys.argv[2] if len(sys.argv) > 2 else ''
    try:
        ctypes.CDLL(None).prctl(15, b'adb', 0, 0, 0)  # PR_SET_NAME：ss -p で adb と見えるように
    except (OSError, AttributeError):
        pass
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)  # 本物の adb と同じ。直前の接続の TIME_WAIT が残っていても、待ち始められる
    try:
        s.bind(('127.0.0.1', port))
    except OSError:
        sys.exit(3)
    s.listen(8)
    held = []
    while True:
        c, _ = s.accept()
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if state:
            serve(c, port, state)
            if c.fileno() != -1:
                held.append(c)  # 閉じないと決めた接続は、このプロセスが終わるまで持っておく
        else:
            c.close()


if __name__ == '__main__':
    main()
