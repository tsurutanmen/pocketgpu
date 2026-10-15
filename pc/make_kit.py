# 試す人に配る 1 人分のまとめ（Windows 側＋iPhone のアプリ）を作る。
#   python make_kit.py redmadog              → D:\iphone-rpc\tester_kit\PocketGPU-redmadog.zip
# ドライバ 2 つ（64・32 ビット）の「#」32 文字を、その人の印（名前＋乱数）に書き換える。長さは変えない。
# iPhone のアプリの IPA にも同じ印のファイルを足す（試す人が自分の Apple ID で署名し直すので、中を足しても入る）。
# 誰に何を渡したかは tester_kit\ledger.csv に足す。配った物が外に出たら、印とハッシュで誰の分かを見分ける
# Android の機種を持っている人用に、android\（受け手・start_android.cmd・stop_android.cmd・android_app.cmd・pocketgpu.apk・README_android.txt）と
# pocketgpu_android.cmd・check_android.cmd も入れる。Android の受け手には印を入れない（Android 側は人ごとの印が dll にあれば足りる）。
# Android の記録を集める入口 Send-Android-Log.cmd（中身は tools\android-log.ps1）も、Android の物と一緒に入れる（--no-android のときは入れない）。
# 試すとき：PGKIT_OUT・PGKIT_KITBUILD・--android-dir で置き場を変えられる（台帳は PGKIT_OUT の下に作る）
# 入口は 3 つ：Start-PocketGPU.cmd（順に確かめて足りない物を案内する）と Run-on-iPhone-GPU.cmd（1 つのアプリを iPhone の GPU で起動する）と
# Start-Mic-Camera.cmd（GPU を使わずに、iPhone をマイクとカメラにする）。
# 中身は kit\tools\*.ps1（Windows PowerShell 5.1 だけで動く。Python は要らない）。tools\kit.ini に受け手との約束の版と、アプリの版を書く
# PC アプリ PocketGPU.exe（work/pcapp2）はキットの一番上に置く。自分の横の iphone_icd.json・dxvk\・pocketgpu.cmd・tools\kit.ini を読み、道具は extras\ から探す
# カメラ・マイク・字幕の道具と仮想カメラは extras\ にまとめる。PocketGPU.exe（横・tools\・extras\ を探す）と tools\miccam.ps1（extras\・camera\・一番上を探す）の
# 両方が探す所で、仮想カメラの DLL は camera_setup.exe と同じフォルダに要る（PocketGPU.exe が camera_setup.exe の横から写すため）
# 古い iPhone（iOS 15・16）用の PocketCam-*.ipa も、RpcServer の IPA と同じ印を足して入れる（カメラとマイクだけのアプリ。PC 側は Start-Mic-Camera.cmd）
# 新しく足した物（extras\ の道具・PocketGPU.exe・PocketCam の IPA）は、キットのフォルダを作る前に揃っているか見る
# （途中で止まると同じ人のフォルダが残って、作り直せなくなるため）。入れないときは --no-media・--no-app
# Android の PC 側の口（10/14）：adb forward が PC で待つ口（50300・50064・50063）を Windows が予約していると待てない。android\_common.cmd が
# tools\pgpu_port.ps1 を呼んで、予約の範囲の外の口（47300・47400・47500 から順）を選び、android\port.txt に覚える。ほかの cmd・run.ps1・Fix-PocketGPU も同じ口を読む
# 更新（10/12）：tools\update.ini に、更新を聞く先（url=）と、その人の更新用の合言葉（token=）を書く。合言葉は乱数で、
# 最後に UPDATE_TOKEN= の行で出す（kit_pool.py が KV の upd:<合言葉> に印と一緒に置く）。kit.ini は全員同じ中身なので、
# 更新でそのまま差し替えられるよう、人ごとの物は update.ini に分けた。入口は Update-PocketGPU.cmd（中身は tools\update.ps1）
#   --template フォルダ   印を入れない「ひな形」を作る（publish_update.py が使う）。IPA・update.ini・zip・台帳は作らない。
#                         dll は印の場所（PGKIT-MARK:###…）を残したまま。更新は、このひな形との違いで配る
import argparse, csv, datetime, hashlib, os, re, secrets, shutil, subprocess, zipfile

KITBUILD = os.environ.get('PGKIT_KITBUILD', r'D:\iphone-rpc\kitbuild')        # vk/build_win.sh D:/iphone-rpc/kitbuild icd icd32 tools で組んだ物
DXVK = r'D:\iphone-rpc\dl\dxvk-macos-1103\x\dxvk-macOS-async-v1.10.3-20230507'
DXVK_LICENSE = r'D:\iphone-rpc\vkbuild\dxvk3\LICENSE'
IPA_DIR = os.environ.get('PGKIT_IPA_DIR', r'D:\iphone-rpc\ipa\serve')
OUT = os.environ.get('PGKIT_OUT', r'D:\iphone-rpc\tester_kit')
ANDROID_DIR = r'D:\iphone-rpc\androidbuild'   # app\vk\build_android.sh で組んだ受け手（server_aarch64 と server_x86_64）
ANDROID_APK = r'D:\iphone-rpc\work\android_app\out\pocketgpu.apk'   # work\android_app\build_apk.sh で組んだ PocketGPU アプリ（Android）
ANDROID_APK_MIN = (0, 1, 4)   # これより古い版のアプリはキットに入れない。0.1.2 からカメラとマイクの口が、0.1.4 から GPU の口も localabstract に変わった（android_app.cmd が版を見て向け方を選ぶ）。
                              # 0.1.3 までは端末の 50060 が使えないと GPU の受け手がすぐ終わった（10/14 テスターの Redmagic 9S Pro）。0.1.4 は次の番号と localabstract:pocketgpu-gpu でも待つ
AAPT2 = r'D:\android-sdk\build-tools\35.0.0\aapt2.exe'   # APK の版を読む。無ければ版の確認だけ飛ばす
PLACE = b'PGKIT-MARK:' + b'#' * 32
PROTO_H = os.environ.get('PGKIT_PROTO_H', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'vk', 'remote', 'proto.h'))
# カメラ・マイク・字幕の道具と仮想カメラ（C で組んだ物。UCRT だけを使うので VC の部品は要らない）。KITBUILD\extras\ に置く → キットの extras\
#   webcam_rx.exe・ipaudio.exe・camera_setup.exe・dshow_cam64.dll・dshow_cam32.dll は work/camset/out、livecap.exe は work/livecap
MEDIA = ('webcam_rx.exe', 'ipaudio.exe', 'livecap.exe', 'camera_setup.exe', 'dshow_cam64.dll', 'dshow_cam32.dll')
# PC アプリ。work/pcapp2 の build.cmd で組んだ out\PocketGPU.exe を KITBUILD の一番上に置く → キットの一番上
APP_EXE = 'PocketGPU.exe'
# 更新を聞く先（Cloudflare の kit-worker）。試すときは PGKIT_UPDATE_URL で PC の中の wrangler dev に向ける
UPDATE_URL = os.environ.get('PGKIT_UPDATE_URL', 'https://pocketgpu-kit.YOUR-SUBDOMAIN.workers.dev')

# 窓から使う人と、Steam の起動オプション（"...\pocketgpu.cmd" %command%）用。引数が無ければ Run-on-iPhone-GPU.cmd に回す
LAUNCH = r'''@echo off
rem PocketGPU: run one app or game on the iPhone GPU.
rem   pocketgpu.cmd "C:\path\to\game.exe"
rem   Steam: put   "C:\path\to\this\folder\pocketgpu.cmd" %command%   in the game's Launch Options.
rem Only the app started from this file sees the iPhone GPU. Nothing is written to the registry.
setlocal
if "%~1"=="" goto pick
set "VK_ADD_DRIVER_FILES=%~dp0iphone_icd.json;%~dp0x86\iphone_icd.json"
if "%IPHONE_VK%"=="" set IPHONE_VK=usb
if "%IPHONE_VK_ONLY%"=="" set IPHONE_VK_ONLY=1
if "%IPHONE_VK_ONLY%"=="1" set "VK_LOADER_DRIVERS_SELECT=*iphone_icd*"
if "%IPHONE_VK_ONLY%"=="1" set "VK_ICD_FILENAMES=%VK_ADD_DRIVER_FILES%"
set DISABLE_TWITCH_VULKAN_OVERLAY=1
set DISABLE_VULKAN_OBS_CAPTURE=1
set DISABLE_VULKAN_OW_OBS_CAPTURE=1
%*
goto :eof
:pick
call "%~dp0Run-on-iPhone-GPU.cmd"
exit /b %errorlevel%
'''

HERE = os.path.dirname(os.path.abspath(__file__))

# PC の様子を 1 つのテキストにまとめる（kit/report.ps1・ユーザー名・PC 名・IP は入れない）
REPORT = r'''@echo off
rem PocketGPU: write pocketgpu-report.txt about this PC (no user name, PC name or IP).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0report.ps1"
pause
'''

