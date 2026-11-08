# /kit で配るキットを先に作り置きする。配るのは Cloudflare の kit-worker（PC が切れていても配れる）
#   python kit_pool.py make 5     → 印の入ったキットを 5 個作り、Cloudflare の KV に置く。/kit で渡す
#   python kit_pool.py status     → 残りの数と、渡した人の一覧
#   python kit_pool.py retire     → まだ誰にも渡していない作り置きを全部下げる（KV の pool: と中身を消す）。版を上げたときに使う
#   python kit_pool.py sweep      → 渡してから 7 日たったキットの中身を消し、KV の記録に expired を付ける。
#                                   渡した記録は tester_kit\assigned.csv に写す（印から誰の分かを引けるように）
#   python kit_pool.py register PocketGPU-xxx.zip  → 手で作ったキット（make_kit.py 名前）の更新用の合言葉を KV に置く。
#                                   zip の中の tools\update.ini の合言葉と、dll の印を読む
#   python kit_pool.py revoke <Discord の番号>     → その人の更新を止める（KV の upd:<合言葉> に revoked を付ける）
# 作り置きの名前は pool-<乱数>。誰に渡したかは KV の taken:<番号> と assigned.csv で分かる
#
# 置き場：10/12 まで Google ドライブだったが、カメラとマイクの道具が入ったキットを「マルウェアの疑い」で止められたのでやめた。
# 今は kit-worker と同じ KV に置く。KV の 1 つの値は 25 MiB までなので、zip を 24 MiB ずつに分けて blob:<番号>:<i> に置き、
# ダウンロードのリンク https://pocketgpu-kit.YOUR-SUBDOMAIN.workers.dev/dl/<合言葉> で worker がつないで渡す。
# 合言葉は作り置きごとに乱数で、/kit で渡された本人にしか見えない。KV の無料の範囲は 1 GB・書き込み 1 日 1,000 回
#
# 更新（10/12）：make_kit.py がキットの tools\update.ini に更新用の合言葉を書き、UPDATE_TOKEN= で出す。
# ここでそれを KV の upd:<合言葉>（キットの番号と印）に置き、pool: にも utoken として持たせる。/kit で渡すと user: と taken: に写る。
# 7 日の sweep でキットのリンクが切れても、upd: は消さない（一度入れた人には更新を配り続ける）。retire は渡していない分の upd: も消す
#
# 試すとき（本番の KV に書かないため）：PGKIT_KV_LOCAL=1 で、PC の中の試し用の KV（wrangler dev --local が読む所）に読み書きする。
#   名前空間は PGKIT_KV_NS（既定は試験用の番号）。ほかに PGKIT_WRANGLER（wrangler の呼び方）・PGKIT_WORKER_DIR・PGKIT_WORKER_URL・
#   PGKIT_MAKE_KIT・PGKIT_OUT（台帳の置き場。make_kit.py と同じ名前）
import csv, datetime, json, os, re, secrets, subprocess, sys, tempfile, zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
LOCAL = os.environ.get('PGKIT_KV_LOCAL') == '1'
WORKER = os.environ.get('PGKIT_WORKER_DIR', os.path.join(ROOT, '..', 'kit-worker'))
WORKER_URL = os.environ.get('PGKIT_WORKER_URL', 'https://pocketgpu-kit.YOUR-SUBDOMAIN.workers.dev')
MAKE_KIT = os.environ.get('PGKIT_MAKE_KIT', os.path.join(ROOT, '..', '..', 'pc', 'make_kit.py'))
PROD_NS = os.environ.get('PGKIT_KV_PROD_NS', 'YOUR-KV-NAMESPACE-ID')
NS = os.environ.get('PGKIT_KV_NS', '0123456789abcdef0123456789abcdef') if LOCAL else PROD_NS
WRANGLER = os.environ.get('PGKIT_WRANGLER', 'npx -y wrangler@4')
OUT = os.environ.get('PGKIT_OUT', r'D:\iphone-rpc\tester_kit')
LEDGER = os.path.join(OUT, 'ledger.csv')
ASSIGNED = os.path.join(OUT, 'assigned.csv')
KEEP_DAYS = 7
CHUNK = 24 * 1024 * 1024
if LOCAL and NS == PROD_NS:
    raise SystemExit('PGKIT_KV_LOCAL=1 のときに本番の名前空間は使えない')


def run(cmd, timeout=600, cwd=None):
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=timeout, cwd=cwd,
                       env=dict(os.environ, PYTHONIOENCODING='utf-8', WRANGLER_SEND_METRICS='false'), shell=isinstance(cmd, str))
    if r.returncode != 0:
        raise RuntimeError(f'{cmd if isinstance(cmd, str) else cmd[0]}: ' + (r.stdout + r.stderr)[-800:])
    return r.stdout


