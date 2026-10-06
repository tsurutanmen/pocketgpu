"""わざと遅い通り道。iPhone をつながずに、USB や Wi-Fi の遅さを PC の中で真似する。

  python slow_link.py --listen 50071 --to 50070 --rtt 1 --mbps 80     # USB くらい
  python slow_link.py --listen 50072 --to 50070 --rtt 31 --mbps 20    # Wi-Fi＋Tailscale くらい

片道に rtt/2 ミリ秒の遅れを足し、1 秒に mbps メガバイトまでしか流さない（向きごとに別々）。
"""
import argparse
import heapq
import socket
import threading
import time


def pipe(src, dst, delay, rate):
    """src から読んだものを、遅れと速さの上限をつけて dst に書く。"""
    q = []
    cv = threading.Condition()
    done = [False]
    seq = [0]

    def reader():
        free_at = 0.0  # 通り道が空く時刻（速さの上限）
        try:
            while True:
                data = src.recv(1 << 16)
                if not data:
                    break
                now = time.perf_counter()
                free_at = max(free_at, now) + len(data) / rate
                due = max(now + delay, free_at)
                with cv:
                    heapq.heappush(q, (due, seq[0], data))
                    seq[0] += 1
                    cv.notify()
        except OSError:
            pass
        with cv:
            done[0] = True
            cv.notify()

    def writer():
        try:
            while True:
                with cv:
                    while not q and not done[0]:
                        cv.wait()
                    if not q and done[0]:
                        break
                    due = q[0][0]
                wait = due - time.perf_counter()
                if wait > 0.003:
                    with cv:
                        cv.wait(wait - 0.002)
                    continue
                # Windows のタイマーは粗いので、最後の数ミリ秒は回って待つ
                while time.perf_counter() < due:
                    pass
                with cv:
                    _, _, data = heapq.heappop(q)
                dst.sendall(data)
        except OSError:
            pass
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    threading.Thread(target=reader, daemon=True).start()
    threading.Thread(target=writer, daemon=True).start()


def main():
    try:
        import ctypes
        ctypes.windll.winmm.timeBeginPeriod(1)  # Windows のタイマーを 1 ms 刻みに
    except (AttributeError, OSError):
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", type=int, required=True)
    ap.add_argument("--to", type=int, required=True)
    ap.add_argument("--rtt", type=float, default=1.0, help="往復の遅れ（ミリ秒）")
    ap.add_argument("--mbps", type=float, default=80.0, help="速さの上限（メガバイト毎秒）")
    a = ap.parse_args()
    delay, rate = a.rtt / 2000.0, a.mbps * 1e6
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", a.listen))
    srv.listen(8)
    print(f"127.0.0.1:{a.listen} → 127.0.0.1:{a.to}（往復 {a.rtt} ms・{a.mbps} MB/s）", flush=True)
    while True:
        c, _ = srv.accept()
        u = socket.create_connection(("127.0.0.1", a.to))
        for s in (c, u):
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        pipe(c, u, delay, rate)
        pipe(u, c, delay, rate)


if __name__ == "__main__":
    main()