# 手順だけの下書き。言い回しは鶴田さんが直す
README = r'''PocketGPU tester kit (draft)
============================

Thank you for testing. Please do not share this kit with anyone else.
Each kit is marked for one person.

Have an Android phone instead of an iPhone? Skip the steps below and read android\README_android.txt.

What you need
- A 64-bit Windows 10 or 11 PC
- An iPhone or iPad and a USB cable. So far it has only been tested on an iPhone 17 Pro.
  USB 3 is several times faster: a USB-C cable that supports USB 3, in a USB-C or USB 3 port on the PC.
  The cable that comes with the iPhone is USB 2. Only Pro models from the iPhone 15 Pro on support USB 3.
- Apple Devices from the Microsoft Store, or iTunes. Windows needs one of them to talk to the iPhone over USB.
- A free tool to install the iPhone app with your own Apple ID, such as AltStore or Sideloadly

Easiest way
Extract the zip (step 1), then double-click Setup-PocketGPU.cmd (do not use "Run as administrator").
It checks this PC, installs what is missing with winget after asking y/N (one UAC prompt for everything that needs administrator rights),
shows how to put the app on the iPhone, and ends with the same check as Start-PocketGPU.cmd. It never types your Apple ID for you.
The steps below are the same thing by hand.

Steps
1. Extract the whole zip: right-click it and choose "Extract All...".
2. Install the iPhone app: open RpcServer-*.ipa with AltStore or Sideloadly and sign in with your Apple ID.
   With a free Apple ID the app stops opening after 7 days. Install it again to continue.
   AltStore needs AltServer running on the PC. After the PC restarts, start AltServer again.
3. Open the app on the iPhone and keep it in front with the screen on. Plug the iPhone into the PC with USB.
4. Double-click Start-PocketGPU.cmd. It checks each part in order and tells you what to do when something is missing.
5. To run a game or app on the iPhone GPU, drop its .exe onto Run-on-iPhone-GPU.cmd.
   You can also double-click Run-on-iPhone-GPU.cmd and choose the .exe.
   For a DirectX 9, 10 or 11 game it offers to put the DXVK files next to the game (9: d3d9.dll. 10: d3d10core.dll, d3d11.dll and dxgi.dll. 11: d3d11.dll and dxgi.dll). To undo, delete those files there.
   Steam games: put this line in the game's Properties > Launch Options:
       "C:\path\to\this\folder\pocketgpu.cmd" %command%
   Start-PocketGPU.cmd shows this line with the real path and can copy it for you.
6. To use the iPhone only as a webcam and microphone, without the GPU, double-click Start-Mic-Camera.cmd.
   In the iPhone app, turn on "Use this iPhone as a webcam" and "Use as PC speaker and mic".
   Using it as a microphone needs VB-CABLE, which is not in this kit. FAQ.txt explains how to install it.
   RpcServer-*.ipa needs iOS 17 or later. On an older iPhone with iOS 15 or 16, install PocketCam-*.ipa instead. It has only the webcam and the microphone.
7. PocketGPU.exe in this folder does all of this in one window: checking, running games, the camera, the microphone, and captions.

Updates
- PC: double-click Update-PocketGPU.cmd. It checks for a new version and downloads only the files that changed.
  PocketGPU.exe can do the same from Settings > Updates.
  The old files are kept in the update-backup folder. If an update fails, the old files are put back.
- iPhone app: in AltStore, open Sources, tap +, and add your own source URL. Type /kit in #get-kit on Discord to see it.
  When a new version is out, AltStore shows Update. Update-PocketGPU.cmd also saves the new RpcServer-*.ipa in this folder.

Good to know
- Only programs started this way see the iPhone GPU. Nothing is written to the registry.
- Start-Mic-Camera.cmd and PocketGPU.exe write to the registry only when you add the camera, and only for your Windows user.
- Programs that run as administrator cannot see the iPhone GPU. Windows does not pass the settings to them.
- Only one program at a time can use the iPhone GPU.
- Do not use games with anti-cheat such as Easy Anti-Cheat, BattlEye, VAC or Vanguard. Your account may be banned.
- In a command window you can also run:   pocketgpu.cmd "C:\path\to\app.exe" arguments

Reporting
- In the iPhone app, tap "Send logs" and send the text to us on Discord or by email.
- On the PC, send pocketgpu-start-log.txt, which Start-PocketGPU.cmd writes. If you used Setup-PocketGPU.cmd, also send pocketgpu-setup-summary.txt.
  Also double-click report.cmd and send pocketgpu-report.txt. Neither has your user name, PC name or IP.
- For the webcam and microphone, also send pocketgpu-miccam-log.txt, which Start-Mic-Camera.cmd writes.
- Please also tell us the PC, the app or game, the resolution, and what happened.

If the connection keeps dropping
- Restart the iPhone first, then plug it in again and reopen the app.
- More answers are in FAQ.txt.

Third-party software
- dxvk\: DXVK, macOS async build 1.10.3 by Gcenx, zlib license. See dxvk\LICENSE.

--------------------------------------------
PocketGPU 試験用キット。この説明は下書きです

試していただき、ありがとうございます。このキットは、他の人に渡さないでください。1 人ずつ識別用の印が含まれています。

iPhone ではなく Android のスマートフォンを使う場合は、以下の手順は実行せずに、android\README_android.txt を読んでください。

必要なもの
- 64 ビット版の Windows 10 または 11 の PC
- iPhone または iPad と USB ケーブル。今のところ、テストしたのは iPhone 17 Pro だけです
  USB 3 を使うと、転送速度が数倍になります。USB 3 に対応した USB-C ケーブルを、PC の USB-C ポートまたは USB 3 ポートに接続してください。
  iPhone に付属のケーブルは USB 2 です。USB 3 に対応しているのは、iPhone 15 Pro 以降の Pro モデルだけです
- Microsoft Store の Apple Devices または iTunes。Windows が USB で iPhone と通信するために、どちらかが必要です
- 自分の Apple ID で iPhone にアプリをインストールするための無料のツール。AltStore や Sideloadly など

いちばん簡単な方法
zip を展開（手順 1）したら、Setup-PocketGPU.cmd をダブルクリックしてください（「管理者として実行」は使わないでください）。
この PC をチェックして、足りない物を y/N で聞いてから winget でインストールします（管理者権限が必要な物は、まとめて UAC の確認が 1 回だけです）。
iPhone へのアプリの入れ方を案内して、最後に Start-PocketGPU.cmd と同じチェックまで行います。Apple ID を代わりに入力することはありません。
以下の手順は、同じことを手作業で行う方法です。

手順
1. zip ファイルを右クリックして「すべて展開」を選び、すべてのファイルを展開してください
2. iPhone のアプリをインストールします。RpcServer-*.ipa を AltStore か Sideloadly で開いて、自分の Apple ID でログインしてください。
   無料の Apple ID の場合は、7 日でアプリが起動できなくなります。その場合は、インストールし直してください。
   AltStore を使う場合は、PC で AltServer が起動している必要があります。PC を再起動したら、AltServer を起動し直してください
3. iPhone でアプリを起動して、前面に表示したまま画面をオンにしておいてください。iPhone を USB ケーブルで PC に接続してください
4. Start-PocketGPU.cmd をダブルクリックしてください。必要な項目を順にチェックして、足りない物があれば対処方法を表示します
5. ゲームやアプリを iPhone の GPU で動かすには、その .exe を Run-on-iPhone-GPU.cmd にドラッグ＆ドロップしてください。
   Run-on-iPhone-GPU.cmd をダブルクリックして、.exe を選択することもできます。
   DirectX 9・10・11 のゲームの場合は、DXVK のファイルをゲームと同じフォルダに配置するかどうかを聞きます（DirectX 9 は d3d9.dll、DirectX 10 は d3d10core.dll・d3d11.dll・dxgi.dll、DirectX 11 は d3d11.dll と dxgi.dll）。元に戻すときは、配置したファイルを削除してください。
   Steam のゲームは、ゲームのプロパティの「起動オプション」に次の 1 行を入力してください：
       "C:\このフォルダの場所\pocketgpu.cmd" %command%
   Start-PocketGPU.cmd は、実際のパスに置き換えたこの 1 行を表示して、コピーすることもできます
6. GPU を使わずに、iPhone をウェブカメラとマイクとしてだけ使う場合は、Start-Mic-Camera.cmd をダブルクリックしてください。
   iPhone のアプリで「ウェブカメラとして使う」と「スピーカーとマイクとして使う」をオンにしてください。
   マイクとして使うには VB-CABLE が必要です。このキットには含まれていません。インストールの方法は FAQ.txt に書いてあります。
   RpcServer-*.ipa には iOS 17 以降が必要です。iOS 15 または 16 の古い iPhone には、代わりに PocketCam-*.ipa をインストールしてください。ウェブカメラとマイクの機能だけがあります
7. このフォルダの PocketGPU.exe を開くと、チェック・ゲームの起動・カメラ・マイク・字幕を 1 つのウィンドウで操作できます

更新
- PC：Update-PocketGPU.cmd をダブルクリックしてください。新しいバージョンがあるかをチェックして、変更されたファイルだけをダウンロードします。
  PocketGPU.exe の「設定」の「アップデート」からも同じ操作ができます。
  古いファイルは update-backup フォルダに残します。更新に失敗したときは、元のファイルに戻します
- iPhone のアプリ：AltStore の「Sources」で「+」をタップして、あなた専用のソースの URL を追加してください。URL は、Discord の #get-kit で /kit と入力すると表示されます。
  新しいバージョンが出ると、AltStore に「Update」が表示されます。Update-PocketGPU.cmd も、新しい RpcServer-*.ipa をこのフォルダに保存します

知っておくこと
- この方法で起動したアプリだけが、iPhone の GPU を認識できます。レジストリには何も書き込みません
- Start-Mic-Camera.cmd と PocketGPU.exe は、カメラを登録するときだけ、今の Windows のユーザーの設定としてレジストリに書き込みます
- 管理者権限で実行されるアプリは、iPhone の GPU を認識できません。Windows が設定を渡さないためです
- iPhone の GPU を一度に使用できるアプリは 1 つだけです
- Easy Anti-Cheat、BattlEye、VAC、Vanguard などのアンチチートを導入しているゲームでは使用しないでください。アカウントが停止されることがあります
- コマンドプロンプトからは  pocketgpu.cmd "C:\アプリの場所\app.exe" 引数  でも起動できます

報告
- iPhone のアプリの「ログを送信」をタップして、そのテキストを Discord かメールで送ってください
- PC では、Start-PocketGPU.cmd が作成する pocketgpu-start-log.txt を送ってください。Setup-PocketGPU.cmd を使ったときは、pocketgpu-setup-summary.txt も送ってください。
  report.cmd をダブルクリックして作成される pocketgpu-report.txt も送ってください。どちらのファイルにも、ユーザー名・PC 名・IP アドレスは含まれません
- ウェブカメラとマイクについては、Start-Mic-Camera.cmd が作成する pocketgpu-miccam-log.txt も送ってください
- PC、アプリやゲームの名前、解像度、何が起きたかも書いてください

接続が何度も切れるとき
- まず iPhone を再起動して、USB ケーブルを接続し直してから、アプリを起動し直してください
- 他の困りごとは FAQ.txt に書いてあります

他の人が作ったソフトウェア
- dxvk\：DXVK。Gcenx さんの macOS async 版 1.10.3 で、zlib ライセンスです。dxvk\LICENSE を参照してください
'''

# extras\ に入れる説明。下書き
EXTRAS_README = r'''PocketGPU extras. This is a draft.
Start-Mic-Camera.cmd and PocketGPU.exe use these tools. You do not need to open them yourself.
- webcam_rx.exe: receives the video from the iPhone camera.
- ipaudio.exe: uses the iPhone as the PC's speaker and microphone.
- livecap.exe: shows captions. The iPhone needs iOS 26 or later.
- camera_setup.exe, dshow_cam64.dll, dshow_cam32.dll: add "PocketGPU iPhone Camera" to the camera list for your Windows user. No administrator rights are needed.
Keep these files in this folder. Start-Mic-Camera.cmd and PocketGPU.exe look for them here.

PocketGPU の追加ツール。この説明は下書きです
Start-Mic-Camera.cmd と PocketGPU.exe が使うツールです。直接開く必要はありません。
- webcam_rx.exe：iPhone のカメラの映像を受信します
- ipaudio.exe：iPhone を PC のスピーカーとマイクとして使います
- livecap.exe：字幕を表示します。iPhone に iOS 26 以降が必要です
- camera_setup.exe・dshow_cam64.dll・dshow_cam32.dll：「PocketGPU iPhone Camera」を、今の Windows のユーザーのカメラの一覧に追加します。管理者権限は必要ありません
これらのファイルは、このフォルダから移動しないでください。Start-Mic-Camera.cmd と PocketGPU.exe は、このフォルダから探します。
'''


