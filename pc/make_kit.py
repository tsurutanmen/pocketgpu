# 試す人に配る 1 人分のまとめ（Windows 側＋iPhone のアプリ）を作る。
#   python make_kit.py redmadog              → D:\iphone-rpc\tester_kit\PocketGPU-redmadog.zip
# ドライバ 2 つ（64・32 ビット）の「#」32 文字を、その人の印（名前＋乱数）に書き換える。長さは変えない。
# iPhone のアプリの IPA にも同じ印のファイルを足す（試す人が自分の Apple ID で署名し直すので、中を足しても入る）。
# 誰に何を渡したかは tester_kit\ledger.csv に足す。配った物が外に出たら、印とハッシュで誰の分かを見分ける
import argparse, csv, datetime, hashlib, os, re, secrets, shutil, zipfile

KITBUILD = r'D:\iphone-rpc\kitbuild'        # vk/build_win.sh D:/iphone-rpc/kitbuild icd icd32 tools で組んだ物
DXVK = r'D:\iphone-rpc\dl\dxvk-macos-1103\x\dxvk-macOS-async-v1.10.3-20230507'
DXVK_LICENSE = r'D:\iphone-rpc\vkbuild\dxvk3\LICENSE'
IPA_DIR = r'D:\iphone-rpc\ipa\serve'
OUT = r'D:\iphone-rpc\tester_kit'
PLACE = b'PGKIT-MARK:' + b'#' * 32

LAUNCH = r'''@echo off
rem PocketGPU: run one app or game on the iPhone GPU.
rem   pocketgpu.cmd "C:\path\to\game.exe"
rem Only the app started from this file sees the iPhone GPU. Nothing is written to the registry.
setlocal
set "VK_ADD_DRIVER_FILES=%~dp0iphone_icd.json;%~dp0x86\iphone_icd.json"
if "%IPHONE_VK%"=="" set IPHONE_VK=usb
if "%IPHONE_VK_ONLY%"=="" set IPHONE_VK_ONLY=1
if "%IPHONE_VK_ONLY%"=="1" set "VK_LOADER_DRIVERS_SELECT=*iphone_icd*"
set DISABLE_TWITCH_VULKAN_OVERLAY=1
set DISABLE_VULKAN_OBS_CAPTURE=1
set DISABLE_VULKAN_OW_OBS_CAPTURE=1
%*
'''

CHECK = r'''@echo off
rem PocketGPU: check that Windows can see the iPhone GPU.
setlocal
set "VK_ADD_DRIVER_FILES=%~dp0iphone_icd.json;%~dp0x86\iphone_icd.json"
if "%IPHONE_VK%"=="" set IPHONE_VK=usb
"%~dp0vklist.exe"
echo.
echo If you see "Apple ... GPU (... USB)" above, it works.
pause
'''

HERE = os.path.dirname(os.path.abspath(__file__))

# PC の様子を 1 つのテキストにまとめる（kit/report.ps1・ユーザー名・PC 名・IP は入れない）
REPORT = r'''@echo off
rem PocketGPU: write pocketgpu-report.txt about this PC (no user name, PC name or IP).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0report.ps1"
pause
'''

