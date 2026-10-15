# -*- coding: utf-8 -*-
# iPhone・Android の熱とメモリの最新値を、ドライバが書いたファイルから読んで表にする。
#   ドライバ側：環境変数 IPHONE_VK_STATUS_FILE=パス（約 2 秒ごとに JSON 1 行を書く。パスの {pid} はプロセス番号になる）
#   python iphone_status.py 状態1.json 状態2.json ...        1 回表にする
#   python iphone_status.py --watch 状態1.json ...           2 秒ごとに描き直す（Ctrl+C で止める）
#   python iphone_status.py --json 状態1.json ...            台ごとの値と古さを JSON で出す（別の道具が読む用）
# 6 秒以上更新が無いファイルは「古い」（つながりが切れた・ドライバが終わった）。値は信用しない
# 項目：thermal 0〜3（nominal・fair・serious・critical）・avail_mb（使えるメモリ。iPhone は os_proc_available_memory）・
#       temp_c（Android の電池の温度。iPhone は温度を教えてくれないので null）・alert（serious 以上かメモリが少ない）
import glob, json, os, sys, time

STALE_S = 6


def read(path):
    try:
        with open(path, 'rb') as f:
            j = json.loads(f.read().decode('utf-8'))
    except (OSError, ValueError):
        return None
    j['age'] = round(time.time() - j.get('ts', 0), 1)
    j['stale'] = j['age'] > STALE_S
    return j


def rows(paths):
    out = []
    for p in paths:
        for q in sorted(glob.glob(p)) or [p]:
            j = read(q)
            out.append((q, j))
    return out


def fmt(paths):
    lines = ['%-28s %-8s %-9s %7s %6s %5s  %s' % ('ファイル', '種類', '熱', '残りMB', '温度', '古さ', '状態')]
    for q, j in rows(paths):
        name = os.path.basename(q)[:28]
        if j is None:
            lines.append('%-28s 読めない' % name)
            continue
        av = j.get('avail_mb')
        tc = j.get('temp_c')
        flag = '古い' if j['stale'] else ('警告' if j.get('alert') else 'ok')
        lines.append('%-28s %-8s %-9s %7s %6s %4.0fs  %s' % (name, j.get('platform', '?'), j.get('thermal_name', '?'),
                     '-' if av is None else av, '-' if tc is None else '%.1f' % tc, j['age'], flag))
    return '\n'.join(lines)


def main():
    a = sys.argv[1:]
    watch = '--watch' in a
    js = '--json' in a
    paths = [x for x in a if not x.startswith('--')]
    if not paths:
        print(__doc__)
        return 2
    if js:
        print(json.dumps({q: j for q, j in rows(paths)}, ensure_ascii=False))
        return 0
    try:
        while True:
            if watch:
                print('\x1b[2J\x1b[H', end='')
            print(fmt(paths))
            if not watch:
                return 0
            time.sleep(2)
    except KeyboardInterrupt:
        return 0


if __name__ == '__main__':
    sys.exit(main())