# ---- Android の機種を持っている人用（android\ と pocketgpu_android.cmd・check_android.cmd）----
# cmd の中の文は英語だけにする（日本語の Windows の cmd は Shift_JIS で読むので、UTF-8 の日本語は化ける）。
# 受け手（server_aarch64）は /data/local/tmp/pgpu_server に置いて、127.0.0.1 だけで待たせ、adb forward で PC につなぐ。
ANDROID_COMMON = r'''@echo off
rem PocketGPU for Android: shared part. The other android\*.cmd files call it. Do not run it by itself.
rem Sets ADB, ANDROID_SERIAL, PGPU_PORT, PGPU_CAM_PORT and PGPU_AUDIO_PORT. Exits with code 1 and a message when something is missing.
rem ADB_OK is set once adb.exe is found, so that the caller can still use adb when the phone is missing.
rem The ports on this PC are chosen in this order: the variable (set PGPU_PORT=...) > the port saved in android\port.txt > the default (50300, 50064, 50063).
rem If the PC port is reserved by Windows (see: netsh int ipv4 show excludedportrange protocol=tcp) or used by another program, tools\pgpu_port.ps1
rem   picks a free port outside the reserved ranges (47300, 47400, 47500 and up), prints one line about it, and saves it in android\port.txt.
rem   This happens when the caller sets PGPU_PICK=gpu or PGPU_PICK=gpu,cam,audio before calling this file. To forget the saved ports: delete android\port.txt.
rem Several phones at the same time: set PGPU_FIXED_PORTS=1, ANDROID_SERIAL=the-serial and PGPU_PORT=a-different-port-for-each-phone (PGPU_CAM_PORT and PGPU_AUDIO_PORT too, if the app is used).
rem   Then android\port.txt is neither read nor written, and the ports are never changed behind your back. Every file of this kit uses exactly the numbers you set.
set "ADB_OK="
if defined PGPU_FIXED_PORTS goto fixed_check
:fixed_back
if not defined PGPU_FIXED_PORTS call :load_ports
if "%PGPU_PORT%"=="" set PGPU_PORT=50300
if "%PGPU_CAM_PORT%"=="" set PGPU_CAM_PORT=50064
if "%PGPU_AUDIO_PORT%"=="" set PGPU_AUDIO_PORT=50063

rem ---- find adb.exe. It is not in this kit: get SDK Platform-Tools from Google ----
if defined ADB goto adb_given
for /f "delims=" %%A in ('where adb.exe 2^>nul') do if not defined ADB set "ADB=%%A"
if defined ADB goto adb_ok
for %%D in ("%LOCALAPPDATA%\Android\Sdk\platform-tools" "%USERPROFILE%\platform-tools" "%USERPROFILE%\Downloads\platform-tools" "%USERPROFILE%\Desktop\platform-tools" "C:\platform-tools" "C:\adb" "%ProgramFiles%\platform-tools" "%~dp0..\platform-tools" "%~dp0..\..\platform-tools") do if not defined ADB if exist "%%~D\adb.exe" set "ADB=%%~D\adb.exe"
rem "Extract All" in Windows puts the zip in a folder named after it: platform-tools-latest-windows\platform-tools
for %%D in ("%USERPROFILE%\Downloads\platform-tools-latest-windows\platform-tools" "%USERPROFILE%\Desktop\platform-tools-latest-windows\platform-tools" "%OneDrive%\Desktop\platform-tools" "%OneDrive%\Desktop\platform-tools-latest-windows\platform-tools" "%OneDrive%\Downloads\platform-tools" "%~dp0..\platform-tools-latest-windows\platform-tools" "%~dp0..\..\platform-tools-latest-windows\platform-tools") do if not defined ADB if exist "%%~D\adb.exe" set "ADB=%%~D\adb.exe"
if defined ADB goto adb_ok
echo.
echo ERROR: adb.exe was not found.
echo Download "SDK Platform-Tools for Windows" from Google and unzip it:
echo   https://developer.android.com/tools/releases/platform-tools
echo Then do one of these:
echo   - put the unzipped "platform-tools" folder inside this kit folder, next to it, or at C:\platform-tools
echo   - add the platform-tools folder to PATH
echo   - run   set ADB=C:\path\to\platform-tools\adb.exe   and start this file again from the same window
exit /b 1
:adb_given
if exist "%ADB%" goto adb_ok
echo.
echo ERROR: ADB is set to "%ADB%", but that file does not exist.
exit /b 1
:adb_ok
set ADB_OK=1

rem ---- find the phone ----
"%ADB%" start-server >nul 2>&1
set NDEV=0
set NUNAUTH=0
set ONE=
for /f "skip=1 tokens=1,2" %%A in ('"%ADB%" devices') do call :count "%%A" "%%B"
if %NDEV%==0 goto no_device
if %NDEV% GTR 1 if not defined ANDROID_SERIAL goto many_devices
if not defined ANDROID_SERIAL set "ANDROID_SERIAL=%ONE%"
"%ADB%" get-state 2>nul | findstr /b /c:"device" >nul
if errorlevel 1 goto bad_serial
if not defined PGPU_PICK exit /b 0
goto pick_ports

:count
if "%~2"=="device" set /a NDEV+=1
if "%~2"=="device" set "ONE=%~1"
if "%~2"=="unauthorized" set /a NUNAUTH+=1
goto :eof

:no_device
echo.
if %NUNAUTH% GTR 0 goto unauthorized
echo ERROR: no Android phone found.
echo   - Connect the phone with a USB cable that can carry data (not a charge-only cable).
echo   - Turn on "USB debugging" in Developer options on the phone.
echo   - If the phone asks to allow USB debugging, tap Allow.
echo   - Run   "%ADB%" devices   to see what adb sees.
exit /b 1
:unauthorized
echo ERROR: the phone is connected but not allowed yet.
echo Unlock the phone and tap "Allow" on the "Allow USB debugging?" question. Then run this file again.
exit /b 1
:many_devices
echo.
echo ERROR: more than one device is connected. Unplug the others, or choose one:
"%ADB%" devices
echo   set ANDROID_SERIAL=the-serial-from-the-list-above
exit /b 1
:bad_serial
echo.
echo ERROR: the device "%ANDROID_SERIAL%" is not ready. Run   "%ADB%" devices   to check.
exit /b 1

:pick_ports
rem With PGPU_FIXED_PORTS the numbers are the caller's choice: nothing is checked here and nothing is saved.
if defined PGPU_FIXED_PORTS exit /b 0
rem Check the ports on this PC and save the ones that had to change. The result is read back from android\port.txt.
if exist "%~dp0..\tools\pgpu_port.ps1" "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\tools\pgpu_port.ps1" -PgpuPick "%PGPU_PICK%" <nul
call :load_ports force
exit /b 0

:fixed_check
rem PGPU_FIXED_PORTS needs the phone and the PC port to be chosen by the caller, so that two phones can never share a port by accident.
if "%ANDROID_SERIAL%"=="" goto fixed_bad
if "%PGPU_PORT%"=="" goto fixed_bad
goto fixed_back
:fixed_bad
echo.
echo ERROR: PGPU_FIXED_PORTS is set, so ANDROID_SERIAL and PGPU_PORT must be set too (a different PGPU_PORT for each phone).
exit /b 1

rem Read android\port.txt (lines like gpu=47300). Without "force" a variable that is already set wins; with "force" the file wins.
:load_ports
if not exist "%~dp0port.txt" exit /b 0
for /f "usebackq tokens=1,* delims==" %%A in ("%~dp0port.txt") do call :load_one "%%A" "%%B" %1
exit /b 0
:load_one
set "LP_KEY=%~1"
set "LP_VAL=%~2"
echo %LP_VAL%| findstr /r /x "[0-9][0-9]*" >nul
if errorlevel 1 exit /b 0
if /i "%LP_KEY%"=="gpu" if "%PGPU_PORT%"=="" set "PGPU_PORT=%LP_VAL%"
if /i "%LP_KEY%"=="gpu" if "%~3"=="force" set "PGPU_PORT=%LP_VAL%"
if /i "%LP_KEY%"=="cam" if "%PGPU_CAM_PORT%"=="" set "PGPU_CAM_PORT=%LP_VAL%"
if /i "%LP_KEY%"=="cam" if "%~3"=="force" set "PGPU_CAM_PORT=%LP_VAL%"
if /i "%LP_KEY%"=="audio" if "%PGPU_AUDIO_PORT%"=="" set "PGPU_AUDIO_PORT=%LP_VAL%"
if /i "%LP_KEY%"=="audio" if "%~3"=="force" set "PGPU_AUDIO_PORT=%LP_VAL%"
set "LP_KEY="
set "LP_VAL="
exit /b 0
'''

ANDROID_START = r'''@echo off
rem PocketGPU for Android: copy the receiver to the phone, start it, and open the USB port to this PC.
rem   android\start_android.cmd
rem Needs adb.exe (Google SDK Platform-Tools). See README_android.txt.
rem Nothing is installed on the phone and no root is needed. The receiver is a file in /data/local/tmp.
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal
rem Check the port on this PC first (Windows can reserve some ports). The chosen port is saved in android\port.txt, and the other files use it.
set "PGPU_PICK=gpu"
call "%~dp0_common.cmd"
if errorlevel 1 goto fail
rem Several phones (PGPU_FIXED_PORTS=1): adb forward quietly takes a port over from another phone. Stop here if this PC port already goes to a different phone.
if defined PGPU_FIXED_PORTS for /f "tokens=1,2" %%A in ('"%ADB%" forward --list') do if "%%B"=="tcp:%PGPU_PORT%" if /i not "%%A"=="%ANDROID_SERIAL%" goto port_taken

set "SRV="
"%ADB%" shell getprop ro.product.cpu.abi | findstr /b /c:"arm64-v8a" >nul && set "SRV=server_aarch64"
"%ADB%" shell getprop ro.product.cpu.abi | findstr /b /c:"x86_64" >nul && set "SRV=server_x86_64"
if not defined SRV goto bad_abi
if not exist "%~dp0%SRV%" goto no_file

echo Phone:
"%ADB%" shell getprop ro.product.model
echo Receiver: %SRV%  Port: %PGPU_PORT%
echo Stopping an old receiver, if any ...
"%ADB%" shell "kill $(pidof pgpu_server) 2>/dev/null; for i in 1 2 3 4 5 6 7 8 9 10; do pidof pgpu_server >/dev/null || break; sleep 0.3; done; kill -9 $(pidof pgpu_server) 2>/dev/null; rm -f /data/local/tmp/pgpu_server /data/local/tmp/pgpu_server.log; true" >nul 2>&1
echo Copying the receiver to the phone ...
"%ADB%" push "%~dp0%SRV%" /data/local/tmp/pgpu_server > "%TEMP%\pocketgpu_push.txt" 2>&1
if errorlevel 1 goto push_failed
echo Starting the receiver ...
"%ADB%" shell "cd /data/local/tmp && chmod 755 pgpu_server && (PGPU_LANG=en nohup ./pgpu_server 127.0.0.1 %PGPU_PORT% > pgpu_server.log 2>&1 &)"
rem Wait until the receiver says it is listening. Starting Vulkan on the phone can take a few seconds.
rem The line is "Vulkan receiver: listening on 127.0.0.1:PORT" (older receivers write it in Japanese, with the same start and address).
set /a TRIES=0
:wait_listen
ping -n 2 127.0.0.1 >nul
set /a TRIES+=1
"%ADB%" shell cat /data/local/tmp/pgpu_server.log 2>nul | findstr /r /c:"^Vulkan .*127\.0\.0\.1:%PGPU_PORT%" >nul
if not errorlevel 1 goto listening
"%ADB%" shell pidof pgpu_server | findstr /r "[0-9]" >nul
if errorlevel 1 goto server_failed
if %TRIES% LSS 30 goto wait_listen
goto server_slow
:listening
"%ADB%" forward tcp:%PGPU_PORT% tcp:%PGPU_PORT% >nul
if errorlevel 1 goto forward_failed

echo.
echo Done. The receiver is running on the phone and port %PGPU_PORT% is connected to this PC.
echo Next: start an app with   pocketgpu_android.cmd "C:\path\to\app.exe"
echo To check that Windows sees the phone GPU: check_android.cmd
echo When you are finished: android\stop_android.cmd
echo Keep the phone unlocked with the screen on while an app is running.
goto end

:bad_abi
echo.
echo ERROR: this phone's CPU is not supported. Only 64-bit ARM (arm64-v8a) phones are supported. This phone says:
"%ADB%" shell getprop ro.product.cpu.abi
goto fail
:no_file
echo.
echo ERROR: %~dp0%SRV% is missing. Unzip the whole kit again.
goto fail
:push_failed
echo.
echo ERROR: could not copy the receiver to the phone. adb said:
type "%TEMP%\pocketgpu_push.txt"
goto fail
:server_failed
echo.
echo ERROR: the receiver did not stay running on the phone. Its log:
"%ADB%" shell cat /data/local/tmp/pgpu_server.log
echo Please send us this log together with the phone model and the Android version.
goto fail
:server_slow
echo.
echo ERROR: the receiver is running on the phone, but it did not start listening within 30 seconds. Its log:
"%ADB%" shell cat /data/local/tmp/pgpu_server.log
echo Unlock the phone and run this file again. If it happens again, send us this log.
goto fail
:port_taken
echo.
echo ERROR: port %PGPU_PORT% on this PC already goes to another phone. Use a different PGPU_PORT for each phone.
"%ADB%" forward --list
goto fail
:forward_failed
echo.
echo ERROR: could not open port %PGPU_PORT% on this PC. Another program may be using it, or Windows reserved it.
echo Run android\stop_android.cmd and try again. This file picks a free port by itself; to choose one:   set PGPU_PORT=47301
echo To see what Windows reserved:   netsh int ipv4 show excludedportrange protocol=tcp
goto fail

:fail
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
:end
if "%PGPU_NOPAUSE%"=="" pause
exit /b 0
'''