# 手順だけの下書き。言い回しは鶴田さんが直す
README = '''PocketGPU tester kit (draft)
============================

Thank you for testing. Please do not share this kit with anyone else.
Each kit is marked for one person.

What you need
- A Windows 10 or 11 PC (64-bit)
- An iPhone or iPad with a USB cable. So far it has only been tested on an iPhone 17 Pro.
- iTunes for Windows from apple.com (not the Microsoft Store version), so Windows can talk to the iPhone over USB
- A free tool to install the iPhone app with your own Apple ID, such as Sideloadly or AltStore

Steps
1. Install the iPhone app: open RpcServer-*.ipa with Sideloadly (or AltStore) and sign in with your Apple ID.
   With a free Apple ID the app stops working after 7 days; install it again to continue.
2. Open the app on the iPhone and keep the screen on. Plug the iPhone into the PC with USB.
3. On the PC, double-click check.cmd. You should see the iPhone GPU in the list.
4. Start a Vulkan app with:   pocketgpu.cmd "C:\\path\\to\\app.exe"
5. DirectX 11 games: copy d3d11.dll and dxgi.dll from dxvk\\x64 (64-bit games) or dxvk\\x32 (32-bit games)
   next to the game's .exe, then start the game with pocketgpu.cmd. Delete those two files to undo.

Reporting
- In the iPhone app, tap "Send diagnostics" and send the text to us on Discord or by email.
- On the PC, double-click report.cmd. It writes pocketgpu-report.txt (no user name, PC name or IP). Send that too.
- Please also tell us the PC, the app or game, the resolution, and what happened.

If the connection keeps dropping
- Restart the iPhone first, then plug it in again and reopen the app.
- More answers are in FAQ.txt.

Third-party software
- dxvk\\: DXVK (macOS async build 1.10.3, by Gcenx), zlib license. See dxvk\\LICENSE.

--------------------------------------------
PocketGPU 試験用のまとめ（下書き）

試してくれてありがとうございます。このまとめは、ほかの人に渡さないでください。1 人ずつ印が入っています。

要るもの
- Windows 10 か 11 の PC（64 ビット）
- iPhone か iPad と USB のケーブル。今のところ iPhone 17 Pro でだけ確かめています
- apple.com の Windows 版の iTunes（Microsoft Store 版ではないもの）。Windows が USB で iPhone と話すのに要ります
- 自分の Apple ID で iPhone のアプリを入れる無料の道具。Sideloadly か AltStore など

手順
1. iPhone のアプリを入れる：RpcServer-*.ipa を Sideloadly（か AltStore）で開いて、自分の Apple ID でログインする。
   無料の Apple ID だと 7 日でアプリが開けなくなるので、そのときは入れ直す
2. iPhone でアプリを開いて、画面をつけたままにする。iPhone を USB で PC に差す
3. PC で check.cmd をダブルクリックする。一覧に iPhone の GPU が出れば OK
4. Vulkan のアプリは、pocketgpu.cmd "C:\\アプリの場所\\app.exe" で起動する
5. DirectX 11 のゲームは、dxvk\\x64（64 ビットのゲーム）か dxvk\\x32（32 ビットのゲーム）の d3d11.dll と dxgi.dll を、
   ゲームの .exe の横に写してから、pocketgpu.cmd で起動する。元に戻すときはその 2 つを消す

報告
- iPhone のアプリの「記録を送る」を押して、その文を Discord かメールで送ってください
- PC で report.cmd をダブルクリックすると pocketgpu-report.txt ができます。ユーザー名・PC 名・IP は入りません。これも送ってください
- PC、アプリやゲームの名前、画面の大きさ、何が起きたかも書いてください

つながりが何度も切れるとき
- まず iPhone を再起動して、差し直してから、アプリを開き直してください
- ほかの困りごとは FAQ.txt に書いてあります

ほかの人が作ったもの
- dxvk\\：DXVK（Gcenx さんの macOS async 版 1.10.3）・zlib ライセンス。dxvk\\LICENSE を見てください
'''


