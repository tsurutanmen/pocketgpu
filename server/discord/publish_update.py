# 一度入れた人に配る更新を作って、Cloudflare の KV に上げる。PC 側（キットのファイル）と iPhone のアプリ（AltStore のソース）の両方
#   python publish_update.py plan                    → 何が変わったかを出すだけ。KV には書かない（読むだけ）
#   python publish_update.py publish --notes-en "..." --notes-ja "..." [--version 0.7.60]
#                                                    → 変わったファイルだけを上げ、最新の版の一覧（urel:latest）を書く
#   python publish_update.py status                  → 今の版と、KV に置いてある中身の数
#   python publish_update.py gc [--keep 2]           → 新しい 2 つの版で使っていない中身を消す
# 既定は PC の中の試し用の KV（wrangler dev --local が読む所）。本番の KV に書くときだけ --remote を付ける
#
# 中身の作り方：make_kit.py --template で、印を入れないキットのひな形を作る。ファイルごとに SHA-256 を取り、
#   - ufile:<sha256>   並び {size, parts}。parts は ['b', 長さ]（次の ublob）・['m', 32]（その人の印）・['c', 4]（印の CRC32）
#   - ublob:<sha256>:<i>  中身。24 MiB ずつ。印の入る所は入れない（worker が渡すときに、その人の印を書く）
# 同じ中身はもう一度上げない（中身の SHA-256 が名前なので、変わったファイルだけが上がる）。
# dll は「PGKIT-MARK:」の後ろの 32 文字に印が入る。IPA は make_kit.py と同じ Payload/<名前>.app/PGKitMark.txt に印が入る：
#   元の IPA の後ろに、圧縮しない PGKitMark.txt（中身は # が 32 個）を足したひな形を作り、中身の 32 バイトと、
#   zip の 2 か所の CRC32（ファイルの頭と、末尾の一覧）を、worker が人ごとに書き換える。大きさは変わらない
# 最新の版の一覧（urel:latest と urel:<番号>）：
#   {rel, version, date, notes:{en,ja}, files:[{path, sha, size, mark_at?}], remove:[...], apps:[...], source:{...}}
# 手元の記録：<PGKIT_OUT>\update\rel-<番号>.json と latest.json（post_update.py が読む）
# 入力は make_kit.py と同じ（KITBUILD・app\pc\kit\・ipa\serve の一番新しい IPA）。PGKIT_KITBUILD・PGKIT_IPA_DIR・PGKIT_MAKE_KIT で変えられる
import argparse, datetime, io, json, os, plistlib, re, shutil, struct, subprocess, sys, tempfile, zipfile, zlib, hashlib, base64

ap = argparse.ArgumentParser()
ap.add_argument('cmd', choices=['plan', 'publish', 'status', 'gc'])
ap.add_argument('--remote', action='store_true', help='本番の KV に読み書きする。付けなければ PC の中の試し用')
ap.add_argument('--version', default='', help='版の名前。無ければ iPhone のアプリの版（PC 側だけ変わったときは 0.7.59-2 のように後ろに数を足す）')
ap.add_argument('--notes-en', default='', help='変わった所。英語')
ap.add_argument('--notes-ja', default='', help='変わった所。日本語')
ap.add_argument('--force', action='store_true', help='変わった物が無くても出す・iPhone のアプリの版が同じでも出す')
ap.add_argument('--keep', type=int, default=2, help='gc で残す版の数')
A = ap.parse_args()
if not A.remote:
    os.environ['PGKIT_KV_LOCAL'] = '1'
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit_pool as kp  # noqa: E402  KV の読み書きは kit_pool.py と同じ道を使う

MAKE_KIT = kp.MAKE_KIT
IPA_DIR = os.environ.get('PGKIT_IPA_DIR', r'D:\iphone-rpc\ipa\serve')
REC_DIR = os.path.join(kp.OUT, 'update')
CHUNK = 24 * 1024 * 1024
BATCH = 48 * 1024 * 1024          # bulk put 1 回に入れる中身の上限（base64 にすると 1.33 倍）
PLACE = b'PGKIT-MARK:' + b'#' * 32
NO_MARK = b'#' * 32

