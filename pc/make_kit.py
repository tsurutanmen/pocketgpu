# 試す人に配る 1 人分のまとめ（Windows 側＋iPhone のアプリ）を作る。
#   python make_kit.py redmadog              → D:\iphone-rpc\tester_kit\PocketGPU-redmadog.zip
# ドライバ 2 つ（64・32 ビット）の「#」32 文字を、その人の印（名前＋乱数）に書き換える。長さは変えない。
# iPhone のアプリの IPA にも同じ印のファイルを足す（試す人が自分の Apple ID で署名し直すので、中を足しても入る）。
# 誰に何を渡したかは tester_kit\ledger.csv に足す。配った物が外に出たら、印とハッシュで誰の分かを見分ける
# Android の機種を持っている人用に、android\（受け手・start_android.cmd・stop_android.cmd・README_android.txt）と
# pocketgpu_android.cmd・check_android.cmd も入れる。Android の受け手には印を入れない（Android 側は人ごとの印が dll にあれば足りる）。
# 試すとき：PGKIT_OUT・PGKIT_KITBUILD・--android-dir で置き場を変えられる（台帳は PGKIT_OUT の下に作る）
# 入口は 2 つ：Start-PocketGPU.cmd（順に確かめて足りない物を案内する）と Run-on-iPhone-GPU.cmd（1 つのアプリを iPhone の GPU で起動する）。
# 中身は kit\tools\*.ps1（Windows PowerShell 5.1 だけで動く。Python は要らない）。tools\kit.ini に受け手との約束の版と、アプリの版を書く
import argparse, csv, datetime, hashlib, os, re, secrets, shutil, zipfile

KITBUILD = os.environ.get('PGKIT_KITBUILD', r'D:\iphone-rpc\kitbuild')        # vk/build_win.sh D:/iphone-rpc/kitbuild icd icd32 tools で組んだ物
DXVK = r'D:\iphone-rpc\dl\dxvk-macos-1103\x\dxvk-macOS-async-v1.10.3-20230507'
DXVK_LICENSE = r'D:\iphone-rpc\vkbuild\dxvk3\LICENSE'
IPA_DIR = r'D:\iphone-rpc\ipa\serve'
OUT = os.environ.get('PGKIT_OUT', r'D:\iphone-rpc\tester_kit')
ANDROID_DIR = r'D:\iphone-rpc\androidbuild'   # app\vk\build_android.sh で組んだ受け手（server_aarch64 と server_x86_64）
PLACE = b'PGKIT-MARK:' + b'#' * 32
PROTO_H = os.environ.get('PGKIT_PROTO_H', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'vk', 'remote', 'proto.h'))
# --extras で入れる、Python の要らない道具（C で組んだ物。UCRT だけを使うので VC の部品は要らない）。KITBUILD\extras\ に置く
EXTRAS = ('ipaudio.exe', 'webcam_rx.exe')

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

Steps
1. Extract the whole zip: right-click it and choose "Extract All...".
2. Install the iPhone app: open RpcServer-*.ipa with AltStore or Sideloadly and sign in with your Apple ID.
   With a free Apple ID the app stops opening after 7 days. Install it again to continue.
   AltStore needs AltServer running on the PC. After the PC restarts, start AltServer again.
3. Open the app on the iPhone and keep it in front with the screen on. Plug the iPhone into the PC with USB.
4. Double-click Start-PocketGPU.cmd. It checks each part in order and tells you what to do when something is missing.
5. To run a game or app on the iPhone GPU, drop its .exe onto Run-on-iPhone-GPU.cmd.
   You can also double-click Run-on-iPhone-GPU.cmd and choose the .exe.
   For a DirectX 11 game it offers to put two DXVK files next to the game. To undo, delete d3d11.dll and dxgi.dll there.
   Steam games: put this line in the game's Properties > Launch Options:
       "C:\path\to\this\folder\pocketgpu.cmd" %command%
   Start-PocketGPU.cmd shows this line with the real path and can copy it for you.

