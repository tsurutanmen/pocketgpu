# 試す人に配る Linux 版のキット（x86_64 の PC 用・Steam Deck を含む）を作る。Windows のキットの make_kit.py の Linux 版。
#   python make_linux_kit.py brewmaegger           → D:\iphone-rpc\tester_kit\PocketGPU-linux-brewmaegger.tar.gz
#   python make_linux_kit.py --template フォルダ    印を入れないひな形のフォルダ（試験用。IPA・tar.gz・台帳は作らない）
# ドライバ（libiphone_icd.so）と道具（pgpu-probe）は zig cc で glibc 2.31 に合わせて組む（Ubuntu 20.04・Steam Runtime sniper と同じ版）。
# 組んだあと、ELF の中を読んで、要る glibc の版が 2.31 以下か・要るライブラリが libc の仲間だけかを見る（X・JPEG は動くときに dlopen）。
# ドライバの「#」32 文字を、その人の印（名前＋乱数）に書き換える（Windows のキットと同じ印の場所・同じ作り方）。
# iPhone のアプリの IPA にも同じ印のファイルを足す。誰に何を渡したかは tester_kit\ledger_linux.csv に足す。
# キットの中：
#   start-pocketgpu.sh        順にチェックして、足りない物の直し方を出す（Windows の Start-PocketGPU.cmd）
#   run-on-iphone-gpu.sh      1 つのアプリを iPhone の GPU で起動する。Steam の起動オプションにも使う（Windows の pocketgpu.cmd）
#   README.txt                手順（英語と日本語）
#   bin/pgpu-probe            start と run が使う道具（usbmuxd・受け手へのあいさつ・USB の速度・Vulkan の一覧・usbmuxd の受け渡し役）
#   lib/x86_64/libiphone_icd.so と iphone_icd.x86_64.json   ドライバ（json の library_path は横の .so）
#   tools/kit.ini             受け手との約束の版（proto=）とアプリの版（app=）
#   THIRD-PARTY.txt           ドライバに入っている他の人のコード（lz4・bcdec・stb_image）のライセンス
#   RpcServer-*.ipa           iPhone のアプリ（印入り）
# 置き場は環境変数で変えられる：PGKIT_OUT（出す所）・PGKIT_LINUX_BUILD（組んだ物）・PGKIT_IPA_DIR・PGKIT_VK_SRC（app\vk）・ZIG・VK_HEADERS
# 台本の横に kit_linux\ があればそれを、無ければ app\pc\kit_linux\ を使う。app\vk は横の src\vk（work\linuxkit の作業コピー）か ..\vk
import argparse, csv, datetime, hashlib, io, json, os, re, secrets, shutil, struct, subprocess, sys, tarfile, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
KIT_SRC = os.path.join(HERE, 'kit_linux')
VK_SRC = os.environ.get('PGKIT_VK_SRC') or next(
    (p for p in (os.path.join(HERE, 'src', 'vk'), os.path.join(HERE, '..', 'vk')) if os.path.exists(os.path.join(p, 'icd', 'icd.c'))), '')
BUILD = os.environ.get('PGKIT_LINUX_BUILD', r'D:\iphone-rpc\kitbuild_linux')
IPA_DIR = os.environ.get('PGKIT_IPA_DIR', r'D:\iphone-rpc\ipa\serve')
OUT = os.environ.get('PGKIT_OUT', r'D:\iphone-rpc\tester_kit')
ZIG = os.environ.get('ZIG') or shutil.which('zig') or r'D:\tools\zig-x86_64-windows-0.17.0\zig.exe'
VK_HEADERS = os.environ.get('VK_HEADERS') or next(
    (p for p in (r'D:\iphone-rpc\third_party\Vulkan-Headers\include', os.path.join(VK_SRC, '..', '..', 'third_party', 'Vulkan-Headers', 'include'))
     if os.path.exists(os.path.join(p, 'vulkan', 'vulkan.h'))), '')
TARGET = 'x86_64-linux-gnu.2.31'
GLIBC_MAX = (2, 31)
NEEDED_OK = {'libc.so.6', 'libpthread.so.0', 'libdl.so.2', 'libm.so.6', 'ld-linux-x86-64.so.2', 'librt.so.1'}
PLACE = b'PGKIT-MARK:' + b'#' * 32
JSON_NAME = 'iphone_icd.x86_64.json'  # VK_LOADER_DRIVERS_SELECT=*iphone_icd* に当たる名前（Windows と同じ）

ICD_JSON = {
    'file_format_version': '1.0.1',
    'ICD': {'library_path': './libiphone_icd.so', 'library_arch': '64', 'api_version': '1.2.0'},
}