# AltStore のソースに出す文。英語と日本語を 1 つの欄に並べる。文は仮
SOURCE = {
    'name': 'PocketGPU beta',
    'subtitle': 'Updates for PocketGPU testers. / PocketGPU のテスター向けの更新です。',
    'description': 'Updates of the PocketGPU iPhone apps for beta testers. This source is only for you, so please do not share its URL.\n\n'
                   'ベータのテスター向けの、PocketGPU の iPhone のアプリの更新です。このソースはあなた専用です。URL を他の人に教えないでください。',
    'developer': 'PocketGPU',
    'tintColor': '#1B1B1B',
}
APPS = {
    'RpcServer': {
        'subtitle': 'Use this iPhone as a GPU, webcam and mic for a PC. / iPhone を PC の GPU・ウェブカメラ・マイクとして使います。',
        'desc': 'Use this iPhone as a GPU, webcam, microphone and speaker for a Windows PC over USB. Needs the PocketGPU kit on the PC. For beta testers only.\n\n'
                'USB で接続した Windows の PC から、この iPhone を GPU・ウェブカメラ・マイク・スピーカーとして使うためのアプリです。PC に PocketGPU のキットが必要です。ベータのテスター専用です。',
    },
    'PocketCam': {
        'subtitle': 'Webcam and mic for a PC, for iOS 15 and 16. / iOS 15 と 16 用のウェブカメラとマイクです。',
        'desc': 'Use this iPhone as a webcam and microphone for a Windows PC. For iPhones with iOS 15 or 16. On iOS 17 or later, use PocketGPU instead.\n\n'
                'この iPhone を Windows の PC のウェブカメラとマイクとして使うためのアプリです。iOS 15 または 16 の iPhone 用です。iOS 17 以降では PocketGPU を使ってください。',
    },
}
# AltStore で書かなくてよい権限（資料のとおり）
IGNORED_ENTITLEMENTS = {'application-identifier', 'com.apple.developer.team-identifier'}


def sha256(b):
    return hashlib.sha256(b).hexdigest()


# ---- PC 側：ひな形のファイル ----
def build_template():
    tmp = tempfile.mkdtemp(prefix='pgupd-')
    tdir = os.path.join(tmp, 'kit')
    # PGKIT_TEMPLATE_ARGS：make_kit.py に足す引数（例 --no-android）。配るキットから物を外したときに使う
    extra = os.environ.get('PGKIT_TEMPLATE_ARGS', '').split()
    out = kp.run([sys.executable, MAKE_KIT, '--template', tdir] + extra, 900)
    if f'TEMPLATE={tdir}' not in out:
        raise SystemExit('make_kit.py --template がひな形を作らなかった:\n' + out[-800:])
    files = {}
    for root, _, names in os.walk(tdir):
        for n in names:
            p = os.path.join(root, n)
            rel = os.path.relpath(p, tdir).replace(os.sep, '/')
            data = open(p, 'rb').read()
            e = {'path': rel, 'sha': sha256(data), 'size': len(data)}
            c = data.count(PLACE)
            if c > 1:
                raise SystemExit(f'{rel} に印の場所が {c} 個ある')
            if c == 1:
                e['mark_at'] = data.index(PLACE) + len(b'PGKIT-MARK:')
            files[rel] = (e, data)
    shutil.rmtree(tmp, ignore_errors=True)
    return files


# ---- iPhone のアプリ ----
def newest_ipa(prefix):
    def ver(n):
        m = re.match(re.escape(prefix) + r'-(\d+)\.(\d+)\.(\d+)\.ipa$', n)
        return tuple(map(int, m.groups())) if m else None
    names = [n for n in os.listdir(IPA_DIR) if ver(n)]
    return os.path.join(IPA_DIR, max(names, key=ver)) if names else None


