"""試験用：偽の adb の「adb forward」が開ける PC のポート。プロセスの名前を adb にして、127.0.0.1 のそのポートで待つだけ。
  python3 adb_listen.py PORT
本物の adb と同じく、ポートが使われていれば終わりのコード 3 で終わる。つないできた相手は、すぐに閉じる。
"""
import ctypes
import socket
import sys


def main():
    port = int(sys.argv[1])
    try:
        ctypes.CDLL(None).prctl(15, b'adb', 0, 0, 0)  # PR_SET_NAME：ss -p で adb と見えるように
    except (OSError, AttributeError):
        pass
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(('127.0.0.1', port))
    except OSError:
        sys.exit(3)
    s.listen(8)
    while True:
        c, _ = s.accept()
        c.close()


if __name__ == '__main__':
    main()