Good to know
- Only programs started this way see the iPhone GPU. Nothing is written to the registry.
- Programs that run as administrator cannot see the iPhone GPU. Windows does not pass the settings to them.
- Only one program at a time can use the iPhone GPU.
- Do not use games with anti-cheat such as Easy Anti-Cheat, BattlEye, VAC or Vanguard. Your account may be banned.
- In a command window you can also run:   pocketgpu.cmd "C:\path\to\app.exe" arguments

Reporting
- In the iPhone app, tap "Send logs" and send the text to us on Discord or by email.
- On the PC, send pocketgpu-start-log.txt, which Start-PocketGPU.cmd writes.
  Also double-click report.cmd and send pocketgpu-report.txt. Neither has your user name, PC name or IP.
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

手順
1. zip ファイルを右クリックして「すべて展開」を選び、すべてのファイルを展開してください
2. iPhone のアプリをインストールします。RpcServer-*.ipa を AltStore か Sideloadly で開いて、自分の Apple ID でログインしてください。
   無料の Apple ID の場合は、7 日でアプリが起動できなくなります。その場合は、インストールし直してください。
   AltStore を使う場合は、PC で AltServer が起動している必要があります。PC を再起動したら、AltServer を起動し直してください
3. iPhone でアプリを起動して、前面に表示したまま画面をオンにしておいてください。iPhone を USB ケーブルで PC に接続してください
4. Start-PocketGPU.cmd をダブルクリックしてください。必要な項目を順にチェックして、足りない物があれば対処方法を表示します
5. ゲームやアプリを iPhone の GPU で動かすには、その .exe を Run-on-iPhone-GPU.cmd にドラッグ＆ドロップしてください。
   Run-on-iPhone-GPU.cmd をダブルクリックして、.exe を選択することもできます。
   DirectX 11 のゲームの場合は、DXVK の 2 つのファイルをゲームと同じフォルダに配置するかどうかを聞きます。元に戻すときは、そのフォルダの d3d11.dll と dxgi.dll を削除してください。
   Steam のゲームは、ゲームのプロパティの「起動オプション」に次の 1 行を入力してください：
       "C:\このフォルダの場所\pocketgpu.cmd" %command%
   Start-PocketGPU.cmd は、実際のパスに置き換えたこの 1 行を表示して、コピーすることもできます

知っておくこと
- この方法で起動したアプリだけが、iPhone の GPU を認識できます。レジストリには何も書き込みません
- 管理者権限で実行されるアプリは、iPhone の GPU を認識できません。Windows が設定を渡さないためです
- iPhone の GPU を一度に使用できるアプリは 1 つだけです
- Easy Anti-Cheat、BattlEye、VAC、Vanguard などのアンチチートを導入しているゲームでは使用しないでください。アカウントが停止されることがあります
- コマンドプロンプトからは  pocketgpu.cmd "C:\アプリの場所\app.exe" 引数  でも起動できます

報告
- iPhone のアプリの「ログを送信」をタップして、そのテキストを Discord かメールで送ってください
- PC では、Start-PocketGPU.cmd が作成する pocketgpu-start-log.txt を送ってください。
  report.cmd をダブルクリックして作成される pocketgpu-report.txt も送ってください。どちらのファイルにも、ユーザー名・PC 名・IP アドレスは含まれません
- PC、アプリやゲームの名前、解像度、何が起きたかも書いてください

接続が何度も切れるとき
- まず iPhone を再起動して、USB ケーブルを接続し直してから、アプリを起動し直してください
- 他の困りごとは FAQ.txt に書いてあります

他の人が作ったソフトウェア
- dxvk\：DXVK。Gcenx さんの macOS async 版 1.10.3 で、zlib ライセンスです。dxvk\LICENSE を参照してください
'''

# --extras のときだけ extras\ に入れる説明。下書き
EXTRAS_README = r'''PocketGPU extras (draft, experimental)
- ipaudio.exe: the iPhone as the PC's speaker and microphone. Turn on "Use as PC speaker and mic" in the iPhone app, then run:  ipaudio.exe --usb
- webcam_rx.exe: the iPhone camera on the PC. Turn on "Use this iPhone as a webcam" in the iPhone app, then run:  webcam_rx.exe --usb
  Showing it to other apps as a webcam needs a one-time install with administrator rights. That is not in this kit yet.