def entitlements_of(macho):
    # Mach-O の署名（LC_CODE_SIGNATURE）の中の entitlements の plist。fat なら全部の CPU の分を合わせる
    magic = struct.unpack('>I', macho[:4])[0]
    if magic == 0xcafebabe:
        out = {}
        for i in range(struct.unpack('>I', macho[4:8])[0]):
            _, _, off, size, _ = struct.unpack('>IIIII', macho[8 + 20 * i:28 + 20 * i])
            out.update(entitlements_of(macho[off:off + size]))
        return out
    m = struct.unpack('<I', macho[:4])[0]
    if m not in (0xfeedfacf, 0xfeedface):
        return {}
    ncmds = struct.unpack('<I', macho[16:20])[0]
    p = 32 if m == 0xfeedfacf else 28
    for _ in range(ncmds):
        cmd, size = struct.unpack('<II', macho[p:p + 8])
        if cmd == 0x1d:
            off, sz = struct.unpack('<II', macho[p + 8:p + 16])
            sb = macho[off:off + sz]
            count = struct.unpack('>I', sb[8:12])[0]
            for j in range(count):
                _, boff = struct.unpack('>II', sb[12 + 8 * j:20 + 8 * j])
                bm, bl = struct.unpack('>II', sb[boff:boff + 8])
                if bm == 0xfade7171:
                    return plistlib.loads(sb[boff + 8:boff + bl])
            return {}
        p += size
    return {}


def app_info(data, prefix):
    # Info.plist と実行ファイルの署名から、AltStore のソースに載せる値を出す。アプリの拡張（PlugIns の .appex）も見る
    z = zipfile.ZipFile(io.BytesIO(data))
    names = z.namelist()
    main = [n for n in names if re.match(r'Payload/[^/]+\.app/Info\.plist$', n)]
    if len(main) != 1:
        raise SystemExit(f'{prefix} の IPA に .app が 1 つではない')
    app_dir = main[0][:-len('Info.plist')]
    plists = main + [n for n in names if re.match(re.escape(app_dir) + r'(.+/)?PlugIns/[^/]+\.appex/Info\.plist$', n)]
    privacy, ents = {}, set()
    d0 = None
    for ip in plists:
        d = plistlib.loads(z.read(ip))
        d0 = d0 or d
        privacy.update({k: v for k, v in d.items() if k.endswith('UsageDescription') and isinstance(v, str)})
        exe = ip[:-len('Info.plist')] + d.get('CFBundleExecutable', '')
        if exe in names:
            ents.update(k for k in entitlements_of(z.read(exe)) if k not in IGNORED_ENTITLEMENTS)
    if app_dir + 'PGKitMark.txt' in names:
        raise SystemExit(f'{prefix} の元の IPA に、もう PGKitMark.txt が入っている。印の無い IPA を使う')
    return {
        'name': d0.get('CFBundleDisplayName') or d0.get('CFBundleName') or prefix,
        'bundle': d0['CFBundleIdentifier'],
        'version': d0['CFBundleShortVersionString'],
        'build': str(d0.get('CFBundleVersion', '')),
        'minOS': str(d0.get('MinimumOSVersion', '')) or None,
        'perm': {'entitlements': sorted(ents), 'privacy': dict(sorted(privacy.items()))},
        'app_dir': app_dir,
    }