def kv(*args):
    where = f'--local --persist-to "{os.path.join(WORKER, ".wrangler", "state")}"' if LOCAL else '--remote'
    return run(f'{WRANGLER} kv ' + ' '.join(args) + f' --namespace-id {NS} {where}', 600, WORKER)


def kv_put(key, value):
    # 値は一時ファイルから渡す（JSON の引用符を shell に通さない）
    tmp = os.path.join(WORKER, '.kv_value.json')
    with open(tmp, 'w', encoding='utf-8') as f:
        f.write(json.dumps(value, ensure_ascii=False))
    try:
        kv('key', 'put', key, '--path', tmp)
    finally:
        os.remove(tmp)


def kv_bulk_put(entries):
    # まとめて書く（1 つずつ書くより速い。数えられる書き込みの回数は同じ）。entries は {key, value, base64?} の並び
    if not entries:
        return
    fd, tmp = tempfile.mkstemp(suffix='.json', dir=WORKER)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            json.dump(entries, f, ensure_ascii=False)
        kv('bulk', 'put', f'"{tmp}"')
    finally:
        os.remove(tmp)


def kv_bulk_delete(keys):
    if not keys:
        return
    fd, tmp = tempfile.mkstemp(suffix='.json', dir=WORKER)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            json.dump(list(keys), f)
        kv('bulk', 'delete', f'"{tmp}"', '--force')
    finally:
        os.remove(tmp)


def kv_list(prefix):
    out = kv('key', 'list', '--prefix', prefix)
    return [k['name'] for k in json.loads(out[out.index('['):])]


def kv_get(key):
    return json.loads(kv('key', 'get', key))


def mark_of(name):
    try:
        with open(LEDGER, encoding='utf-8') as f:
            rows = [r for r in csv.reader(f) if len(r) > 2 and r[1] == name]
    except OSError:
        return ''
    return rows[-1][2] if rows else ''


def put_blobs(kid, zp):
    # zip を CHUNK ずつに分けて KV に置く。分けた数を返す
    tmp = os.path.join(WORKER, '.kv_blob.bin')
    n = 0
    try:
        with open(zp, 'rb') as f:
            while True:
                b = f.read(CHUNK)
                if not b:
                    break
                with open(tmp, 'wb') as o:
                    o.write(b)
                kv('key', 'put', f'blob:{kid}:{n}', '--path', tmp)
                n += 1
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return n


def drop_blobs(kid, chunks, token):
    for i in range(chunks):
        kv('key', 'delete', f'blob:{kid}:{i}')
    if token:
        kv('key', 'delete', f'dl:{token}')


def info_of(rec):
    # pool: と taken: の記録から（番号・分けた数・合言葉）を出す。合言葉はリンクの後ろ
    token = rec.get('token') or rec.get('link', '').rsplit('/dl/', 1)[-1]
    chunks = rec.get('chunks')
    if chunks is None and token:
        try:
            chunks = kv_get(f'dl:{token}')['chunks']
        except Exception:
            chunks = 0
    return rec.get('kit'), chunks or 0, token


def make(n):
    for _ in range(n):
        kid = secrets.token_hex(4)
        name = f'pool-{kid}'
        out = run([sys.executable, MAKE_KIT, name], 600)
        zp = re.search(r'^ZIP=(.+)$', out, re.M).group(1).strip()
        m = re.search(r'^MARK=(.+)$', out, re.M)
        mark = m.group(1).strip() if m else mark_of(name)
        u = re.search(r'^UPDATE_TOKEN=([A-Za-z0-9_-]{16,64})$', out, re.M)
        utoken = u.group(1) if u else ''
        size = os.path.getsize(zp)
        chunks = put_blobs(kid, zp)
        token = secrets.token_urlsafe(24)
        kv_put(f'dl:{token}', {'kid': kid, 'chunks': chunks, 'size': size, 'name': os.path.basename(zp)})
        made = datetime.datetime.now().isoformat(timespec='seconds')
        if utoken:
            # 更新用の合言葉。キットのリンクが切れても残す
            kv_put(f'upd:{utoken}', {'kid': kid, 'mark': mark, 'made': made})
        link = f'{WORKER_URL}/dl/{token}'
        kv_put(f'pool:{kid}', {'link': link, 'token': token, 'chunks': chunks, 'mark': mark, 'utoken': utoken,
                               'zip': os.path.basename(zp), 'made': made})
        os.remove(zp)  # 手元の写しは消す（KV と台帳に残る）
        print('作り置いた', kid, f'{size / 1048576:.1f} MiB・{chunks} 個に分けた', '・更新あり' if utoken else '・更新なし')