def sha256(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def stamp(src, dst, mark):
    data = open(src, 'rb').read()
    if data.count(PLACE) != 1:
        raise SystemExit(f'{src} の印の場所が 1 つではない（{data.count(PLACE)} 個）。kitbuild を組み直す')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(data.replace(PLACE, b'PGKIT-MARK:' + mark))


def newest_ipa():
    def ver(n):
        m = re.match(r'RpcServer-(\d+)\.(\d+)\.(\d+)\.ipa$', n)
        return tuple(map(int, m.groups())) if m else None
    names = [n for n in os.listdir(IPA_DIR) if ver(n)]
    return os.path.join(IPA_DIR, max(names, key=ver))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('who', help='渡す人の名前（Reddit の名前など・英数字）')
    ap.add_argument('--discord-id', default='', help='Discord の bot から作るときの、その人の番号。同じ番号には 2 つめを作らない')
    a = ap.parse_args()
    ledger = os.path.join(OUT, 'ledger.csv')
    if a.discord_id and os.path.exists(ledger):
        with open(ledger, encoding='utf-8') as f:
            if any(row and row[-1] == a.discord_id for row in csv.reader(f)):
                print('ALREADY')
                raise SystemExit(3)
    who = re.sub(r'[^A-Za-z0-9_-]', '_', a.who)[:15]
    mark = (who.ljust(15, '_') + '-' + secrets.token_hex(8)).encode()
    assert len(mark) == 32
    kit = os.path.join(OUT, f'PocketGPU-{who}' + (f'-{a.discord_id[-6:]}' if a.discord_id else ''))
    if os.path.exists(kit):
        raise SystemExit(f'{kit} はもうある。同じ人に作り直すなら、先にそのフォルダと zip を片づける')

    stamp(os.path.join(KITBUILD, 'iphone_icd.dll'), os.path.join(kit, 'iphone_icd.dll'), mark)
    stamp(os.path.join(KITBUILD, 'x86', 'iphone_icd.dll'), os.path.join(kit, 'x86', 'iphone_icd.dll'), mark)
    shutil.copy2(os.path.join(KITBUILD, 'iphone_icd.json'), os.path.join(kit, 'iphone_icd.json'))
    shutil.copy2(os.path.join(KITBUILD, 'x86', 'iphone_icd.json'), os.path.join(kit, 'x86', 'iphone_icd.json'))
    shutil.copy2(os.path.join(KITBUILD, 'vklist.exe'), os.path.join(kit, 'vklist.exe'))
    for arch in ('x64', 'x32'):
        os.makedirs(os.path.join(kit, 'dxvk', arch), exist_ok=True)
        for n in ('d3d11.dll', 'dxgi.dll', 'd3d10core.dll'):
            shutil.copy2(os.path.join(DXVK, arch, n), os.path.join(kit, 'dxvk', arch, n))
    shutil.copy2(DXVK_LICENSE, os.path.join(kit, 'dxvk', 'LICENSE'))
    for name, text in (('pocketgpu.cmd', LAUNCH), ('check.cmd', CHECK), ('report.cmd', REPORT)):
        open(os.path.join(kit, name), 'w', encoding='ascii', newline='\r\n').write(text)
    shutil.copy2(os.path.join(HERE, 'kit', 'report.ps1'), os.path.join(kit, 'report.ps1'))
    shutil.copy2(os.path.join(HERE, 'kit', 'FAQ.txt'), os.path.join(kit, 'FAQ.txt'))
    open(os.path.join(kit, 'README.txt'), 'w', encoding='utf-8-sig', newline='\r\n').write(README)

    # iPhone のアプリ：Payload/RpcServer.app/ に印のファイルを足した IPA
    src_ipa = newest_ipa()
    dst_ipa = os.path.join(kit, os.path.basename(src_ipa))
    with zipfile.ZipFile(src_ipa) as zi, zipfile.ZipFile(dst_ipa, 'w', zipfile.ZIP_DEFLATED) as zo:
        app = None
        for item in zi.infolist():
            zo.writestr(item, zi.read(item.filename))
            m = re.match(r'(Payload/[^/]+\.app/)Info\.plist$', item.filename)
            if m: app = m.group(1)
        if not app:
            raise SystemExit('IPA の中に .app が見つからない')
        zo.writestr(app + 'PGKitMark.txt', mark.decode())

    zpath = kit + '.zip'
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(kit):
            for n in files:
                p = os.path.join(root, n)
                z.write(p, os.path.join(os.path.basename(kit), os.path.relpath(p, kit)))

    new = not os.path.exists(ledger)
    with open(ledger, 'a', newline='', encoding='utf-8') as f:
        w = csv.writer(f)
        if new:
            w.writerow(['日時', '渡す人', '印', 'iphone_icd.dll（64）', 'iphone_icd.dll（32）', 'IPA', 'IPA の元', 'zip', 'Discord の番号'])
        w.writerow([datetime.datetime.now().strftime('%Y-%m-%d %H:%M'), a.who, mark.decode(),
                    sha256(os.path.join(kit, 'iphone_icd.dll')), sha256(os.path.join(kit, 'x86', 'iphone_icd.dll')),
                    sha256(dst_ipa), os.path.basename(src_ipa), sha256(zpath), a.discord_id])
    shutil.rmtree(kit)  # 配るのは zip。中身のフォルダは残さない
    print(f'ZIP={zpath}')
    print(f'MARK={mark.decode()}')


if __name__ == '__main__':
    main()