def ipa_template(data, app_dir):
    # 元の IPA の後ろに、圧縮しない PGKitMark.txt（# が 32 個）を足す。元の中身はそのまま（作り直さない）
    eocd = data.rfind(b'PK\x05\x06')
    if eocd < 0:
        raise SystemExit('IPA の zip の終わりが見つからない')
    _, disk, cd_disk, n_disk, n_total, cd_size, cd_off, clen = struct.unpack('<IHHHHIIH', data[eocd:eocd + 22])
    if disk or cd_disk or cd_off + cd_size != eocd or cd_off == 0xFFFFFFFF:
        raise SystemExit('IPA の zip の形が想定と違う（分割・zip64・一覧の後ろに何かある）')
    name = (app_dir + 'PGKitMark.txt').encode()
    # 日時は Info.plist と同じにする。今の時刻を入れると、同じ IPA から作っても毎回 SHA-256 が変わってしまう
    y, mo, dd, h, mi, s = zipfile.ZipFile(io.BytesIO(data)).getinfo(app_dir + 'Info.plist').date_time
    t = (h << 11) | (mi << 5) | (s // 2)
    d = ((y - 1980) << 9) | (mo << 5) | dd
    crc = zlib.crc32(NO_MARK)
    L = cd_off
    lh = struct.pack('<IHHHHHIIIHH', 0x04034b50, 20, 0, 0, t, d, crc, 32, 32, len(name), 0) + name
    data_at = L + len(lh)
    cd_new = data_at + 32
    ce = struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, (3 << 8) | 20, 20, 0, 0, t, d, crc, 32, 32,
                     len(name), 0, 0, 0, 0, 0o100644 << 16, L) + name
    end = struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, n_disk + 1, n_total + 1, cd_size + len(ce), cd_new, 0)
    out = data[:L] + lh + NO_MARK + data[cd_off:cd_off + cd_size] + ce + end
    crc_at = [L + 14, cd_new + cd_size + 16]
    # 印を入れた形でも zip として読めて、CRC が合うかを見る
    test = bytearray(out)
    sample = b'probe__________-0123456789abcdef'
    test[data_at:data_at + 32] = sample
    for o in crc_at:
        test[o:o + 4] = struct.pack('<I', zlib.crc32(sample))
    with zipfile.ZipFile(io.BytesIO(bytes(test))) as z:
        if z.read(name.decode()) != sample or z.testzip() is not None:
            raise SystemExit('印を入れた IPA が zip として正しくない')
    return out, data_at, crc_at


def build_apps():
    apps = {}
    for prefix in APPS:
        src = newest_ipa(prefix)
        if not src:
            print(f'  {prefix} の IPA が {IPA_DIR} に無いので、載せない')
            continue
        data = open(src, 'rb').read()
        info = app_info(data, prefix)
        expect = f'{prefix}-{info["version"]}.ipa'
        if os.path.basename(src) != expect:
            raise SystemExit(f'{os.path.basename(src)} の中の版は {info["version"]}。ファイル名と合わない')
        tpl, mark_at, crc_at = ipa_template(data, info.pop('app_dir'))
        e = dict(info, prefix=prefix, file=expect, sha=sha256(tpl), size=len(tpl), mark_at=mark_at, crc_at=crc_at,
                 subtitle=APPS[prefix]['subtitle'], desc=APPS[prefix]['desc'])
        apps[prefix] = (e, tpl)
    return apps


# ---- KV に置く形 ----
def layout(e, data):
    # 並び（parts）と、ublob に置く中身（印の所を抜いた残り）
    patches = []
    if e.get('mark_at') is not None:
        patches.append((e['mark_at'], 'm', 32))
    for o in e.get('crc_at') or []:
        patches.append((o, 'c', 4))
    patches.sort()
    parts, blobs, pos = [], [], 0

    def plain(a, b):
        while a < b:
            n = min(CHUNK, b - a)
            parts.append(['b', n])
            blobs.append(data[a:a + n])
            a += n
    for at, kind, n in patches:
        plain(pos, at)
        parts.append([kind, n])
        pos = at + n
    plain(pos, len(data))
    assert sum(p[1] for p in parts) == len(data)
    return {'size': len(data), 'parts': parts}, blobs