def retire():
    # 渡した物（taken: と user:）には触らない。pool: に残っている物だけ。誰にも渡していないので、更新用の合言葉も消す
    for key in kv_list('pool:'):
        kid = key[5:]
        rec = kv_get(key)
        _, chunks, token = info_of(dict(rec, kit=kid))
        if 'drive.google.com' not in rec.get('link', ''):
            drop_blobs(kid, chunks, token)
        if rec.get('utoken'):
            kv('key', 'delete', f'upd:{rec["utoken"]}')
        kv('key', 'delete', key)
        print('下げた', kid)


def status():
    pool = kv_list('pool:')
    taken = kv_list('taken:')
    print(f'残り {len(pool)} 個・渡した {len(taken)} 個')
    for k in taken:
        t = kv_get(k)
        print(' ', t['at'][:16], t.get('name'), t['uid'], t['kit'], 'expired' if t.get('expired') else '',
              '更新あり' if t.get('utoken') else '')


def sweep():
    # 7 日たったキットのリンクと中身を消す。更新用の合言葉（upd:）は消さない
    now = datetime.datetime.now(datetime.timezone.utc)
    rows = []
    for k in kv_list('taken:'):
        t = kv_get(k)
        rows.append([t['at'], t.get('name', ''), t['uid'], t['kit'], t.get('mark', ''), t.get('expired', '')])
        at = datetime.datetime.fromisoformat(t['at'].replace('Z', '+00:00'))
        if t.get('expired') or (now - at).days < KEEP_DAYS:
            continue
        if 'drive.google.com' not in t.get('link', ''):
            kid, chunks, token = info_of(t)
            drop_blobs(kid, chunks, token)
        t['expired'] = now.isoformat(timespec='seconds')
        kv_put(k, t)
        u = kv_get(f'user:{t["uid"]}')
        u['expired'] = t['expired']
        kv_put(f'user:{t["uid"]}', u)
        rows[-1][5] = t['expired']
        print('消した', t['kit'], t.get('name'))
    with open(ASSIGNED, 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f)
        w.writerow(['渡した日時', 'Discord の名前', 'Discord の番号', 'キット', '印', '消した日時'])
        w.writerows(sorted(rows))
    print(f'assigned.csv に {len(rows)} 行')


def register(zp):
    # 手で作ったキットの更新用の合言葉を KV に置く。合言葉は zip の tools/update.ini、印は dll の PGKIT-MARK: の後ろ
    with zipfile.ZipFile(zp) as z:
        ini = [n for n in z.namelist() if n.endswith('tools/update.ini')]
        dll = [n for n in z.namelist() if n.endswith('/iphone_icd.dll') and '/x86/' not in n]
        if not ini or not dll:
            raise SystemExit(f'{zp} に tools/update.ini か iphone_icd.dll が無い。更新に対応した make_kit.py で作ったキットか見る')
        m = re.search(r'^token=([A-Za-z0-9_-]{16,64})\s*$', z.read(ini[0]).decode('ascii', 'replace'), re.M)
        data = z.read(dll[0])
    if not m:
        raise SystemExit('update.ini に token= が無い')
    i = data.find(b'PGKIT-MARK:')
    mark = data[i + 11:i + 43].decode('ascii', 'replace') if i >= 0 else ''
    kid = os.path.splitext(os.path.basename(zp))[0]
    kv_put(f'upd:{m.group(1)}', {'kid': kid, 'mark': mark, 'made': datetime.datetime.now().isoformat(timespec='seconds'), 'manual': True})
    print('更新用の合言葉を置いた', kid, mark)


def revoke(uid):
    u = kv_get(f'user:{uid}')
    if not u.get('utoken'):
        raise SystemExit('この人には更新用の合言葉が無い')
    rec = kv_get(f'upd:{u["utoken"]}')
    rec['revoked'] = datetime.datetime.now().isoformat(timespec='seconds')
    kv_put(f'upd:{u["utoken"]}', rec)
    print('更新を止めた', uid, u.get('name', ''))


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'status'
    if cmd == 'retire':
        retire()
    elif cmd == 'make':
        make(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
    elif cmd == 'sweep':
        sweep()
    elif cmd == 'register' and len(sys.argv) > 2:
        register(sys.argv[2])
    elif cmd == 'revoke' and len(sys.argv) > 2:
        revoke(sys.argv[2])
    else:
        status()
