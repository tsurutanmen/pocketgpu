# カメラの約束（ポート 50064・work/webcam/pc/wcam.h と camset の足し分）を、PC 側から細かく試す小さな道具。
#   python -I wcam_probe.py --addr 127.0.0.1:50531 --secs 10 [--w 640 --h 480 --fps 15 --camera 1 --camera-set]
#          [--live] [--quality 2] [--code 123456] [--len 32|36|40] [--setkbps 1500@3] [--reqkey 4] [--rehello 640x480@5]
# 届いた ACK・EVENT・コマ（数・大きさ・キーフレームに SPS があるか）を書く。H.264 はほどかない
import argparse, socket, struct, sys, time

T_HELLO, T_ACK, T_FRAME, T_EVENT, T_REQ_KEY, T_SET_KBPS, T_PING, T_PONG = 1, 2, 3, 4, 16, 17, 20, 21


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--addr', default='127.0.0.1:50531')
    ap.add_argument('--secs', type=float, default=10)
    for k in ('w', 'h', 'fps', 'kbps', 'gop', 'camera', 'quality', 'code'):
        ap.add_argument('--' + k, type=int, default=0)
    ap.add_argument('--camera-set', action='store_true')
    ap.add_argument('--live', action='store_true')
    ap.add_argument('--len', type=int, default=40, help='HELLO の長さ（32 は古い PC の道具・36 は合言葉まで・40 は camset）')
    ap.add_argument('--setkbps', default='', help='kbps@秒')
    ap.add_argument('--reqkey', type=float, default=0, help='この秒に REQ_KEY')
    ap.add_argument('--rehello', default='', help='WxH@秒 で HELLO を送り直す')
    a = ap.parse_args()
    host, port = a.addr.rsplit(':', 1)
    s = socket.create_connection((host, int(port)), timeout=10)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    flags = (2 if a.camera_set else 0) | (4 if a.live else 0)

    def hello(w, h):
        body = struct.pack('<10I', 1, w, h, a.fps, a.kbps, a.gop, a.camera, flags, a.code, a.quality)[:a.len]
        s.sendall(struct.pack('<II', T_HELLO, len(body)) + body)

    hello(a.w, a.h)
    t0 = time.time()
    s.settimeout(0.2)
    buf = b''
    frames = keys = keys_sps = bytes_ = 0
    sizes = {}
    seqs = []
    done_kbps = done_key = done_re = False
    s.sendall(struct.pack('<II', T_PING, 8) + struct.pack('<Q', int(time.monotonic() * 1e6)))
    while time.time() - t0 < a.secs:
        el = time.time() - t0
        if a.setkbps and not done_kbps and el >= float(a.setkbps.split('@')[1]):
            k = int(a.setkbps.split('@')[0]); s.sendall(struct.pack('<III', T_SET_KBPS, 4, k)); done_kbps = True; print('%.1fs SET_KBPS %d を送った' % (el, k))
        if a.reqkey and not done_key and el >= a.reqkey:
            s.sendall(struct.pack('<II', T_REQ_KEY, 0)); done_key = True; print('%.1fs REQ_KEY を送った' % el)
        if a.rehello and not done_re and el >= float(a.rehello.split('@')[1]):
            w, h = map(int, a.rehello.split('@')[0].split('x')); hello(w, h); done_re = True; print('%.1fs HELLO を送り直した %dx%d' % (el, w, h))
        try:
            d = s.recv(1 << 20)
            if not d:
                print('%.1fs 相手が切った' % el); break
            buf += d
        except socket.timeout:
            continue
        while len(buf) >= 8:
            typ, ln = struct.unpack_from('<II', buf)
            if len(buf) < 8 + ln:
                break
            body = buf[8:8 + ln]; buf = buf[8 + ln:]
            el = time.time() - t0
            if typ == T_ACK:
                v = struct.unpack('<Ii6I', body[:32])
                print('%.1fs ACK version=%d status=%d %dx%d fps=%d kbps=%d camera=%d flags=0x%x' % ((el,) + v))
            elif typ == T_EVENT:
                print('%.1fs EVENT code=%d arg=%d' % ((el,) + struct.unpack('<II', body[:8])))
            elif typ == T_PONG:
                print('%.1fs PONG を受けた（%d バイト）' % (el, len(body)))
            elif typ == T_FRAME:
                cap, sent, seq, fl, w, h = struct.unpack_from('<QQIIII', body)
                nal = body[32:]
                frames += 1; bytes_ += len(nal); seqs.append(seq)
                sizes[(w, h)] = sizes.get((w, h), 0) + 1
                if fl & 1:
                    keys += 1
                    if nal[:5] == b'\x00\x00\x00\x01\x67' or nal[:4] == b'\x00\x00\x01\x67':
                        keys_sps += 1
                    print('%.1fs キーフレーム seq=%d %dx%d %d バイト SPS=%s' % (el, seq, w, h, len(nal), 'あり' if keys_sps == keys else 'なし'))
    el = time.time() - t0
    gaps = sum(1 for x, y in zip(seqs, seqs[1:]) if y != x + 1)
    print('結果 %.1f 秒・コマ %d（%.1f コマ/秒）・キーフレーム %d（SPS つき %d）・%.0f kbps・番号の飛び %d・大きさ %s' %
          (el, frames, frames / el, keys, keys_sps, bytes_ * 8 / 1000 / el, gaps, sizes))
    s.close()


if __name__ == '__main__':
    main()
