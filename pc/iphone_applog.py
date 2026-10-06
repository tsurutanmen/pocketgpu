"""「計算の受け手」がためた記録（ggml と Vulkan の受け手）を USB で読む。

アプリは USB（iPhone の中の 127.0.0.1）の 50059 番で待っていて、つながると記録を全部書いて切る。
os_log は USB の記録の中継（syslog_relay）に出てこないので、こちらを使う。

  python iphone_applog.py           # 全部
  python iphone_applog.py vk:       # この文字を含む行だけ
"""
import sys

import usb_forward as uf


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    s = uf.connect_device(uf.usb_device_id(), 50059)
    data = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    text = data.decode("utf-8", errors="replace")
    want = sys.argv[1] if len(sys.argv) > 1 else None
    for line in text.splitlines():
        if want is None or want in line:
            print(line)


if __name__ == "__main__":
    main()