ANDROID_APP = r'''@echo off
rem PocketGPU for Android (app version): open the USB ports to the PocketGPU app and turn its features on.
rem   android\android_app.cmd          start: forward the ports and turn on webcam, speaker and mic, and GPU
rem   android\android_app.cmd stop     close the ports and turn the features off
rem Needs adb.exe and USB debugging. Install the app first:  adb install android\pocketgpu.apk
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
rem
rem Port form for the webcam (50064) and the audio (50063). It depends on the app version:
rem   app 0.1.2 and newer:   adb forward tcp:50064 localabstract:pocketgpu-webcam   and   adb forward tcp:50063 localabstract:pocketgpu-audio
rem                          The app accepts these only from adb, so other apps on the phone cannot read the camera or the mic.
rem   app 0.1.1 and older:   adb forward tcp:50064 tcp:50064   and   adb forward tcp:50063 tcp:50063
rem This file reads the app version from the phone and uses the matching form.
rem   If the version cannot be read it uses the new form. If adb refuses the new form it uses the old one.
rem   To choose the form yourself:   set PGPU_APP_FORWARD=new   or   set PGPU_APP_FORWARD=old
rem   The 0.1.2 app also takes the old form when "Also accept old PC tools" is on in the app settings. It is off by default.
rem Other ports:   set PGPU_CAM_PORT=50064   set PGPU_AUDIO_PORT=50063   set PGPU_PORT=50300
rem   Ports on this PC: if Windows reserved one of them (netsh int ipv4 show excludedportrange protocol=tcp) or another program uses it,
rem   this file picks a free port outside the reserved ranges (47300, 47400 and 47500 and up), says so, and saves it in android\port.txt.
rem   Use the numbers this file prints. stop and the other files in this kit read android\port.txt, so they use the same ports.
rem GPU: if the kit receiver from android\start_android.cmd is running on the phone, the app does not start its own
rem   receiver and port %PGPU_PORT% goes to the kit receiver (tcp:%PGPU_PORT%). Otherwise it goes to the receiver inside the app:
rem   app 0.1.4 and newer:   adb forward tcp:50300 localabstract:pocketgpu-gpu   (no port number on the phone, so nothing else can be in the way)
rem   app 0.1.3 and older:   adb forward tcp:50300 tcp:50060
rem   If the version cannot be read it uses tcp:50060 (the 0.1.4 app also listens there when 50060 is free on the phone).
rem   If adb refuses the localabstract form it uses tcp:50060.
rem   To choose the GPU form yourself:   set PGPU_APP_GPU=usb   or   set PGPU_APP_GPU=tcp
setlocal
if /i not "%~1"=="stop" set "PGPU_PICK=gpu,cam,audio"
call "%~dp0_common.cmd"
if errorlevel 1 goto nodev
set "PKG=com.tsurutanmen.pocketgpu"
set "APP=%PKG%/.MainActivity"
if "%PGPU_CAM_PORT%"=="" set PGPU_CAM_PORT=50064
if "%PGPU_AUDIO_PORT%"=="" set PGPU_AUDIO_PORT=50063
if /i "%~1"=="stop" goto stop

"%ADB%" shell pm path %PKG% | findstr /b /c:"package:" >nul
if errorlevel 1 goto no_app

rem ---- which form of the ports does this app version take? ----
set "APPVER="
for /f "tokens=1,2 delims== " %%U in ('"%ADB%" shell dumpsys package %PKG%') do if "%%U"=="versionName" if not defined APPVER set "APPVER=%%V"
set "FORM=new"
set "FORMBY=the app version could not be read"
set "GPUFORM=tcp"
set "GPUOLD="
set "V1="
set "V2="
set "V3="
if defined APPVER for /f "tokens=1-3 delims=.-+_ " %%A in ("%APPVER%") do set "V1=%%A" & set "V2=%%B" & set "V3=%%C"
if not defined V1 goto ver_done
if not defined V2 set "V2=0"
if not defined V3 set "V3=0"
echo %V1%.%V2%.%V3%| findstr /r /x "[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*" >nul
if errorlevel 1 goto ver_done
set /a VN=1000000*%V1%+1000*%V2%+%V3%
set "FORMBY=app version %APPVER%"
if %VN% LSS 1002 set "FORM=old"
if %VN% GEQ 1004 set "GPUFORM=usb"
if %VN% LSS 1004 set "GPUOLD=1"
:ver_done
set "FORCED="
if /i "%PGPU_APP_FORWARD%"=="new" set "FORM=new"
if /i "%PGPU_APP_FORWARD%"=="new" set "FORCED=1"
if /i "%PGPU_APP_FORWARD%"=="old" set "FORM=old"
if /i "%PGPU_APP_FORWARD%"=="old" set "FORCED=1"
if defined FORCED set "FORMBY=PGPU_APP_FORWARD=%PGPU_APP_FORWARD%"

if "%FORM%"=="old" goto fwd_old
"%ADB%" forward tcp:%PGPU_CAM_PORT% localabstract:pocketgpu-webcam >nul 2>&1
if errorlevel 1 goto fwd_new_failed
"%ADB%" forward tcp:%PGPU_AUDIO_PORT% localabstract:pocketgpu-audio >nul 2>&1
if errorlevel 1 goto fwd_new_failed
goto fwd_done
:fwd_new_failed
if defined FORCED goto forward_failed
echo adb did not accept the new port form. Trying the old form ...
echo The 0.1.2 app takes the old form only when "Also accept old PC tools" is on in the app settings.
set "FORM=old"
set "FORMBY=adb refused the new form"
:fwd_old
"%ADB%" forward tcp:%PGPU_CAM_PORT% tcp:50064 >nul || goto forward_failed
"%ADB%" forward tcp:%PGPU_AUDIO_PORT% tcp:50063 >nul || goto forward_failed
:fwd_done

rem ---- GPU: the kit receiver if it runs, else the receiver inside the app in the form for this app version ----
set "GPU_FORCED="
if /i "%PGPU_APP_GPU%"=="usb" set "GPUFORM=usb"
if /i "%PGPU_APP_GPU%"=="usb" set "GPU_FORCED=1"
if /i "%PGPU_APP_GPU%"=="tcp" set "GPUFORM=tcp"
if /i "%PGPU_APP_GPU%"=="tcp" set "GPU_FORCED=1"
set "GPU_TARGET=tcp:50060"
set "GPU_WHO=the app receiver, port 50060 on the phone"
if "%GPUFORM%"=="usb" set "GPU_TARGET=localabstract:pocketgpu-gpu"
if "%GPUFORM%"=="usb" set "GPU_WHO=the app receiver, USB port pocketgpu-gpu"
"%ADB%" shell pidof pgpu_server | findstr /r "[0-9]" >nul
if errorlevel 1 goto gpu_forward
set "GPU_TARGET=tcp:%PGPU_PORT%"
set "GPU_WHO=the kit receiver started by android\start_android.cmd"
set "GPU_FORCED=1"
set "GPUOLD="
:gpu_forward
"%ADB%" forward tcp:%PGPU_PORT% %GPU_TARGET% >nul 2>&1
if not errorlevel 1 goto gpu_done
if defined GPU_FORCED goto forward_failed
if not "%GPU_TARGET%"=="localabstract:pocketgpu-gpu" goto forward_failed
echo adb did not accept the USB port form for the GPU. Using port 50060 on the phone ...
set "GPU_TARGET=tcp:50060"
set "GPU_WHO=the app receiver, port 50060 on the phone"
"%ADB%" forward tcp:%PGPU_PORT% tcp:50060 >nul || goto forward_failed
:gpu_done
"%ADB%" shell am start -n %APP% --ez webcam true --ez audio true --ez gpu true >nul
echo.
echo Done. If the phone asks for camera or microphone access, tap Allow.
echo Port form: %FORM% (%FORMBY%)
echo   Webcam:          webcam_rx.exe --addr 127.0.0.1:%PGPU_CAM_PORT%
echo   Speaker and mic: ipaudio.exe --connect 127.0.0.1:%PGPU_AUDIO_PORT% --speaker --mic
echo   GPU:             pocketgpu_android.cmd "C:\path\to\app.exe"   (uses %GPU_WHO%)
echo   GPU port form:   adb forward tcp:%PGPU_PORT% %GPU_TARGET%
if defined GPUOLD echo The app is older than 0.1.4. If the GPU does not answer, update the app:   "%ADB%" install -r "%~dp0pocketgpu.apk"
if "%FORM%"=="old" echo The app on the phone is old. The new app is android\pocketgpu.apk. To update:   "%ADB%" install -r "%~dp0pocketgpu.apk"
if "%FORM%"=="old" if not defined FORCED echo The new app takes the camera and the mic only from adb, so other apps on the phone cannot read them.
echo When you are finished: android\android_app.cmd stop
goto end

:stop
"%ADB%" shell am start -n %APP% --ez webcam false --ez audio false --ez gpu false >nul 2>&1
"%ADB%" forward --remove tcp:%PGPU_CAM_PORT% >nul 2>&1
"%ADB%" forward --remove tcp:%PGPU_AUDIO_PORT% >nul 2>&1
"%ADB%" forward --remove tcp:%PGPU_PORT% >nul 2>&1
echo Stopped.
goto end

:nodev
if /i not "%~1"=="stop" goto fail
if not defined ADB_OK goto fail
if "%PGPU_CAM_PORT%"=="" set PGPU_CAM_PORT=50064
if "%PGPU_AUDIO_PORT%"=="" set PGPU_AUDIO_PORT=50063
"%ADB%" forward --remove tcp:%PGPU_CAM_PORT% >nul 2>&1
"%ADB%" forward --remove tcp:%PGPU_AUDIO_PORT% >nul 2>&1
"%ADB%" forward --remove tcp:%PGPU_PORT% >nul 2>&1
echo The phone was not found. The ports on this PC were closed, but the app on the phone was not told to stop.
goto fail

:no_app
echo.
echo ERROR: the PocketGPU app is not installed on the phone. Install it with:   "%ADB%" install "%~dp0pocketgpu.apk"
goto fail
:forward_failed
echo.
echo ERROR: could not open a port on this PC. Another program may be using %PGPU_CAM_PORT%, %PGPU_AUDIO_PORT% or %PGPU_PORT%, or Windows reserved it.
echo Run android\android_app.cmd stop and try again. To see what Windows reserved:   netsh int ipv4 show excludedportrange protocol=tcp
echo To choose other ports (see the top of this file):   set PGPU_PORT=47301
goto fail

:fail
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
:end
if "%PGPU_NOPAUSE%"=="" pause
exit /b 0
'''