def upload(objs):
    # objs: [(sha, entry, data)]。中身を先に、並び（ufile:）を後に書く。書いた回数を返す
    entries, size, writes = [], 0, 0
    index = []
    for sha, e, data in objs:
        info, blobs = layout(e, data)
        for i, b in enumerate(blobs):
            if size + len(b) > BATCH and entries:
                kp.kv_bulk_put(entries)
                writes += len(entries)
                entries, size = [], 0
            entries.append({'key': f'ublob:{sha}:{i}', 'value': base64.b64encode(b).decode(), 'base64': True})
            size += len(b)
        index.append({'key': f'ufile:{sha}', 'value': json.dumps(info)})
    if entries:
        kp.kv_bulk_put(entries)
        writes += len(entries)
    kp.kv_bulk_put(index)
    return writes + len(index)


def kv_json(key):
    try:
        return kp.kv_get(key)
    except Exception:
        return None


def existing_shas():
    try:
        return {k[len('ufile:'):] for k in kp.kv_list('ufile:')}
    except Exception:
        return set()


def plan(prev):
    files = build_template()
    apps = build_apps()
    prev_files = {f['path']: f for f in (prev or {}).get('files', [])}
    prev_apps = {a['prefix']: a for a in (prev or {}).get('apps', [])}
    changed = [p for p, (e, _) in sorted(files.items()) if p not in prev_files or prev_files[p]['sha'] != e['sha']]
    removed = sorted(set(prev_files) - set(files))
    apps_changed = [p for p, (e, _) in apps.items() if p not in prev_apps or prev_apps[p]['sha'] != e['sha']]
    return files, apps, changed, removed, apps_changed, prev_apps


def show(files, apps, changed, removed, apps_changed, prev):
    print(f'今の版: {prev["version"]}（{prev["rel"]} 番目・{prev["date"]}）' if prev else '今の版: まだ出していない')
    print(f'PC 側のファイル {len(files)} 個のうち、変わった物 {len(changed)} 個・{sum(files[p][0]["size"] for p in changed) / 1048576:.2f} MB')
    for p in changed:
        print('   ', p, '（印あり）' if files[p][0].get('mark_at') is not None else '')
    if removed:
        print(f'無くなったファイル {len(removed)} 個（キットでは控えに移す）')
        for p in removed:
            print('   ', p)
    for p, (e, _) in apps.items():
        print(f'iPhone のアプリ {e["file"]}（{e["bundle"]}・build {e["build"]}・iOS {e["minOS"]} 以上・{e["size"] / 1048576:.1f} MB）',
              '変わった' if p in apps_changed else '同じ')
        print('    権限:', ', '.join(e['perm']['entitlements']) or 'なし', '/', ', '.join(e['perm']['privacy']) or 'なし')


def version_label(apps, prev):
    base = apps['RpcServer'][0]['version'] if 'RpcServer' in apps else (prev or {}).get('version', '0')
    pv = (prev or {}).get('version', '')
    if A.version:
        return A.version
    if pv == base:
        return f'{base}-2'
    if pv.startswith(base + '-') and pv[len(base) + 1:].isdigit():
        return f'{base}-{int(pv[len(base) + 1:]) + 1}'
    return base