THIRD_PARTY_HEAD = '''Third-party code in lib/x86_64/libiphone_icd.so
================================================
The PocketGPU driver includes code from these projects. Their licenses follow.
- LZ4 by Yann Collet, BSD 2-Clause license
- bcdec by Sergii Kudlai, see its license below
- stb_image by Sean Barrett, public domain or MIT license

ドライバ lib/x86_64/libiphone_icd.so には、次の他の人が作ったコードが含まれています。ライセンスは下のとおりです。
'''


def sha256(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


# ---- ELF の中を読む：要るライブラリ（DT_NEEDED）と、glibc の版（.gnu.version_r） ----
def elf_needs(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF' or d[4] != 2:
        raise SystemExit(f'{path} は 64 ビットの ELF ではない')
    shoff, = struct.unpack_from('<Q', d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3A)
    secs = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize) for i in range(shnum)]
    # (name, type, flags, addr, offset, size, link, info, align, entsize)
    def strat(off, i):
        e = d.index(b'\0', off + i)
        return d[off + i:e].decode()
    needed, versions = [], set()
    for s in secs:
        if s[1] == 6:  # SHT_DYNAMIC
            stroff = secs[s[6]][4]
            for k in range(s[5] // 16):
                tag, val = struct.unpack_from('<qQ', d, s[4] + k * 16)
                if tag == 1:
                    needed.append(strat(stroff, val))
                if tag == 0:
                    break
        if s[1] == 0x6ffffffe:  # SHT_GNU_verneed
            stroff = secs[s[6]][4]
            off = s[4]
            for _ in range(s[7]):
                vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from('<HHIII', d, off)
                a = off + vn_aux
                for _ in range(vn_cnt):
                    vna_hash, vna_flags, vna_other, vna_name, vna_next = struct.unpack_from('<IHHII', d, a)
                    versions.add(strat(stroff, vna_name))
                    a += vna_next
                off += vn_next
                if not vn_next:
                    break
    return needed, versions


def check_elf(path):
    needed, versions = elf_needs(path)
    bad = [n for n in needed if n not in NEEDED_OK]
    if bad:
        raise SystemExit(f'{os.path.basename(path)} が libc の仲間以外を要る：{bad}（X・JPEG・Vulkan は dlopen にする）')
    gl = sorted((tuple(int(x) for x in v[6:].split('.')) for v in versions if v.startswith('GLIBC_')), reverse=True)
    if gl and gl[0] > GLIBC_MAX:
        raise SystemExit(f'{os.path.basename(path)} が glibc {".".join(map(str, gl[0]))} を要る（{".".join(map(str, GLIBC_MAX))} 以下にする）')
    return needed, '.'.join(map(str, gl[0])) if gl else '-'


def build(out):
    if not VK_SRC:
        raise SystemExit('app\\vk が見つからない。PGKIT_VK_SRC で決める')
    if not VK_HEADERS:
        raise SystemExit('Vulkan のヘッダが見つからない。VK_HEADERS で決める')
    if not (os.path.exists(ZIG) or shutil.which(ZIG)):
        raise SystemExit(f'zig が見つからない（{ZIG}）。ZIG で決める')
    os.makedirs(out, exist_ok=True)
    # -g0 -s：デバッグ情報を入れない（組んだ PC のパスや利用者の名前が入るため）。-mcpu=baseline：古い x86_64 でも動く
    cc = [ZIG, 'cc', '-target', TARGET, '-mcpu=baseline', '-O2', '-g0', '-s', '-Wall', '-Wextra', '-Wno-unused-parameter', '-I' + VK_HEADERS]
    jobs = [
        (cc + ['-fPIC', '-shared', '-fvisibility=hidden', '-rdynamic', os.path.join('icd', 'icd.c'),
               '-o', os.path.join(out, 'libiphone_icd.so'), '-lpthread', '-ldl'], 'libiphone_icd.so'),
        (cc + [os.path.join('tools', 'pgpu_probe.c'), '-o', os.path.join(out, 'pgpu-probe'), '-ldl'], 'pgpu-probe'),
    ]
    for cmd, name in jobs:
        print(f'組む：{name}', flush=True)
        r = subprocess.run(cmd, cwd=VK_SRC, capture_output=True, text=True, encoding='utf-8', errors='replace')
        errs = [l for l in (r.stdout + r.stderr).splitlines() if 'error' in l]
        if r.returncode != 0:
            raise SystemExit(f'{name} を組めない：\n' + '\n'.join(errs[:20] or (r.stdout + r.stderr).splitlines()[-20:]))


def stamp(src, dst, mark):
    data = open(src, 'rb').read()
    if data.count(PLACE) != 1:
        raise SystemExit(f'{src} の印の場所が 1 つではない（{data.count(PLACE)} 個）。組み直す')
    open(dst, 'wb').write(data if mark is None else data.replace(PLACE, b'PGKIT-MARK:' + mark))


def proto_version():
    m = re.search(r'#define VR_PROTO_VERSION (\d+)', open(os.path.join(VK_SRC, 'remote', 'proto.h'), encoding='utf-8').read())
    if not m:
        raise SystemExit('proto.h に VR_PROTO_VERSION が無い')
    return int(m.group(1))


def newest_ipa(name='RpcServer'):
    def ver(n):
        m = re.match(re.escape(name) + r'-(\d+)\.(\d+)\.(\d+)\.ipa$', n)
        return tuple(map(int, m.groups())) if m else None
    names = [n for n in os.listdir(IPA_DIR) if ver(n)]
    return os.path.join(IPA_DIR, max(names, key=ver))


def mark_ipa(src, dst, mark):
    with zipfile.ZipFile(src) as zi, zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as zo:
        app = None
        for item in zi.infolist():
            zo.writestr(item, zi.read(item.filename))
            m = re.match(r'(Payload/[^/]+\.app/)Info\.plist$', item.filename)
            if m:
                app = m.group(1)
        if not app:
            raise SystemExit(f'{os.path.basename(src)} の中に .app が見つからない')
        zo.writestr(app + 'PGKitMark.txt', mark.decode())


def third_party_text():
    parts = [THIRD_PARTY_HEAD]
    for name, rel in (('LZ4', ('lz4', 'LICENSE')), ('bcdec', ('bcdec', 'LICENSE')), ('stb_image', ('stb', 'README.txt'))):
        p = os.path.join(VK_SRC, 'third_party', *rel)
        if os.path.exists(p):
            parts.append(f'\n---- {name} ----\n' + open(p, encoding='utf-8', errors='replace').read().replace('\r\n', '\n'))
    return ''.join(parts)


def kit_files(kit):
    # (キットの中の場所, 実行できるか) の一覧。tar.gz の権限に使う
    for root, _, files in os.walk(kit):
        for n in files:
            p = os.path.join(root, n)
            rel = os.path.relpath(p, kit).replace(os.sep, '/')
            yield p, rel, rel.endswith('.sh') or rel == 'bin/pgpu-probe'


def write_tar(kit, path):
    top = os.path.basename(kit)
    now = int(datetime.datetime.now().timestamp())
    with tarfile.open(path, 'w:gz', format=tarfile.PAX_FORMAT) as t:
        dirs = set()
        for p, rel, exe in sorted(kit_files(kit), key=lambda x: x[1]):
            parts = rel.split('/')[:-1]
            for i in range(len(parts) + 1):
                dname = '/'.join([top] + parts[:i])
                if dname not in dirs:
                    dirs.add(dname)
                    ti = tarfile.TarInfo(dname)
                    ti.type, ti.mode, ti.mtime = tarfile.DIRTYPE, 0o755, now
                    t.addfile(ti)
            data = open(p, 'rb').read()
            ti = tarfile.TarInfo(top + '/' + rel)
            ti.size, ti.mode, ti.mtime = len(data), 0o755 if exe else 0o644, now
            t.addfile(ti, io.BytesIO(data))


def lf_copy(src, dst, mode_text=True):
    # .sh と README は LF で（Windows で開いても化けないよう README は UTF-8 の BOM なし）
    data = open(src, 'rb').read()
    if mode_text:
        data = data.replace(b'\r\n', b'\n')
    open(dst, 'wb').write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('who', nargs='?', default='', help='渡す人の名前（Discord の名前など・英数字）。--template のときは要らない')
    ap.add_argument('--discord-id', default='', help='Discord の bot から作るときの、その人の番号。同じ番号には 2 つめを作らない')
    ap.add_argument('--template', metavar='DIR', help='印を入れないひな形をこのフォルダに作る（試験用）。IPA・tar.gz・台帳は作らない')
    ap.add_argument('--no-build', action='store_true', help='組まずに PGKIT_LINUX_BUILD にある物を使う')
    ap.add_argument('--no-ipa', action='store_true', help='iPhone のアプリ（IPA）を入れない（試験用）')
    a = ap.parse_args()
    ledger = os.path.join(OUT, 'ledger_linux.csv')
    if a.template:
        a.no_ipa = True
        mark = None
        kit = os.path.abspath(a.template)
        if os.path.exists(kit) and os.listdir(kit):
            raise SystemExit(f'{kit} が空ではない。ひな形は空のフォルダに作る')
    else:
        if not a.who:
            raise SystemExit('渡す人の名前が要る。ひな形を作るなら --template フォルダ')
        if a.discord_id and os.path.exists(ledger):
            with open(ledger, encoding='utf-8') as f:
                if any(row and row[-1] == a.discord_id for row in csv.reader(f)):
                    print('ALREADY')
                    raise SystemExit(3)
        who = re.sub(r'[^A-Za-z0-9_-]', '_', a.who)[:15]
        mark = (who.ljust(15, '_') + '-' + secrets.token_hex(8)).encode()
        assert len(mark) == 32
        kit = os.path.join(OUT, f'PocketGPU-linux-{who}' + (f'-{a.discord_id[-6:]}' if a.discord_id else ''))
        if os.path.exists(kit) or os.path.exists(kit + '.tar.gz'):
            raise SystemExit(f'{kit} はもうある。同じ人に作り直すなら、先にそのフォルダと tar.gz を片づける')
    if not os.path.isdir(KIT_SRC):
        raise SystemExit(f'{KIT_SRC} が無い')
    src_ipa = None
    if not a.no_ipa:
        try:
            src_ipa = newest_ipa()
        except (OSError, ValueError):
            raise SystemExit(f'{IPA_DIR} に RpcServer-*.ipa が無い。入れないなら --no-ipa')

    if not a.no_build:
        build(BUILD)
    so, probe = os.path.join(BUILD, 'libiphone_icd.so'), os.path.join(BUILD, 'pgpu-probe')
    for p in (so, probe):
        if not os.path.exists(p):
            raise SystemExit(f'{p} が無い。--no-build を外して組む')
        needed, gl = check_elf(p)
        print(f'{os.path.basename(p)}：要るライブラリ {", ".join(needed)}・glibc {gl} まで', flush=True)

    os.makedirs(os.path.join(kit, 'lib', 'x86_64'), exist_ok=True)
    os.makedirs(os.path.join(kit, 'bin'), exist_ok=True)
    os.makedirs(os.path.join(kit, 'tools'), exist_ok=True)
    stamp(so, os.path.join(kit, 'lib', 'x86_64', 'libiphone_icd.so'), mark)
    with open(os.path.join(kit, 'lib', 'x86_64', JSON_NAME), 'w', encoding='ascii', newline='\n') as f:
        json.dump(ICD_JSON, f, indent=4)
        f.write('\n')
    shutil.copyfile(probe, os.path.join(kit, 'bin', 'pgpu-probe'))
    for n in ('start-pocketgpu.sh', 'run-on-iphone-gpu.sh', 'README.txt'):
        lf_copy(os.path.join(KIT_SRC, n), os.path.join(kit, n))
    try:
        app_ver = re.match(r'RpcServer-(.+)\.ipa$', os.path.basename(src_ipa or newest_ipa())).group(1)
    except (OSError, ValueError, AttributeError):
        app_ver = ''
    with open(os.path.join(kit, 'tools', 'kit.ini'), 'w', encoding='ascii', newline='\n') as f:
        f.write(f'proto={proto_version()}\napp={app_ver}\nplatform=linux-x86_64\n')
    with open(os.path.join(kit, 'THIRD-PARTY.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(third_party_text())
    dst_ipa = None
    if src_ipa:
        dst_ipa = os.path.join(kit, os.path.basename(src_ipa))
        mark_ipa(src_ipa, dst_ipa, mark)

    if a.template:
        # ひな形は、Linux で展開したときと同じく実行の印を付けておけないので、tar.gz も横に作る（試験で使う）
        tpath = kit.rstrip('\\/') + '.tar.gz'
        write_tar(kit, tpath)
        print(f'TEMPLATE={kit}')
        print(f'TAR={tpath}')
        return

    tpath = kit + '.tar.gz'
    write_tar(kit, tpath)
    new = not os.path.exists(ledger)
    with open(ledger, 'a', newline='', encoding='utf-8') as f:
        w = csv.writer(f)
        if new:
            w.writerow(['日時', '渡す人', '印', 'libiphone_icd.so', 'IPA', 'IPA の元', 'tar.gz', 'Discord の番号'])
        w.writerow([datetime.datetime.now().strftime('%Y-%m-%d %H:%M'), a.who, mark.decode(),
                    sha256(os.path.join(kit, 'lib', 'x86_64', 'libiphone_icd.so')),
                    sha256(dst_ipa) if dst_ipa else '', os.path.basename(src_ipa) if src_ipa else '', sha256(tpath), a.discord_id])
    shutil.rmtree(kit)  # 配るのは tar.gz。中身のフォルダは残さない
    print(f'ZIP={tpath}')  # Windows の make_kit.py と同じ鍵の名前（bot が読む）。中身は tar.gz
    print(f'MARK={mark.decode()}')


if __name__ == '__main__':
    main()