ANDROID_STOP = r'''@echo off
rem PocketGPU for Android: stop the receiver on the phone and close the USB port.
rem   android\stop_android.cmd
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal
call "%~dp0_common.cmd"
if errorlevel 1 goto nodev
echo Stopping the receiver on the phone ...
"%ADB%" shell "kill $(pidof pgpu_server) 2>/dev/null; rm -f /data/local/tmp/pgpu_server; true" >nul 2>&1
"%ADB%" forward --remove tcp:%PGPU_PORT% >nul 2>&1
rem The port saved in android\port.txt (the one start_android.cmd used) is closed too, even if PGPU_PORT is set to something else in this window.
if not defined PGPU_FIXED_PORTS if exist "%~dp0port.txt" for /f "usebackq tokens=1,* delims==" %%A in ("%~dp0port.txt") do if /i "%%A"=="gpu" "%ADB%" forward --remove tcp:%%B >nul 2>&1
echo Stopped. The receiver is removed from the phone; its log stays at /data/local/tmp/pgpu_server.log.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 0
:nodev
if defined ADB_OK "%ADB%" forward --remove tcp:%PGPU_PORT% >nul 2>&1
if defined ADB_OK if not defined PGPU_FIXED_PORTS if exist "%~dp0port.txt" for /f "usebackq tokens=1,* delims==" %%A in ("%~dp0port.txt") do if /i "%%A"=="gpu" "%ADB%" forward --remove tcp:%%B >nul 2>&1
echo The receiver on the phone was not stopped. Connect the phone and run this file again, or restart the phone.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
'''

ANDROID_LAUNCH = r'''@echo off
rem PocketGPU: run one app or game on the Android phone GPU. Run android\start_android.cmd first.
rem   pocketgpu_android.cmd "C:\path\to\game.exe"
rem Only the app started from this file sees the phone GPU. Nothing is written to the registry.
rem DirectX 9, 10 and 11 games also need DXVK next to the game. Start-Android-GPU.cmd puts it there for you (it asks first).
rem DXVK_FILTER_DEVICE_NAME=PocketGPU makes DXVK pick the phone GPU ("PocketGPU phone (...)").
setlocal
if "%~1"=="" (
  echo Usage: pocketgpu_android.cmd "C:\path\to\app.exe"
  exit /b 1
)
set "VK_ADD_DRIVER_FILES=%~dp0iphone_icd.json;%~dp0x86\iphone_icd.json"
rem The port on this PC: the variable PGPU_PORT, else the port saved in android\port.txt by android\start_android.cmd, else 50300.
if "%PGPU_PORT%"=="" if exist "%~dp0android\port.txt" for /f "usebackq tokens=1,* delims==" %%A in ("%~dp0android\port.txt") do if /i "%%A"=="gpu" echo %%B| findstr /r /x "[0-9][0-9]*" >nul && set "PGPU_PORT=%%B"
if "%PGPU_PORT%"=="" set PGPU_PORT=50300
set IPHONE_VK=127.0.0.1:%PGPU_PORT%
if "%DXVK_FILTER_DEVICE_NAME%"=="" if "%IPHONE_VK_NAME%"=="" set DXVK_FILTER_DEVICE_NAME=PocketGPU
if "%IPHONE_VK_ONLY%"=="" set IPHONE_VK_ONLY=1
if "%IPHONE_VK_ONLY%"=="1" set "VK_LOADER_DRIVERS_SELECT=*iphone_icd*"
if "%IPHONE_VK_ONLY%"=="1" set "VK_ICD_FILENAMES=%VK_ADD_DRIVER_FILES%"
set DISABLE_TWITCH_VULKAN_OVERLAY=1
set DISABLE_VULKAN_OBS_CAPTURE=1
set DISABLE_VULKAN_OW_OBS_CAPTURE=1
%*
'''

ANDROID_CHECK = r'''@echo off
rem PocketGPU: check that Windows can see the Android phone GPU. Run android\start_android.cmd first.
setlocal
set "VK_ADD_DRIVER_FILES=%~dp0iphone_icd.json;%~dp0x86\iphone_icd.json"
rem The port on this PC: the variable PGPU_PORT, else the port saved in android\port.txt by android\start_android.cmd, else 50300.
if "%PGPU_PORT%"=="" if exist "%~dp0android\port.txt" for /f "usebackq tokens=1,* delims==" %%A in ("%~dp0android\port.txt") do if /i "%%A"=="gpu" echo %%B| findstr /r /x "[0-9][0-9]*" >nul && set "PGPU_PORT=%%B"
if "%PGPU_PORT%"=="" set PGPU_PORT=50300
set IPHONE_VK=127.0.0.1:%PGPU_PORT%
set "VK_ICD_FILENAMES=%VK_ADD_DRIVER_FILES%"
"%~dp0vklist.exe"
echo.
echo If you see a GPU named PocketGPU phone in the list above, it works.
echo If it is not listed, run android\start_android.cmd first.
if "%PGPU_NOPAUSE%"=="" pause
'''

# 手順だけの下書き。言い回しは鶴田さんが直す
ANDROID_EASY = r'''@echo off
rem PocketGPU for Android: the easy way. Double-click this file.
rem It copies the receiver to the phone, starts it, connects it to this PC and checks that Windows sees the phone GPU.
rem Then drag a game's .exe onto this window (or onto this file) to start it on the phone GPU.
rem The game is started by tools\run.ps1, the same as Run-on-iPhone-GPU.cmd but for the Android phone (PGPU_ANDROID=1).
rem It reads the .exe and tells DirectX 9, 10, 11, 12, Unity and Unreal games apart. For DirectX 9, 10 and 11 it asks before it puts
rem DXVK (dxvk\x64 or dxvk\x32) next to the game, and never replaces another d3d9.dll, d3d10core.dll, d3d11.dll or dxgi.dll.
rem The "Use as a GPU" switch in the phone app is not needed for this. Needs adb.exe, see android\README_android.txt.
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal
set "GAME=%~1"
echo PocketGPU for Android
echo.
echo [1/2] Setting up the phone ...
set PGPU_NOPAUSE=1
rem "<nul" keeps adb from reading this window's input, so a game dragged in early is not lost.
call "%~dp0android\start_android.cmd" <nul
if errorlevel 1 goto failed
echo.
echo [2/2] Checking that Windows sees the phone GPU ...
set "VK_ADD_DRIVER_FILES=%~dp0iphone_icd.json;%~dp0x86\iphone_icd.json"
rem start_android.cmd may have picked another port (Windows can reserve some ports) and saved it in android\port.txt. That one wins here.
if not defined PGPU_FIXED_PORTS if exist "%~dp0android\port.txt" for /f "usebackq tokens=1,* delims==" %%A in ("%~dp0android\port.txt") do if /i "%%A"=="gpu" echo %%B| findstr /r /x "[0-9][0-9]*" >nul && set "PGPU_PORT=%%B"
if "%PGPU_PORT%"=="" set PGPU_PORT=50300
set IPHONE_VK=127.0.0.1:%PGPU_PORT%
set "VK_ICD_FILENAMES=%VK_ADD_DRIVER_FILES%"
"%~dp0vklist.exe" > "%TEMP%\pocketgpu_vklist.txt" 2>&1 <nul
type "%TEMP%\pocketgpu_vklist.txt"
findstr /c:"PocketGPU phone" "%TEMP%\pocketgpu_vklist.txt" >nul
if errorlevel 1 goto not_seen
echo.
echo OK. Windows sees the phone GPU.
echo Keep the phone unlocked with the screen on while a game is running.
set "VK_ICD_FILENAMES="
if not "%GAME%"=="" goto run
:ask
echo.
echo Drag a game's .exe into this window and press Enter. To finish, just press Enter.
echo Next time you can also drop the game's .exe onto Start-Android-GPU.cmd itself.
set "GAME="
set /p GAME=Game:
if not defined GAME goto end
set "GAME=%GAME:"=%"
if "%GAME%"=="" goto end
:run
if not exist "%GAME%" goto no_game
if not exist "%~dp0tools\run.ps1" goto run_plain
echo.
set PGPU_ANDROID=1
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\run.ps1" "%GAME%"
set "RUNRC=%errorlevel%"
set "PGPU_ANDROID="
echo.
if not "%RUNRC%"=="0" echo The game was not started.
echo The receiver keeps running on the phone until you run android\stop_android.cmd or restart the phone.
goto ask
:run_plain
rem tools\run.ps1 is missing: start the game directly. DirectX 9, 10 and 11 games then need DXVK copied by hand (android\README_android.txt step 6).
rem Start the game in its own folder, as Steam and Explorer do. Some games look for their files there.
for %%G in ("%GAME%") do cd /d "%%~dpG"
echo Starting "%GAME%" on the phone GPU ...
call "%~dp0pocketgpu_android.cmd" "%GAME%"
echo.
echo The game (or its launcher) has exited. The receiver keeps running on the phone until you run android\stop_android.cmd or restart the phone.
goto end

:no_game
echo.
echo ERROR: this file was not found:
echo   "%GAME%"
echo Drag the game's .exe file itself into this window.
goto ask

:not_seen
echo.
echo The phone GPU is not in the list above.
echo Please send a screenshot of this window, and the receiver log:
call "%~dp0android\_common.cmd" >nul 2>&1 <nul
if defined ADB "%ADB%" shell cat /data/local/tmp/pgpu_server.log 2>nul <nul
goto failed
:failed
echo.
echo Something did not work. Please send a screenshot of this window in Discord.
pause
exit /b 1
:end
pause
exit /b 0
'''