def cmd_publish(dry):
    prev = kv_json('urel:latest')
    files, apps, changed, removed, apps_changed, prev_apps = plan(prev)
    show(files, apps, changed, removed, apps_changed, prev)
    if not changed and not removed and not apps_changed and not A.force:
        print('変わった物が無いので、出さない')
        return
    # iPhone のアプリは、版か build が変わっていないと AltStore が新しい版に気づかない
    for p in apps_changed:
        e, pa = apps[p][0], prev_apps.get(p)
        if pa and pa['version'] == e['version'] and pa['build'] == e['build'] and not A.force:
            raise SystemExit(f'{p} の中身が変わったのに、版 {e["version"]} と build {e["build"]} が前と同じ。AltStore に Update が出ないので、版を上げて組み直す')
    if dry:
        print('（plan なので KV には書いていない）')
        return
    if not A.notes_en or not A.notes_ja:
        raise SystemExit('--notes-en と --notes-ja の両方が要る（テスターに見せる、変わった所の説明）')
    rel = (prev or {}).get('rel', 0) + 1
    now = datetime.datetime.now().astimezone().isoformat(timespec='seconds')
    have = existing_shas()
    objs, seen = [], set()
    for e, data in list(files.values()) + list(apps.values()):
        if e['sha'] not in have and e['sha'] not in seen:
            objs.append((e['sha'], e, data))
            seen.add(e['sha'])
    print(f'上げる中身 {len(objs)} 個・{sum(len(d) for _, _, d in objs) / 1048576:.1f} MB')
    writes = upload(objs)
    app_list = []
    for p, (e, _) in apps.items():
        pa = prev_apps.get(p)
        if pa and pa['sha'] == e['sha']:
            app_list.append(pa)  # 変わっていないアプリは、前の日付と説明のまま
        else:
            app_list.append(dict(e, date=now, notes=f'{A.notes_en}\n\n{A.notes_ja}'))
    old_remove = set((prev or {}).get('remove', []))
    man = {
        'rel': rel,
        'version': version_label(apps, prev),
        'date': now,
        'notes': {'en': A.notes_en, 'ja': A.notes_ja},
        'files': [files[p][0] for p in sorted(files)],
        'remove': sorted((old_remove | set(removed)) - set(files)),
        'apps': app_list,
        'source': SOURCE,
    }
    kp.kv_bulk_put([{'key': f'urel:{rel}', 'value': json.dumps(man, ensure_ascii=False)}])
    kp.kv_bulk_put([{'key': 'urel:latest', 'value': json.dumps(man, ensure_ascii=False)}])
    writes += 2
    os.makedirs(REC_DIR, exist_ok=True)
    rec = dict(man, changed=changed, removed=removed, apps_changed=apps_changed, kv='remote' if A.remote else 'local')
    for name in (f'rel-{rel}.json', 'latest.json'):
        with open(os.path.join(REC_DIR, name), 'w', encoding='utf-8') as f:
            json.dump(rec, f, ensure_ascii=False, indent=1)
    print(f'出した: 版 {man["version"]}（{rel} 番目）・KV への書き込み {writes} 回・記録 {os.path.join(REC_DIR, f"rel-{rel}.json")}')


def cmd_status():
    man = kv_json('urel:latest')
    if not man:
        print('まだ出していない')
        return
    print(f'今の版: {man["version"]}（{man["rel"]} 番目・{man["date"]}）')
    print(f'PC 側のファイル {len(man["files"])} 個・{sum(f["size"] for f in man["files"]) / 1048576:.1f} MB')
    for a in man.get('apps', []):
        print(f'iPhone のアプリ {a["file"]}・{a["size"] / 1048576:.1f} MB・{a["date"]}')
    keys = existing_shas()
    print(f'KV に置いてある中身 {len(keys)} 個')


def cmd_gc():
    man = kv_json('urel:latest')
    if not man:
        print('まだ出していない')
        return
    keep = set()
    for r in range(man['rel'], max(0, man['rel'] - A.keep), -1):
        m = kv_json(f'urel:{r}')
        if m:
            keep |= {f['sha'] for f in m['files']} | {a['sha'] for a in m.get('apps', [])}
    drop, n_blobs = [], 0
    for sha in sorted(existing_shas() - keep):
        info = kv_json(f'ufile:{sha}')
        nb = sum(1 for p in (info or {}).get('parts', []) if p[0] == 'b')
        drop += [f'ublob:{sha}:{i}' for i in range(nb)] + [f'ufile:{sha}']
        n_blobs += 1
    print(f'残す中身 {len(keep)} 個・消す中身 {n_blobs} 個（キー {len(drop)} 個）')
    for i in range(0, len(drop), 1000):
        kp.kv_bulk_delete(drop[i:i + 1000])


if __name__ == '__main__':
    print('KV:', '本番' if A.remote else f'PC の中の試し用（{kp.WORKER}）')
    if A.cmd == 'plan':
        cmd_publish(True)
    elif A.cmd == 'publish':
        cmd_publish(False)
    elif A.cmd == 'status':
        cmd_status()
    else:
        cmd_gc()
