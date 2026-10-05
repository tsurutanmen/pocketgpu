"""USB でつないだ iPhone との実際の速さを測る。

試しのファイルを iPhone の「メディア」の一番上に書いて、読み戻して、消す。iPhone の写真などには触らない。
"""
import os
import struct
import time

import iphone_crashlogs as cl

NAME = "/usb_speed_test.bin"
OP_FILE_WRITE, OP_REMOVE_PATH = 16, 8
MB = 1 << 20


def write_file(afc, path, data, chunk=4 * MB):
    d, _ = afc.call(cl.OP_FILE_OPEN, struct.pack("<Q", 3) + path.encode() + b"\0")  # 3 = 書き込み（新規）
    fh = struct.unpack("<Q", d[:8])[0]
    for i in range(0, len(data), chunk):
        afc.call(OP_FILE_WRITE, struct.pack("<Q", fh), data[i:i + chunk])
    afc.call(cl.OP_FILE_CLOSE, struct.pack("<Q", fh))


def main():
    afc = cl.Afc(cl.lockdown_service("com.apple.afc"))
    data = os.urandom(256 * MB)
    t = time.time()
    write_file(afc, NAME, data)
    w = time.time() - t
    t = time.time()
    back = afc.read(NAME)
    r = time.time() - t
    afc.call(OP_REMOVE_PATH, NAME.encode() + b"\0")
    ok = back == data
    print(f"PC → iPhone  {len(data) / MB / w:6.1f} MB/s  ({w:.1f} 秒で 256MB)")
    print(f"iPhone → PC  {len(back) / MB / r:6.1f} MB/s  ({r:.1f} 秒で 256MB)  中身の一致: {ok}")
    print("試しのファイルは消した")


if __name__ == "__main__":
    main()