ANDROID_README = r'''PocketGPU for Android (draft)
=============================

This lets a Windows PC use the GPU of an Android phone as a Vulkan GPU. It is an experiment.
Please do not share this kit with anyone else. Each kit is marked for one person.
It has so far been tested only on an emulator. Your phone may crash or not work at all. That is useful to know, so please tell us.

What you need
- A Windows 10 or 11 PC (64-bit) and this kit
- An Android phone: 64-bit ARM (arm64-v8a), Android 11 or newer, with a Vulkan GPU
- A USB cable that carries data
- "SDK Platform-Tools for Windows" from Google (it contains adb.exe), free:
  https://developer.android.com/tools/releases/platform-tools
  It is not in this kit. Unzip it anywhere you like.

Steps
1. On the phone, turn on Developer options: Settings > About phone > tap "Build number" 7 times.
   (The menu names depend on the maker.) Then open Developer options and turn on "USB debugging".
   Turning on "Stay awake" there also helps.
2. On the PC, put the unzipped "platform-tools" folder inside this kit folder, or next to it (or at C:\platform-tools).
   If you keep it somewhere else, either add it to PATH or run this in the window you use:
       set ADB=C:\path\to\platform-tools\adb.exe
3. Connect the phone to the PC with USB. Unlock it. If it asks "Allow USB debugging?", tap Allow.
4. Double-click android\start_android.cmd.
   It finds adb, checks the phone, copies the receiver (android\server_aarch64) to /data/local/tmp on the phone,
   starts it, and connects port 50300 on this PC to it with "adb forward". Nothing is installed on the phone and no root is needed.
   Some PCs reserve port ranges for Hyper-V or WSL, and then port 50300 cannot be used. If so, start_android.cmd says so, picks a free port
   outside the reserved ranges (47300 and up) by itself, and saves it in android\port.txt. The other files in this kit use the same port.
   If it says "Done", go on.
5. Double-click check_android.cmd. You should see a GPU named "PocketGPU phone (...)" in the list.
6. Start a Vulkan app with:   pocketgpu_android.cmd "C:\path\to\app.exe"
   DirectX 11 games: copy d3d11.dll and dxgi.dll from dxvk\x64 (64-bit games) or dxvk\x32 (32-bit games)
   next to the game's .exe, then start the game with pocketgpu_android.cmd. Delete those files to undo.
   DirectX 10 games: copy d3d10core.dll, d3d11.dll and dxgi.dll the same way. DirectX 9 games: copy d3d9.dll.
   With Start-Android-GPU.cmd: it does steps 4 and 5 for you. Then drag the game's .exe into its window.
   It sees whether the game uses DirectX 9, 10 or 11, and asks (y/N) before it puts the DXVK files next to the game.
   It never replaces a d3d9.dll, d3d10core.dll, d3d11.dll or dxgi.dll that is already there. DirectX 12 games do not work yet.
   Keep the phone unlocked with the screen on while the app runs.
7. When you are finished, double-click android\stop_android.cmd. It stops the receiver and removes it from the phone.
   (Restarting the phone also stops it.)

Using the PocketGPU app (optional): camera, microphone, speaker and GPU
This is another way, not part of steps 4 to 7. The PocketGPU app (android\pocketgpu.apk) runs the receiver itself and also lends the camera, the microphone and the speaker.
A. Install the app once. In a Command Prompt run:   "C:\path\to\adb.exe" install android\pocketgpu.apk
   To update an old app, run it with   install -r   instead.
B. Double-click android\android_app.cmd. It reads the app version from the phone and connects the USB ports in the matching form:
   - App 0.1.2 and newer:  adb forward tcp:50064 localabstract:pocketgpu-webcam   and   adb forward tcp:50063 localabstract:pocketgpu-audio
     Only adb can reach these, so other apps on the phone cannot read the camera or the mic.
   - App 0.1.1 and older:  adb forward tcp:50064 tcp:50064   and   adb forward tcp:50063 tcp:50063
   If the version cannot be read, it tries the new form first and the old form when adb refuses it.
   To choose the form yourself:   set PGPU_APP_FORWARD=new   or   set PGPU_APP_FORWARD=old
   The 0.1.2 app takes the old form only when "Also accept old PC tools" is on in the app settings. It is off by default. Leave it off.
   If you turn it on for an old PC tool, turn it off again afterwards: other apps on the phone can then reach the camera and the mic.
C. If the phone asks for camera or microphone access, tap Allow. Then start the tools from extras\ :
       webcam_rx.exe --addr 127.0.0.1:50064
       ipaudio.exe --connect 127.0.0.1:50063 --speaker --mic
D. GPU: if android\start_android.cmd has started the receiver, the app uses that one. If not, the app uses its own receiver. pocketgpu_android.cmd and check_android.cmd work the same either way.
   android\android_app.cmd connects the GPU port 50300 to the app receiver in the form for the app version:
   - App 0.1.4 and newer:  adb forward tcp:50300 localabstract:pocketgpu-gpu
     No port number is used on the phone, so another program on the phone cannot be in the way.
   - App 0.1.3 and older:  adb forward tcp:50300 tcp:50060
   If the version cannot be read, or adb refuses the first form, it uses tcp:50060. To choose yourself:   set PGPU_APP_GPU=usb   or   set PGPU_APP_GPU=tcp
E. When you are finished, run   android\android_app.cmd stop
If the camera or the mic does not connect, run Fix-PocketGPU.cmd. It checks these ports against the app version and connects them again in the right form.

Notes
- Only the app started from pocketgpu_android.cmd sees the phone GPU. Nothing is written to the registry.
- The receiver listens only inside the phone (127.0.0.1). It is reachable only through the USB cable.
- Do not use games with anti-cheat (Easy Anti-Cheat, BattlEye, VAC, Vanguard). Your account may be banned.
- The iPhone files in this kit (RpcServer-*.ipa, Start-PocketGPU.cmd, Run-on-iPhone-GPU.cmd, pocketgpu.cmd) are not needed for Android.
- Ports on this PC: the kit checks them before it uses them. If Windows reserved a port (netsh int ipv4 show excludedportrange protocol=tcp shows the reserved ranges)
  or another program uses it, it picks a free one (47300 and up for the GPU) and saves it in android\port.txt. check_android.cmd, pocketgpu_android.cmd,
  android_app.cmd and stop_android.cmd read that file, so you do not have to type the number. To forget it: delete android\port.txt.
  To choose a port yourself: set PGPU_PORT=47301 before step 4, 5 and 6 (use the same number in all of them). The camera and mic ports of android_app.cmd work the same way
  (PGPU_CAM_PORT and PGPU_AUDIO_PORT, usually 50064 and 50063): use the numbers it prints for webcam_rx.exe and ipaudio.exe.
- More than one phone connected: run   adb devices   and set ANDROID_SERIAL=the-serial before step 4.
  To use several phones at the same time, give each phone its own PC port and keep android\port.txt out of it. Open one Command Prompt per phone and, in each one, run
    set PGPU_FIXED_PORTS=1   then   set ANDROID_SERIAL=the-serial   and   set PGPU_PORT=47441 (47442 for the next phone, and so on)   before step 4, 5 and 6.
  The numbers are used as they are (nothing is picked or saved). If a port goes to another phone already, start_android.cmd says so and stops.
- To send the window picture as video (less data, the PC needs to decode H.264): set IPHONE_VK_VIDEO=1 before step 6.

If something does not work
- "adb.exe was not found": do step 2.
- "not allowed yet": unlock the phone and tap Allow. If no question appears, unplug and plug in again, or in Developer options use "Revoke USB debugging authorizations".
- "no Android phone found": try another cable or USB port, set the USB mode to "File transfer", and check step 1.
  Some makers need their own USB driver on the PC.
- "the receiver did not stay running": the script prints the receiver's log. Send it to us.
- The picture stops or the connection drops: run android\stop_android.cmd, then android\start_android.cmd again.
- To send the logs: double-click Send-Android-Log.cmd in the kit folder and send the zip it makes (no user name, PC name or phone number).

Reporting
Please send us, on Discord or by email:
- The phone model, Android version, and chip (for example "Pixel 8, Android 15, Tensor G3")
- The receiver log. Run this in a normal Command Prompt:   "C:\path\to\adb.exe" shell cat /data/local/tmp/pgpu_server.log > server_log.txt
  and send server_log.txt (do this before stop_android.cmd, or after: stop keeps the log)
- On the PC, double-click report.cmd and send pocketgpu-report.txt (no user name, PC name or IP)
- The app or game, the resolution, the frame rate you saw, what worked and what crashed
- Whether the phone got hot

Third-party software
- dxvk\: DXVK (macOS async build 1.10.3, by Gcenx), zlib license. See dxvk\LICENSE.

--------------------------------------------
PocketGPU for Android。この説明は下書きです

Windows の PC から、Android のスマートフォンの GPU を Vulkan の GPU として使うためのものです。実験段階です。
他の人に渡さないでください。1 人ずつ識別用の印が含まれています。
今のところ、テストしたのはエミュレーターだけです。スマートフォンでは、クラッシュしたり、動作しなかったりするかもしれません。それも知りたいので、教えてください。

必要なもの
- 64 ビット版の Windows 10 または 11 の PC と、このキット
- Android のスマートフォン：64 ビット ARM の arm64-v8a で、Android 11 以上、Vulkan に対応した GPU を搭載しているもの
- データ通信に対応した USB ケーブル
- Google の「SDK Platform-Tools for Windows」。adb.exe が含まれていて、無料です
  https://developer.android.com/tools/releases/platform-tools
  このキットには含まれていません。好きな場所に展開してください。

手順
1. スマートフォンで開発者向けオプションを表示します。「設定」>「端末情報」>「ビルド番号」を 7 回タップしてください。
   メニューの名前はメーカーによって違います。次に、開発者向けオプションを開いて「USB デバッグ」をオンにしてください。
   同じ画面の「スリープモードにしない」か「Stay awake」もオンにすると便利です
2. PC で、展開した「platform-tools」フォルダを、このキットのフォルダの中か横に置いてください。C:\platform-tools に置いてもかまいません。
   他の場所に置く場合は、PATH に追加するか、使うウィンドウで次のコマンドを入力してください：
       set ADB=C:\置いた場所\platform-tools\adb.exe
3. スマートフォンを USB で PC に接続して、ロックを解除してください。「USB デバッグを許可しますか？」と表示されたら、「許可」をタップしてください
4. android\start_android.cmd をダブルクリックしてください。
   adb を探し、スマートフォンをチェックして、受信アプリの android\server_aarch64 をスマートフォンの /data/local/tmp にコピーして起動します。
   さらに、PC のポート 50300 を「adb forward」で受信アプリに接続します。スマートフォンには何もインストールしません。root 権限も必要ありません。
   PC によっては、Hyper-V や WSL のために Windows がポートの範囲を予約していて、50300 を使えないことがあります。その場合は、start_android.cmd がそのことを表示して、
   予約の範囲の外の空いているポート（47300 から順）を自動で選び、android\port.txt に覚えます。キットのほかのファイルも、同じポートを使います。
   「Done」と表示されたら、次へ進んでください
5. check_android.cmd をダブルクリックしてください。一覧に「PocketGPU phone (…)」という名前の GPU が表示されれば成功です
6. Vulkan のアプリは、pocketgpu_android.cmd "C:\アプリの場所\app.exe" で起動してください。
   DirectX 11 のゲームの場合は、64 ビットのゲームなら dxvk\x64、32 ビットのゲームなら dxvk\x32 の d3d11.dll と dxgi.dll を、
   ゲームの .exe と同じフォルダにコピーしてから、pocketgpu_android.cmd で起動してください。元に戻すときは、そのファイルを削除してください。
   DirectX 10 のゲームは d3d10core.dll・d3d11.dll・dxgi.dll の 3 つを、DirectX 9 のゲームは d3d9.dll を、同じようにコピーしてください。
   Start-Android-GPU.cmd を使う場合は、手順 4 と 5 をまとめて行います。そのあと、ゲームの .exe をそのウィンドウにドラッグしてください。
   DirectX 9・10・11 のゲームかどうかを調べて、DXVK のファイルをゲームと同じフォルダに配置するかを y/N で聞きます。
   すでにある d3d9.dll・d3d10core.dll・d3d11.dll・dxgi.dll は上書きしません。DirectX 12 のゲームは、まだ動きません。
   アプリが動作している間は、スマートフォンのロックを解除したまま、画面をオンにしておいてください
7. 終わったら、android\stop_android.cmd をダブルクリックしてください。受信アプリを停止して、スマートフォンから削除します
   スマートフォンを再起動しても、受信アプリは停止します

PocketGPU アプリを使う場合：カメラ、マイク、スピーカー、GPU
手順 4〜7 とは別の方法です。PocketGPU アプリ（android\pocketgpu.apk）が受信アプリの役目をして、カメラ、マイク、スピーカーも PC に貸します。
A. アプリを 1 回だけインストールします。コマンドプロンプトで次を入力してください：   "C:\置いた場所\adb.exe" install android\pocketgpu.apk
   古いアプリを更新するときは、install の代わりに install -r を使ってください。
B. android\android_app.cmd をダブルクリックしてください。スマートフォンからアプリの版を読んで、合う向け方で USB の口をつなぎます。
   - アプリ 0.1.2 以上：adb forward tcp:50064 localabstract:pocketgpu-webcam と adb forward tcp:50063 localabstract:pocketgpu-audio
     adb からしか届かないので、スマートフォンの他のアプリは、カメラとマイクを読めません。
   - アプリ 0.1.1 以前：adb forward tcp:50064 tcp:50064 と adb forward tcp:50063 tcp:50063
   版が読めないときは、新しい向け方を先に使い、adb が断ったら古い向け方にします。
   向け方を自分で決めるときは、set PGPU_APP_FORWARD=new か set PGPU_APP_FORWARD=old を入力してください。
   0.1.2 のアプリは、アプリの設定で「古い PC のツールも受け付ける」をオンにしたときだけ、古い向け方も受けます。既定はオフです。オフのままにしてください。
   古い PC のツールのためにオンにしたら、使い終わったらオフに戻してください。オンの間は、スマートフォンの他のアプリからもカメラとマイクに届いてしまいます。
C. スマートフォンがカメラやマイクの許可を求めたら、「許可」をタップしてください。そのあと、extras\ のツールを起動してください：
       webcam_rx.exe --addr 127.0.0.1:50064
       ipaudio.exe --connect 127.0.0.1:50063 --speaker --mic
D. GPU：android\start_android.cmd で受信アプリを起動してあれば、アプリはそれを使います。起動していなければ、アプリの中の受信アプリを使います。pocketgpu_android.cmd と check_android.cmd は、どちらでも同じように使えます。
   android\android_app.cmd は、GPU のポート 50300 を、アプリの版に合う向け方でアプリの中の受信アプリにつなぎます。
   - アプリ 0.1.4 以上：adb forward tcp:50300 localabstract:pocketgpu-gpu
     スマートフォンのポート番号を使わないので、スマートフォンの他のアプリとぶつかりません。
   - アプリ 0.1.3 以前：adb forward tcp:50300 tcp:50060
   版が読めないときや、adb が最初の向け方を断ったときは、tcp:50060 を使います。自分で決めるときは、set PGPU_APP_GPU=usb か set PGPU_APP_GPU=tcp を入力してください。
E. 終わったら、android\android_app.cmd stop を実行してください。
カメラやマイクがつながらないときは、Fix-PocketGPU.cmd を実行してください。これらの口をアプリの版と照らして、合う向け方でつなぎ直します。

メモ
- pocketgpu_android.cmd から起動したアプリだけが、スマートフォンの GPU を認識できます。レジストリには何も書き込みません
- 受信アプリは、スマートフォンの中の 127.0.0.1 だけで待ち受けています。USB ケーブルを通してしか接続できません
- Easy Anti-Cheat、BattlEye、VAC、Vanguard などのアンチチートを導入しているゲームでは使用しないでください。アカウントが停止されることがあります
- このキットの iPhone 用のファイル RpcServer-*.ipa、Start-PocketGPU.cmd、Run-on-iPhone-GPU.cmd、pocketgpu.cmd は、Android では必要ありません
- PC のポートは、使う前にキットがチェックします。Windows が予約している場合（予約の範囲は netsh int ipv4 show excludedportrange protocol=tcp で表示できます）や、
  ほかのプログラムが使っている場合は、空いているポート（GPU は 47300 から順）を選んで android\port.txt に覚えます。check_android.cmd・pocketgpu_android.cmd・
  android_app.cmd・stop_android.cmd はそのファイルを読むので、番号を入力する必要はありません。覚えた番号を消すには、android\port.txt を削除してください
  自分でポートを決める場合は、手順 4・5・6 の前に set PGPU_PORT=47301 を入力してください。3 つの手順で同じ番号を使ってください。
  android_app.cmd のカメラとマイクのポート（PGPU_CAM_PORT と PGPU_AUDIO_PORT。通常は 50064 と 50063）も同じ仕組みです。webcam_rx.exe と ipaudio.exe には、表示された番号を使ってください
- スマートフォンを 2 台以上接続している場合は、adb devices を入力し、手順 4 の前に set ANDROID_SERIAL=一覧の番号 を入力してください
  複数台を同時に使う場合は、台ごとに PC のポートを分け、android\port.txt を使わないようにします。台ごとにコマンドプロンプトを開き、手順 4・5・6 の前に
    set PGPU_FIXED_PORTS=1 、set ANDROID_SERIAL=一覧の番号 、set PGPU_PORT=47441（次の台は 47442、…）を入力してください
  番号はそのまま使われます（選び直しも記録もしません）。すでに別のスマートフォンにつながっているポートを指定すると、start_android.cmd が知らせて止まります
- ウィンドウの映像を動画で送る場合は、手順 6 の前に set IPHONE_VK_VIDEO=1 を入力してください。通信量が減りますが、PC で H.264 をデコードする必要があります

動作しないとき
- 「adb.exe was not found」：手順 2 を実行してください
- 「not allowed yet」：スマートフォンのロックを解除して「許可」をタップしてください。何も表示されない場合は、ケーブルを抜き差しするか、開発者向けオプションの「USB デバッグの承認を取り消す」をタップしてください
- 「no Android phone found」：別のケーブルや USB ポートを試してください。USB の用途を「ファイル転送」にして、手順 1 をチェックしてください。
  メーカーによっては、PC にそのメーカーの USB ドライバが必要です
- 「the receiver did not stay running」：受信アプリのログが画面に表示されます。それを送ってください
- 映像が止まる、または接続が切れる：android\stop_android.cmd を実行してから、もう一度 android\start_android.cmd を実行してください
- 記録を送るとき：キットのフォルダの Send-Android-Log.cmd をダブルクリックして、作成される zip を送ってください。ユーザー名・PC 名・電話番号は含まれません

報告
Discord かメールで次を送ってください。
- スマートフォンの機種、Android のバージョン、チップ。例：「Pixel 8、Android 15、Tensor G3」
- 受信アプリのログ。普通のコマンドプロンプトで次を入力してください：   "C:\置いた場所\adb.exe" shell cat /data/local/tmp/pgpu_server.log > server_log.txt
  作成された server_log.txt を送ってください。stop_android.cmd を実行する前でも後でもかまいません。stop しても、ログは残ります
- PC で report.cmd をダブルクリックして、作成された pocketgpu-report.txt を送ってください。ユーザー名・PC 名・IP アドレスは含まれません
- アプリやゲームの名前、解像度、表示されたフレームレート、動作したこと、クラッシュしたこと
- スマートフォンが熱くなったかどうか

他の人が作ったソフトウェア
- dxvk\：DXVK。Gcenx さんの macOS async 版 1.10.3 で、zlib ライセンスです。dxvk\LICENSE を参照してください
'''