These run without Python. Close them with Ctrl+C.

PocketGPU の追加ツール。下書きで、試験的なものです
- ipaudio.exe：iPhone を PC のスピーカーとマイクとして使うツールです。iPhone のアプリで「PC のスピーカーとマイクとして使う」をオンにして、次のコマンドを実行してください：ipaudio.exe --usb
- webcam_rx.exe：iPhone のカメラの映像を PC に表示するツールです。iPhone のアプリで「ウェブカメラとして使う」をオンにして、次のコマンドを実行してください：webcam_rx.exe --usb
  他のアプリにウェブカメラとして認識させるには、管理者権限で 1 回だけインストールするものが必要です。それはまだこのキットに含まれていません。
どちらも Python は不要です。終了するときは Ctrl+C を押してください。
'''


# ---- Android の機種を持っている人用（android\ と pocketgpu_android.cmd・check_android.cmd）----
# cmd の中の文は英語だけにする（日本語の Windows の cmd は Shift_JIS で読むので、UTF-8 の日本語は化ける）。
# 受け手（server_aarch64）は /data/local/tmp/pgpu_server に置いて、127.0.0.1 だけで待たせ、adb forward で PC につなぐ。
ANDROID_COMMON = r'''@echo off
rem PocketGPU for Android: shared part. The other android\*.cmd files call it. Do not run it by itself.
rem Sets ADB, ANDROID_SERIAL and PGPU_PORT. Exits with code 1 and a message when something is missing.
rem ADB_OK is set once adb.exe is found, so that the caller can still use adb when the phone is missing.
set "ADB_OK="
if "%PGPU_PORT%"=="" set PGPU_PORT=50300

rem ---- find adb.exe. It is not in this kit: get SDK Platform-Tools from Google ----
if defined ADB goto adb_given
for /f "delims=" %%A in ('where adb.exe 2^>nul') do if not defined ADB set "ADB=%%A"
if defined ADB goto adb_ok
for %%D in ("%LOCALAPPDATA%\Android\Sdk\platform-tools" "%USERPROFILE%\platform-tools" "%USERPROFILE%\Downloads\platform-tools" "%USERPROFILE%\Desktop\platform-tools" "C:\platform-tools" "C:\adb" "%ProgramFiles%\platform-tools" "%~dp0..\platform-tools" "%~dp0..\..\platform-tools") do if not defined ADB if exist "%%~D\adb.exe" set "ADB=%%~D\adb.exe"
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
exit /b 0

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
'''

ANDROID_START = r'''@echo off
rem PocketGPU for Android: copy the receiver to the phone, start it, and open the USB port to this PC.
rem   android\start_android.cmd
rem Needs adb.exe (Google SDK Platform-Tools). See README_android.txt.
rem Nothing is installed on the phone and no root is needed. The receiver is a file in /data/local/tmp.
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal
call "%~dp0_common.cmd"
if errorlevel 1 goto fail

set "SRV="
"%ADB%" shell getprop ro.product.cpu.abi | findstr /b /c:"arm64-v8a" >nul && set "SRV=server_aarch64"
"%ADB%" shell getprop ro.product.cpu.abi | findstr /b /c:"x86_64" >nul && set "SRV=server_x86_64"
if not defined SRV goto bad_abi
if not exist "%~dp0%SRV%" goto no_file