def sha256(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def stamp(src, dst, mark):
    # mark が None のときは印を入れない（ひな形。印の場所はそのまま残す）
    data = open(src, 'rb').read()
    if data.count(PLACE) != 1:
        raise SystemExit(f'{src} の印の場所が 1 つではない（{data.count(PLACE)} 個）。kitbuild を組み直す')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(data if mark is None else data.replace(PLACE, b'PGKIT-MARK:' + mark))


def proto_version():
    # 「始める」が受け手にあいさつするときの版。ドライバと同じ数を tools\kit.ini に書く
    m = re.search(r'#define VR_PROTO_VERSION (\d+)', open(PROTO_H, encoding='utf-8').read())
    if not m:
        raise SystemExit(f'{PROTO_H} に VR_PROTO_VERSION が無い')
    return int(m.group(1))


def apk_version(path):
    # PocketGPU アプリ（APK）の versionName。aapt2 dump badging で読む。aapt2 が無い・読めないときは None
    if not os.path.exists(AAPT2):
        return None
    try:
        out = subprocess.run([AAPT2, 'dump', 'badging', path], capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.search(r"versionName='([^']+)'", out)
    return m.group(1) if m else None


def check_apk(path):
    # キットに入れる APK が ANDROID_APK_MIN 以上の版か。版を読めたときだけ止める。版を返す。読めなければ ''
    ver = apk_version(path)
    if ver is None:
        print(f'注意：{path} の版を読めなかった（aapt2 が無い）。{".".join(map(str, ANDROID_APK_MIN))} 以上かを自分で確かめる')
        return ''
    m = re.match(r'(\d+)\.(\d+)(?:\.(\d+))?', ver)
    if not m or tuple(int(x or 0) for x in m.groups()) < ANDROID_APK_MIN:
        raise SystemExit(f'{path} の版は {ver}。{".".join(map(str, ANDROID_APK_MIN))} 以上の APK でないと入れない（0.1.1 以前はカメラとマイクの口の向け方が違い、0.1.3 以前は端末の 50060 が使えないと GPU の受け手が動かない）。work/android_app/build_apk.sh で組み直す')
    return ver


def newest_ipa(name='RpcServer'):
    # IPA_DIR の 名前-x.y.z.ipa のうち一番新しい物。無ければ ValueError
    def ver(n):
        m = re.match(re.escape(name) + r'-(\d+)\.(\d+)\.(\d+)\.ipa$', n)
        return tuple(map(int, m.groups())) if m else None
    names = [n for n in os.listdir(IPA_DIR) if ver(n)]
    return os.path.join(IPA_DIR, max(names, key=ver))


def mark_ipa(src, dst, mark):
    # Payload/名前.app/ に印のファイル PGKitMark.txt を足した IPA を作る
    with zipfile.ZipFile(src) as zi, zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as zo:
        app = None
        for item in zi.infolist():
            zo.writestr(item, zi.read(item.filename))
            m = re.match(r'(Payload/[^/]+\.app/)Info\.plist$', item.filename)
            if m: app = m.group(1)
        if not app:
            raise SystemExit(f'{os.path.basename(src)} の中に .app が見つからない')
        zo.writestr(app + 'PGKitMark.txt', mark.decode())


def missing_inputs(a):
    # 新しく足した物（extras\ の道具・入口・PocketGPU.exe・PocketCam の IPA）が揃っているか。キットのフォルダを作る前に見る
    miss = []
    if not a.no_media:
        miss += [p for p in (os.path.join(KITBUILD, 'extras', n) for n in MEDIA) if not os.path.exists(p)]
        miss += [p for p in (os.path.join(HERE, 'kit', 'Start-Mic-Camera.cmd'), os.path.join(HERE, 'kit', 'tools', 'miccam.ps1')) if not os.path.exists(p)]
        if not a.no_ipa:
            try:
                newest_ipa('PocketCam')
            except (OSError, ValueError):
                miss.append(os.path.join(IPA_DIR, 'PocketCam-*.ipa'))
    if not a.no_app and not os.path.exists(os.path.join(KITBUILD, APP_EXE)):
        miss.append(os.path.join(KITBUILD, APP_EXE))
    if not a.no_android and not a.no_android_apk and not os.path.exists(a.android_apk):
        miss.append(a.android_apk)
    if not a.no_android:
        miss += [p for p in (os.path.join(HERE, 'kit', 'Send-Android-Log.cmd'), os.path.join(HERE, 'kit', 'tools', 'android-log.ps1')) if not os.path.exists(p)]
    return miss


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('who', nargs='?', default='', help='渡す人の名前（Reddit の名前など・英数字）。--template のときは要らない')
    ap.add_argument('--discord-id', default='', help='Discord の bot から作るときの、その人の番号。同じ番号には 2 つめを作らない')
    ap.add_argument('--android-dir', default=ANDROID_DIR, help='Android の受け手（server_aarch64）がある所')
    ap.add_argument('--android-apk', default=ANDROID_APK, help='Android の PocketGPU アプリ（pocketgpu.apk）。ANDROID_APK_MIN（0.1.4）以上でないと入れない')
    ap.add_argument('--no-android-apk', action='store_true', help='Android のアプリ（android\pocketgpu.apk）を入れない')
    ap.add_argument('--with-x86_64', action='store_true', help='エミュレーター用の server_x86_64 も入れる（試験用・配るキットには入れない）')
    ap.add_argument('--no-android', action='store_true', help='Android の物を入れない（今までと同じ中身）')
    ap.add_argument('--no-ipa', action='store_true', help='iPhone のアプリ（IPA）を入れない（試験用）')
    ap.add_argument('--no-media', action='store_true', help='カメラ・マイク・字幕の物（extras\\・Start-Mic-Camera.cmd・PocketCam の IPA）を入れない')
    ap.add_argument('--no-app', action='store_true', help='PC アプリ（PocketGPU.exe）を入れない')
    ap.add_argument('--extras', action='store_true', help='前の呼び方。今は extras\\ の道具は既定で入るので、付けても何も変わらない')
    ap.add_argument('--template', metavar='DIR', help='印を入れないひな形をこのフォルダに作る（publish_update.py 用）。IPA・update.ini・zip・台帳は作らない')
    ap.add_argument('--update-token', default='', help='tools\\update.ini に書く更新用の合言葉。無ければ乱数で作る')
    a = ap.parse_args()
    ledger = os.path.join(OUT, 'ledger.csv')
    if a.template:
        # ひな形：印は入れず、IPA と人ごとの物は入れない。中身のフォルダを残す
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
        kit = os.path.join(OUT, f'PocketGPU-{who}' + (f'-{a.discord_id[-6:]}' if a.discord_id else ''))
        if os.path.exists(kit):
            raise SystemExit(f'{kit} はもうある。同じ人に作り直すなら、先にそのフォルダと zip を片づける')
    update_token = a.update_token or secrets.token_urlsafe(24)
    if not re.fullmatch(r'[A-Za-z0-9_-]{16,64}', update_token):
        raise SystemExit('--update-token は英数字と - _ の 16〜64 文字')
    miss = missing_inputs(a)
    if miss:
        raise SystemExit('次の物が無い。extras\\ の道具は work/camset/out と work/livecap で、PocketGPU.exe は work/pcapp2 の build.cmd で組んだ物を '
                         'KITBUILD に置く。入れないなら --no-media・--no-app・--no-android-apk\n  ' + '\n  '.join(miss))

    apk_ver = ''
    if not a.no_android and not a.no_android_apk:
        apk_ver = check_apk(a.android_apk)   # 古い版なら、キットのフォルダを作る前に止める

    stamp(os.path.join(KITBUILD, 'iphone_icd.dll'), os.path.join(kit, 'iphone_icd.dll'), mark)
    stamp(os.path.join(KITBUILD, 'x86', 'iphone_icd.dll'), os.path.join(kit, 'x86', 'iphone_icd.dll'), mark)
    shutil.copy2(os.path.join(KITBUILD, 'iphone_icd.json'), os.path.join(kit, 'iphone_icd.json'))
    shutil.copy2(os.path.join(KITBUILD, 'x86', 'iphone_icd.json'), os.path.join(kit, 'x86', 'iphone_icd.json'))
    shutil.copy2(os.path.join(KITBUILD, 'vklist.exe'), os.path.join(kit, 'vklist.exe'))
    for arch in ('x64', 'x32'):
        os.makedirs(os.path.join(kit, 'dxvk', arch), exist_ok=True)
        for n in ('d3d11.dll', 'dxgi.dll', 'd3d10core.dll', 'd3d9.dll'):
            shutil.copy2(os.path.join(DXVK, arch, n), os.path.join(kit, 'dxvk', arch, n))
    shutil.copy2(DXVK_LICENSE, os.path.join(kit, 'dxvk', 'LICENSE'))
    for name, text in (('pocketgpu.cmd', LAUNCH), ('report.cmd', REPORT)):
        open(os.path.join(kit, name), 'w', encoding='ascii', newline='\r\n').write(text)
    # 入口 2 つと、その中身（PowerShell）。前の check.cmd は Start-PocketGPU.cmd の 7 番目が代わる
    # Update-PocketGPU.cmd と tools\update.ps1 は更新の入口（10/12）
    for name in ('Start-PocketGPU.cmd', 'Setup-PocketGPU.cmd', 'Run-on-iPhone-GPU.cmd', 'Update-PocketGPU.cmd'):
        shutil.copy2(os.path.join(HERE, 'kit', name), os.path.join(kit, name))
    os.makedirs(os.path.join(kit, 'tools'), exist_ok=True)
    for name in ('pgpu_common.ps1', 'pgpu_port.ps1', 'start.ps1', 'setup.ps1', 'setup-admin.ps1', 'run.ps1', 'update.ps1'):
        shutil.copy2(os.path.join(HERE, 'kit', 'tools', name), os.path.join(kit, 'tools', name))
    # 更新を聞く先と、その人の合言葉。人ごとの物なので更新では差し替えない。ひな形には入れない
    if not a.template:
        open(os.path.join(kit, 'tools', 'update.ini'), 'w', encoding='ascii', newline='\r\n').write(
            f'url={UPDATE_URL}\ntoken={update_token}\n')
    try:
        app_ver = re.match(r'RpcServer-(.+)\.ipa$', os.path.basename(newest_ipa())).group(1)
    except (OSError, ValueError):
        app_ver = ''
    open(os.path.join(kit, 'tools', 'kit.ini'), 'w', encoding='ascii', newline='\r\n').write(
        f'proto={proto_version()}\napp={app_ver}\n')
    shutil.copy2(os.path.join(HERE, 'kit', 'report.ps1'), os.path.join(kit, 'report.ps1'))
    shutil.copy2(os.path.join(HERE, 'kit', 'FAQ.txt'), os.path.join(kit, 'FAQ.txt'))
    open(os.path.join(kit, 'README.txt'), 'w', encoding='utf-8-sig', newline='\r\n').write(README)

    # Android の受け手と、android\ の中の cmd・手順書。受け手には印を入れない（人ごとの印は dll にある）
    android_sha = ''
    android_apk_sha = ''
    if not a.no_android:
        adir = os.path.join(kit, 'android')
        os.makedirs(adir, exist_ok=True)
        srv = os.path.join(a.android_dir, 'server_aarch64')
        if not os.path.exists(srv):
            raise SystemExit(f'{srv} が無い。app/vk/build_android.sh で組む')
        shutil.copy2(srv, os.path.join(adir, 'server_aarch64'))
        android_sha = sha256(srv)
        if a.with_x86_64:
            shutil.copy2(os.path.join(a.android_dir, 'server_x86_64'), os.path.join(adir, 'server_x86_64'))
        for name, text in (('_common.cmd', ANDROID_COMMON), ('start_android.cmd', ANDROID_START), ('stop_android.cmd', ANDROID_STOP), ('android_app.cmd', ANDROID_APP)):
            open(os.path.join(adir, name), 'w', encoding='ascii', newline='\r\n').write(text)
        if not a.no_android_apk:
            shutil.copy2(a.android_apk, os.path.join(adir, 'pocketgpu.apk'))
            android_apk_sha = sha256(a.android_apk)
        open(os.path.join(adir, 'README_android.txt'), 'w', encoding='utf-8-sig', newline='\r\n').write(ANDROID_README)
        for name, text in (('pocketgpu_android.cmd', ANDROID_LAUNCH), ('check_android.cmd', ANDROID_CHECK), ('Start-Android-GPU.cmd', ANDROID_EASY)):
            open(os.path.join(kit, name), 'w', encoding='ascii', newline='\r\n').write(text)
        # Android の記録を 1 つの zip にまとめる入口（10/15）。中身は tools\android-log.ps1。android\_common.cmd で adb とスマートフォンを見てから動く。
        # 公開リポジトリ pocketgpu-setup の kit\ にも同じ 2 つを置く（バイト単位で同じ。更新はファイルの SHA-256 で配る）
        shutil.copy2(os.path.join(HERE, 'kit', 'Send-Android-Log.cmd'), os.path.join(kit, 'Send-Android-Log.cmd'))
        os.makedirs(os.path.join(kit, 'tools'), exist_ok=True)
        shutil.copy2(os.path.join(HERE, 'kit', 'tools', 'android-log.ps1'), os.path.join(kit, 'tools', 'android-log.ps1'))

    # カメラ・マイク・字幕：extras\ に道具と仮想カメラ、一番上に Start-Mic-Camera.cmd、tools\ に miccam.ps1。印は入れない
    if not a.no_media:
        edir = os.path.join(kit, 'extras')
        os.makedirs(edir, exist_ok=True)
        for n in MEDIA:
            shutil.copy2(os.path.join(KITBUILD, 'extras', n), os.path.join(edir, n))
        open(os.path.join(edir, 'README.txt'), 'w', encoding='utf-8-sig', newline='\r\n').write(EXTRAS_README)
        shutil.copy2(os.path.join(HERE, 'kit', 'Start-Mic-Camera.cmd'), os.path.join(kit, 'Start-Mic-Camera.cmd'))
        shutil.copy2(os.path.join(HERE, 'kit', 'tools', 'miccam.ps1'), os.path.join(kit, 'tools', 'miccam.ps1'))

    # PC アプリ。一番上に置く（自分の横をキットのフォルダとして読む）。印は入れない
    if not a.no_app:
        shutil.copy2(os.path.join(KITBUILD, APP_EXE), os.path.join(kit, APP_EXE))

    # iPhone のアプリ：Payload/RpcServer.app/ に印のファイルを足した IPA。古い iPhone 用の PocketCam にも同じ印を足す
    src_ipa = dst_ipa = cam_ipa = None
    if not a.no_ipa:
        src_ipa = newest_ipa()
        dst_ipa = os.path.join(kit, os.path.basename(src_ipa))
        mark_ipa(src_ipa, dst_ipa, mark)
        if not a.no_media:
            cam_ipa = newest_ipa('PocketCam')
            mark_ipa(cam_ipa, os.path.join(kit, os.path.basename(cam_ipa)), mark)

    if a.template:
        print(f'TEMPLATE={kit}')
        return

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
                    sha256(dst_ipa) if dst_ipa else '', os.path.basename(src_ipa) if src_ipa else '', sha256(zpath), a.discord_id])
    shutil.rmtree(kit)  # 配るのは zip。中身のフォルダは残さない
    print(f'ZIP={zpath}')
    print(f'MARK={mark.decode()}')
    print(f'UPDATE_TOKEN={update_token}')
    if android_sha:
        print(f'ANDROID_SERVER_SHA256={android_sha}')
    if android_apk_sha:
        print(f'ANDROID_APK_SHA256={android_apk_sha}')
        print(f'ANDROID_APK_VERSION={apk_ver}')
    if cam_ipa:
        print(f'POCKETCAM_IPA={os.path.basename(cam_ipa)}')


if __name__ == '__main__':
    main()