echo Phone:
"%ADB%" shell getprop ro.product.model
echo Receiver: %SRV%  Port: %PGPU_PORT%
echo Stopping an old receiver, if any ...
"%ADB%" shell "kill $(pidof pgpu_server) 2>/dev/null; rm -f /data/local/tmp/pgpu_server; true" >nul 2>&1
echo Copying the receiver to the phone ...
"%ADB%" push "%~dp0%SRV%" /data/local/tmp/pgpu_server >nul
if errorlevel 1 goto push_failed
echo Starting the receiver ...
"%ADB%" shell "cd /data/local/tmp && chmod 755 pgpu_server && (nohup ./pgpu_server 127.0.0.1 %PGPU_PORT% > pgpu_server.log 2>&1 &)"
ping -n 3 127.0.0.1 >nul
"%ADB%" shell pidof pgpu_server | findstr /r "[0-9]" >nul
if errorlevel 1 goto server_failed
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
echo ERROR: could not copy the receiver to the phone (see the message above).
goto fail
:server_failed
echo.
echo ERROR: the receiver did not stay running on the phone. Its log:
"%ADB%" shell cat /data/local/tmp/pgpu_server.log
echo Please send us this log together with the phone model and the Android version.
goto fail
:forward_failed
echo.
echo ERROR: could not open port %PGPU_PORT% on this PC. Another program may be using it.
echo Run android\stop_android.cmd, or use another port:   set PGPU_PORT=50301
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
echo Stopped. The receiver is removed from the phone; its log stays at /data/local/tmp/pgpu_server.log.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 0
:nodev
if defined ADB_OK "%ADB%" forward --remove tcp:%PGPU_PORT% >nul 2>&1
echo The receiver on the phone was not stopped. Connect the phone and run this file again, or restart the phone.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
'''

ANDROID_LAUNCH = r'''@echo off
rem PocketGPU: run one app or game on the Android phone GPU. Run android\start_android.cmd first.
rem   pocketgpu_android.cmd "C:\path\to\game.exe"
rem Only the app started from this file sees the phone GPU. Nothing is written to the registry.
setlocal
if "%~1"=="" (
  echo Usage: pocketgpu_android.cmd "C:\path\to\app.exe"
  exit /b 1
)
set "VK_ADD_DRIVER_FILES=%~dp0iphone_icd.json;%~dp0x86\iphone_icd.json"
if "%PGPU_PORT%"=="" set PGPU_PORT=50300
set IPHONE_VK=127.0.0.1:%PGPU_PORT%
if "%IPHONE_VK_ONLY%"=="" set IPHONE_VK_ONLY=1
if "%IPHONE_VK_ONLY%"=="1" set "VK_LOADER_DRIVERS_SELECT=*iphone_icd*"
set DISABLE_TWITCH_VULKAN_OVERLAY=1
set DISABLE_VULKAN_OBS_CAPTURE=1
set DISABLE_VULKAN_OW_OBS_CAPTURE=1
%*
'''

ANDROID_CHECK = r'''@echo off
rem PocketGPU: check that Windows can see the Android phone GPU. Run android\start_android.cmd first.
setlocal
set "VK_ADD_DRIVER_FILES=%~dp0iphone_icd.json;%~dp0x86\iphone_icd.json"
if "%PGPU_PORT%"=="" set PGPU_PORT=50300
set IPHONE_VK=127.0.0.1:%PGPU_PORT%
"%~dp0vklist.exe"
echo.
echo If you see a GPU named PocketGPU phone in the list above, it works.
echo If it is not listed, run android\start_android.cmd first.
if "%PGPU_NOPAUSE%"=="" pause
'''

# 手順だけの下書き。言い回しは鶴田さんが直す
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
   If it says "Done", go on.
5. Double-click check_android.cmd. You should see a GPU named "PocketGPU phone (...)" in the list.
6. Start a Vulkan app with:   pocketgpu_android.cmd "C:\path\to\app.exe"
   DirectX 11 games: copy d3d11.dll and dxgi.dll from dxvk\x64 (64-bit games) or dxvk\x32 (32-bit games)
   next to the game's .exe, then start the game with pocketgpu_android.cmd. Delete those two files to undo.
   Keep the phone unlocked with the screen on while the app runs.
7. When you are finished, double-click android\stop_android.cmd. It stops the receiver and removes it from the phone.
   (Restarting the phone also stops it.)

Notes
- Only the app started from pocketgpu_android.cmd sees the phone GPU. Nothing is written to the registry.
- The receiver listens only inside the phone (127.0.0.1). It is reachable only through the USB cable.
- Do not use games with anti-cheat (Easy Anti-Cheat, BattlEye, VAC, Vanguard). Your account may be banned.
- The iPhone files in this kit (RpcServer-*.ipa, Start-PocketGPU.cmd, Run-on-iPhone-GPU.cmd, pocketgpu.cmd) are not needed for Android.
- Another port: set PGPU_PORT=50301 before step 4, 5 and 6 (use the same number in all of them).
- More than one phone connected: run   adb devices   and set ANDROID_SERIAL=the-serial before step 4.
- To send the window picture as video (less data, the PC needs to decode H.264): set IPHONE_VK_VIDEO=1 before step 6.

If something does not work
- "adb.exe was not found": do step 2.
- "not allowed yet": unlock the phone and tap Allow. If no question appears, unplug and plug in again, or in Developer options use "Revoke USB debugging authorizations".
- "no Android phone found": try another cable or USB port, set the USB mode to "File transfer", and check step 1.
  Some makers need their own USB driver on the PC.
- "the receiver did not stay running": the script prints the receiver's log. Send it to us.
- The picture stops or the connection drops: run android\stop_android.cmd, then android\start_android.cmd again.

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
   「Done」と表示されたら、次へ進んでください
5. check_android.cmd をダブルクリックしてください。一覧に「PocketGPU phone (…)」という名前の GPU が表示されれば成功です
6. Vulkan のアプリは、pocketgpu_android.cmd "C:\アプリの場所\app.exe" で起動してください。
   DirectX 11 のゲームの場合は、64 ビットのゲームなら dxvk\x64、32 ビットのゲームなら dxvk\x32 の d3d11.dll と dxgi.dll を、
   ゲームの .exe と同じフォルダにコピーしてから、pocketgpu_android.cmd で起動してください。元に戻すときは、その 2 つのファイルを削除してください。
   アプリが動作している間は、スマートフォンのロックを解除したまま、画面をオンにしておいてください
7. 終わったら、android\stop_android.cmd をダブルクリックしてください。受信アプリを停止して、スマートフォンから削除します
   スマートフォンを再起動しても、受信アプリは停止します

メモ
- pocketgpu_android.cmd から起動したアプリだけが、スマートフォンの GPU を認識できます。レジストリには何も書き込みません
- 受信アプリは、スマートフォンの中の 127.0.0.1 だけで待ち受けています。USB ケーブルを通してしか接続できません
- Easy Anti-Cheat、BattlEye、VAC、Vanguard などのアンチチートを導入しているゲームでは使用しないでください。アカウントが停止されることがあります
- このキットの iPhone 用のファイル RpcServer-*.ipa、Start-PocketGPU.cmd、Run-on-iPhone-GPU.cmd、pocketgpu.cmd は、Android では必要ありません
- 別のポート番号を使う場合は、手順 4・5・6 の前に set PGPU_PORT=50301 を入力してください。3 つの手順で同じ番号を使ってください
- スマートフォンを 2 台以上接続している場合は、adb devices を入力し、手順 4 の前に set ANDROID_SERIAL=一覧の番号 を入力してください
- ウィンドウの映像を動画で送る場合は、手順 6 の前に set IPHONE_VK_VIDEO=1 を入力してください。通信量が減りますが、PC で H.264 をデコードする必要があります

動作しないとき
- 「adb.exe was not found」：手順 2 を実行してください
- 「not allowed yet」：スマートフォンのロックを解除して「許可」をタップしてください。何も表示されない場合は、ケーブルを抜き差しするか、開発者向けオプションの「USB デバッグの承認を取り消す」をタップしてください
- 「no Android phone found」：別のケーブルや USB ポートを試してください。USB の用途を「ファイル転送」にして、手順 1 をチェックしてください。
  メーカーによっては、PC にそのメーカーの USB ドライバが必要です
- 「the receiver did not stay running」：受信アプリのログが画面に表示されます。それを送ってください
- 映像が止まる、または接続が切れる：android\stop_android.cmd を実行してから、もう一度 android\start_android.cmd を実行してください

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
    data = open(src, 'rb').read()
    if data.count(PLACE) != 1:
        raise SystemExit(f'{src} の印の場所が 1 つではない（{data.count(PLACE)} 個）。kitbuild を組み直す')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(data.replace(PLACE, b'PGKIT-MARK:' + mark))


def proto_version():
    # 「始める」が受け手にあいさつするときの版。ドライバと同じ数を tools\kit.ini に書く
    m = re.search(r'#define VR_PROTO_VERSION (\d+)', open(PROTO_H, encoding='utf-8').read())
    if not m:
        raise SystemExit(f'{PROTO_H} に VR_PROTO_VERSION が無い')
    return int(m.group(1))


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
    ap.add_argument('--android-dir', default=ANDROID_DIR, help='Android の受け手（server_aarch64）がある所')
    ap.add_argument('--with-x86_64', action='store_true', help='エミュレーター用の server_x86_64 も入れる（試験用・配るキットには入れない）')
    ap.add_argument('--no-android', action='store_true', help='Android の物を入れない（今までと同じ中身）')
    ap.add_argument('--no-ipa', action='store_true', help='iPhone のアプリ（IPA）を入れない（試験用）')
    ap.add_argument('--extras', action='store_true', help='Python の要らないおまけ（ipaudio.exe・webcam_rx.exe）を extras に入れる。KITBUILD の extras から')
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
    for name, text in (('pocketgpu.cmd', LAUNCH), ('report.cmd', REPORT)):
        open(os.path.join(kit, name), 'w', encoding='ascii', newline='\r\n').write(text)
    # 入口 2 つと、その中身（PowerShell）。前の check.cmd は Start-PocketGPU.cmd の 7 番目が代わる
    for name in ('Start-PocketGPU.cmd', 'Run-on-iPhone-GPU.cmd'):
        shutil.copy2(os.path.join(HERE, 'kit', name), os.path.join(kit, name))
    os.makedirs(os.path.join(kit, 'tools'), exist_ok=True)
    for name in ('pgpu_common.ps1', 'start.ps1', 'run.ps1'):
        shutil.copy2(os.path.join(HERE, 'kit', 'tools', name), os.path.join(kit, 'tools', name))
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
        for name, text in (('_common.cmd', ANDROID_COMMON), ('start_android.cmd', ANDROID_START), ('stop_android.cmd', ANDROID_STOP)):
            open(os.path.join(adir, name), 'w', encoding='ascii', newline='\r\n').write(text)
        open(os.path.join(adir, 'README_android.txt'), 'w', encoding='utf-8-sig', newline='\r\n').write(ANDROID_README)
        for name, text in (('pocketgpu_android.cmd', ANDROID_LAUNCH), ('check_android.cmd', ANDROID_CHECK)):
            open(os.path.join(kit, name), 'w', encoding='ascii', newline='\r\n').write(text)

    # Python の要らないおまけ。印は入れない
    if a.extras:
        edir = os.path.join(kit, 'extras')
        os.makedirs(edir, exist_ok=True)
        for n in EXTRAS:
            src = os.path.join(KITBUILD, 'extras', n)
            if not os.path.exists(src):
                raise SystemExit(f'{src} が無い。work/audio と work/webcam で組んだ物を置く')
            shutil.copy2(src, os.path.join(edir, n))
        open(os.path.join(edir, 'README.txt'), 'w', encoding='utf-8-sig', newline='\r\n').write(EXTRAS_README)

    # iPhone のアプリ：Payload/RpcServer.app/ に印のファイルを足した IPA
    src_ipa = dst_ipa = None
    if not a.no_ipa:
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
                    sha256(dst_ipa) if dst_ipa else '', os.path.basename(src_ipa) if src_ipa else '', sha256(zpath), a.discord_id])
    shutil.rmtree(kit)  # 配るのは zip。中身のフォルダは残さない
    print(f'ZIP={zpath}')
    print(f'MARK={mark.decode()}')
    if android_sha:
        print(f'ANDROID_SERVER_SHA256={android_sha}')


if __name__ == '__main__':
    main()
