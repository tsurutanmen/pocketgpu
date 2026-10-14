# PocketGPU Setup

Setup and repair tools for PocketGPU, which uses a phone as a second GPU, webcam and microphone for a Windows PC.

This repository has only the scripts that install, check and repair a PocketGPU kit. The driver and the phone apps are not here. They come with the tester kit.

- Discord: https://discord.gg/F3Mk8DBxqQ
- License: MIT

日本語は下にあります。

## What is in here

- `Start-PocketGPU.cmd`: checks the PC and the phone step by step
- `Fix-PocketGPU.cmd`: finds problems and fixes the ones it can
- `Update-PocketGPU.cmd`: downloads only the files that changed
- `Run-on-iPhone-GPU.cmd`: starts a game or app on the phone GPU
- Android scripts in `kit/android/`: `start_android.cmd` and `stop_android.cmd` for the GPU receiver, `android_app.cmd` for the PocketGPU app (camera, microphone, speaker and GPU)
- Tests in `tests/`

Not in here: the GPU driver (`iphone_icd.dll`), the iPhone and Android apps, the webcam and audio tools, and `PocketGPU.exe`.

## Fix-PocketGPU

Double-click `Fix-PocketGPU.cmd` in your kit folder. It does not need administrator rights.

- It asks `y/N` before each fix. It does not delete anything. Replaced files are kept in `fix-backup\`.
- For problems it cannot fix, it tells you what to do and which page to open.
- At the end it writes a short summary to `pocketgpu-fix-summary.txt`. You can paste it in Discord or in an Issue. It has no tokens, user names, PC names or folder paths.

Options:

- `-CheckOnly`: check only, change nothing
- `-Only apple,gpu`: run only these items
- `-KitDir "folder"`: use a kit in another folder

| Item | What it checks | Fix | If it can't fix it |
| --- | --- | --- | --- |
| `pc` | Administrator rights, UAC, characters in the folder path | (advice only) | Open a normal window, move the kit to `C:\PocketGPU` |
| `files` | Missing or broken kit files | Downloads them again with `update.ps1` | Extract the kit again, ask on Discord |
| `loader` | Vulkan loader older than 1.3.234, old launch scripts | Replaces the launch scripts | GPU driver download page |
| `apple` | Apple Mobile Device Service missing or stopped | Starts it, or opens Apple Devices | Administrator steps, Store page |
| `tools` | The same tool running twice | Stops the older one | |
| `iphone` | iPhone seen over USB, receiver answering | Stops the program holding the receiver | Reopen or reinstall the iPhone app |
| `android` | Two receivers, old receiver, ports in use, adb forward. A PC port inside a range that Windows reserved. With the PocketGPU app: the camera and mic ports, in the form that fits the app version | Reinstalls, reconnects or stops. Picks a free port outside the reserved ranges and saves it. Reconnects the camera and mic ports in the right form | Permission and cable advice |
| `gpu` | The phone GPU seen through the driver | (fixed by the items above) | How to check your antivirus |

### Android app ports

The camera and mic ports of the PocketGPU app depend on the app version.

| App | Camera | Mic and speaker |
| --- | --- | --- |
| 0.1.2 and newer | `adb forward tcp:50064 localabstract:pocketgpu-webcam` | `adb forward tcp:50063 localabstract:pocketgpu-audio` |
| 0.1.1 and older | `adb forward tcp:50064 tcp:50064` | `adb forward tcp:50063 tcp:50063` |

`android\android_app.cmd` reads the app version from the phone and uses the matching form. If it cannot read the version, it uses the new form first and the old form if adb refuses it. Set `PGPU_APP_FORWARD=new` or `PGPU_APP_FORWARD=old` to choose the form yourself. The 0.1.2 app also takes the old form when "Also accept old PC tools" is on in its settings, but that is off by default and lets other apps on the phone reach the camera and mic.

The GPU port (50300 on the PC) also depends on the app version, when the app runs the GPU receiver itself:

| App | GPU |
| --- | --- |
| 0.1.4 and newer | `adb forward tcp:50300 localabstract:pocketgpu-gpu` |
| 0.1.3 and older | `adb forward tcp:50300 tcp:50060` |

The 0.1.4 app does not need port 50060 on the phone to be free. If the version cannot be read, or adb refuses the first form, `android\android_app.cmd` uses `tcp:50060`. Set `PGPU_APP_GPU=usb` or `PGPU_APP_GPU=tcp` to choose yourself. When the kit receiver from `android\start_android.cmd` runs, the GPU port goes to it (`tcp:50300`) as before.

### Ports reserved by Windows

`adb forward` listens on a port of this PC (50300 for the GPU, 50064 for the webcam, 50063 for the audio). Some PCs reserve port ranges for Hyper-V or WSL (`netsh int ipv4 show excludedportrange protocol=tcp` shows them), and then adb cannot listen on a port inside a range. Only the Android part listens on the PC; the iPhone side connects to Apple's service and listens on nothing.

The Android cmd files in `android\` check the port before they use it. If it is reserved, or another program uses it, `tools\pgpu_port.ps1` picks a free port outside the reserved ranges (47300 and up for the GPU, 47400 for the webcam, 47500 for the audio), prints one line such as `Port 50300 is reserved by Windows, so port 47300 is used instead`, and saves the port in `android\port.txt`. `pocketgpu_android.cmd`, `check_android.cmd`, `android_app.cmd`, `stop_android.cmd` and `tools\run.ps1` read that file, so they use the same port. The order is: the variable (`PGPU_PORT`, `PGPU_CAM_PORT`, `PGPU_AUDIO_PORT`), then `android\port.txt`, then the default. Delete `android\port.txt` to forget the saved ports. `Fix-PocketGPU.cmd` finds a port inside a reserved range, asks, and does the same. It never changes the Windows reservation.

## Linux: fix.sh

For the Linux kit (CachyOS, Arch, Linux Mint, Ubuntu, Debian, Fedora, SteamOS). It is the Linux version of `Fix-PocketGPU.cmd`. The file is `linux/fix.sh`.

- Copy `fix.sh` into the kit folder and run `bash fix.sh`. Or point it at the kit from anywhere: `bash fix.sh --kit ~/PocketGPU-linux-yourname`
- No sudo. Run it as the user who starts the games.
- It asks `y/N` before each fix. It does not delete anything. Replaced files and the old secret file go to `fix-backup/`.
- Anything that needs sudo (packages, services, groups, kernel modules) is shown as a command for your distribution. It does not run it.
- At the end it writes `pocketgpu-fix-summary.txt`. Paste it in Discord or in an Issue. It has no secrets, user names, PC names, folder paths, phone serials or iPhone UDIDs.
- adb comes from `sudo apt install adb` on Debian, Ubuntu and Linux Mint, `sudo pacman -S android-tools` on Arch and CachyOS, and `sudo dnf install android-tools` on Fedora.

Options:

- `--check-only`: check only, change nothing
- `--only usbmuxd,gpu`: run only these items
- `--kit folder`: use a kit in another folder

| Item | What it checks | Fix | If it can't fix it |
| --- | --- | --- | --- |
| `pc` | x86_64, glibc 2.31+, run as root, kit on a `noexec` drive, X11 or Wayland | (advice only) | Run without sudo, extract the kit in your home folder |
| `files` | Missing or broken kit files: 64/32-bit ELF, driver json, CRLF line endings, missing execute permission | Fixes line endings, `chmod +x`, writes the json again | Extract the kit again from the tar.gz |
| `loader` | `libvulkan.so.1` and its version. Older than 1.3.207: only `VK_ICD_FILENAMES` works. Older than 1.3.234: no `VK_ADD_DRIVER_FILES`. Variables that hide the driver. 32-bit loader | (advice only) | Install command per distribution |
| `usbmuxd` | usbmuxd and idevicepair installed, usbmuxd running, an old socket left after a crash, socket permission | (needs sudo) | `sudo systemctl restart usbmuxd`, install command |
| `udev` | usbmuxd and Android udev rules, the group the rule needs (`plugdev` and so on), a group that needs a new login | (needs sudo) | `sudo usermod -aG ...`, log in again |
| `tools` | `start-mic-camera.sh` running twice, leftover camera/mic tools, the Flatpak USB helper (`--broker`) and its secret file | Stops the older one, moves the old secret file to `fix-backup/` | |
| `iphone` | iPhone seen by usbmuxd, pairing, the app answering, a program holding the iPhone GPU | Runs `idevicepair pair`, stops the program holding the GPU | Unlock and tap Trust, reopen or reinstall the app |
| `android` | adb, phone allowed, `no permissions`, receiver running twice or old, `adb forward` missing, stale or pointing elsewhere, the camera and mic ports in the form that fits the app version | Restarts the adb server, runs `start-android.sh`, redoes `adb forward`, stops a program on the port | Allow USB debugging, cable advice |
| `media` | PipeWire or PulseAudio, leftover PocketGPU virtual mic, v4l2loopback and `exclusive_caps`, `/dev/video*` permission, FFmpeg | Unloads the leftover PocketGPU modules only | `modprobe` and install commands |
| `gpu` | The phone GPU seen through the driver | (fixed by the items above) | Lists the items that are still broken |

When the PocketGPU app runs the GPU receiver itself, `start-android.sh --app` uses the same forms as the table above: `adb forward tcp:50300 localabstract:pocketgpu-gpu` for the 0.1.4 app and newer, and `adb forward tcp:50300 tcp:50060` for 0.1.3 and older. `fix.sh` accepts both and redoes the one that fits the app version.

Tests: `bash linux/tests/run-tests.sh`. They use fake usbmuxd, adb, pgpu-probe, vulkaninfo and pactl only. With `--real-probe path/to/pgpu-probe` the iPhone tests also run with the real probe against a fake usbmuxd. With `--android-src path/to/kit_linux` the Android test also runs the real `start-android.sh` of the kit on the fake adb. They need bash 4.4 or newer, python3, `ss` and coreutils.

## Development

- Windows 10 or 11, Windows PowerShell 5.1, Python 3
- Run the tests: `powershell -ExecutionPolicy Bypass -File tests\run-tests.ps1`
- The tests use fake devices only. They do not use a real phone, Apple services or a GPU.
- See [CONTRIBUTING.md](CONTRIBUTING.md).

## Reporting a bug

Open an [Issue](../../issues), choose "Bug report", and paste the contents of `pocketgpu-fix-summary.txt`.

---

## 日本語

PocketGPU は、スマホを Windows の PC の 2 枚目の GPU・ウェブカメラ・マイクにする仕組みです。このリポジトリには、キットを入れる・チェックする・直すスクリプトだけが入っています。ドライバとスマホのアプリは、テスター向けのキットに入っています。

- Discord：https://discord.gg/F3Mk8DBxqQ
- ライセンス：MIT

### 使い方

- `Start-PocketGPU.cmd`：PC とスマホを順にチェックします
- `Fix-PocketGPU.cmd`：問題を探して、直せる物を直します。直す前に 1 つずつ `y/N` で聞きます。何も消しません。入れ替えた物は `fix-backup\` に残します
- `Update-PocketGPU.cmd`：変わったファイルだけをダウンロードします
- `Run-on-iPhone-GPU.cmd`：ゲームやアプリを iPhone の GPU で起動します

直らないときは、`pocketgpu-fix-summary.txt` の中身を Discord か Issue に貼ってください。合言葉・ユーザー名・PC の名前・フォルダの場所は入っていません。

### Android：Windows が予約している口

`adb forward` は、この PC の口（GPU は 50300、ウェブカメラは 50064、音声は 50063）で待ちます。Hyper-V や WSL のために Windows が口の範囲を予約している PC があり（`netsh int ipv4 show excludedportrange protocol=tcp` で見えます）、範囲の中の口では adb が待てません。PC で待つのは Android の部分だけです。iPhone は Apple のサービスにつなぐだけで、PC では何も待ちません。

`android\` の cmd は、使う前に口をチェックします。予約されている口や、ほかのプログラムが使っている口なら、`tools\pgpu_port.ps1` が、予約の範囲の外の空いている口（GPU は 47300、ウェブカメラは 47400、音声は 47500 から順）を選び、`ポート 50300 は Windows に予約されているので、47300 を使います` のような 1 行を出して、`android\port.txt` に覚えます。`pocketgpu_android.cmd`・`check_android.cmd`・`android_app.cmd`・`stop_android.cmd`・`tools\run.ps1` はそのファイルを読むので、同じ口を使います。決める順番は、環境変数（`PGPU_PORT`・`PGPU_CAM_PORT`・`PGPU_AUDIO_PORT`）、`android\port.txt`、既定の順です。覚えた口を消すには、`android\port.txt` を削除してください。`Fix-PocketGPU.cmd` は、予約の範囲に入っている口を見つけて、聞いてから、同じように直します。Windows の予約の設定は変えません。

### Linux：fix.sh

Linux 版のキット（CachyOS・Arch・Linux Mint・Ubuntu・Debian・Fedora・SteamOS）を直す台本です。Windows の `Fix-PocketGPU.cmd` の Linux 版で、ファイルは `linux/fix.sh` です。

- `fix.sh` をキットのフォルダに置いて、`bash fix.sh` を実行します。どこからでも `bash fix.sh --kit ~/PocketGPU-linux-あなたの名前` の形でキットを指定することもできます
- sudo は要りません。ゲームを起動するのと同じユーザーで実行してください
- 直す前に 1 つずつ `y/N` で聞きます。何も消しません。入れ替えた物と古い合言葉のファイルは `fix-backup/` に残します
- sudo が要る物（パッケージ・サービス・グループ・カーネルのモジュール）は、ディストリごとのコマンドを出すだけで、実行しません
- 最後に `pocketgpu-fix-summary.txt` を書きます。Discord か Issue に貼ってください。合言葉・ユーザー名・PC の名前・フォルダの場所・スマホのシリアル・iPhone の UDID は入っていません
- adb は、Debian・Ubuntu・Linux Mint では `sudo apt install adb`、Arch・CachyOS では `sudo pacman -S android-tools`、Fedora では `sudo dnf install android-tools` で入ります

| 項目 | 見る所 | 直し方 | 直せないときの案内 |
| --- | --- | --- | --- |
| `pc` | x86_64・glibc 2.31 以上・root で実行していないか・キットが noexec の場所に無いか・X11 か Wayland か | （案内だけ） | sudo を付けずに実行・ホームに展開し直す |
| `files` | キットのファイルの欠け・壊れ（ELF の 64・32 ビット、json の中身、CRLF の行末、実行の権限） | 行末を直す・`chmod +x`・json を作り直す | tar.gz から展開し直す |
| `loader` | `libvulkan.so.1` の有無と版（1.3.207 より前は `VK_ICD_FILENAMES` だけ、1.3.234 より前は `VK_ADD_DRIVER_FILES` が無い）・ドライバを隠す環境変数・32 ビットのローダー | （案内だけ） | ディストリごとの入れ方 |
| `usbmuxd` | usbmuxd と idevicepair が入っているか・動いているか・落ちたあとの古いソケット・ソケットの権限 | （sudo が要る） | `sudo systemctl restart usbmuxd`・入れ方 |
| `udev` | usbmuxd と Android の udev の規則・規則が求めるグループ（plugdev など）・ログインし直しが要るか | （sudo が要る） | `sudo usermod -aG …`・ログインし直す |
| `tools` | `start-mic-camera.sh` の二重起動・残ったカメラとマイクの道具・Flatpak 用の受け渡し役（`--broker`）と合言葉のファイル | 古い方を止める・古い合言葉のファイルを `fix-backup/` に移す | |
| `iphone` | usbmuxd に見えるか・ペアリング・アプリが答えるか・GPU を握っているプログラム | `idevicepair pair` を実行・握っているプログラムを止める | ロックを解除して「信頼」・アプリを開き直す |
| `android` | adb・許可・`no permissions`・受け手の二重と古さ・`adb forward` の欠け・残り・向け先の間違い・アプリの版に合うカメラとマイクの向け方 | adb のサーバーを起動し直す・`start-android.sh` を実行・`adb forward` をやり直す・ポートを使うプログラムを止める | USB デバッグの許可・ケーブルの案内 |
| `media` | PipeWire か PulseAudio・残った PocketGPU の仮想マイク・v4l2loopback と `exclusive_caps`・`/dev/video*` の権限・FFmpeg | 残った PocketGPU のモジュールだけを外す | `modprobe` と入れ方のコマンド |
| `gpu` | ドライバ経由でスマホの GPU が見えるか | （上の項目で直す） | まだ直っていない項目を挙げる |

アプリが GPU の受け手も動かしているとき、`start-android.sh --app` は上の表と同じ向け方を使います。0.1.4 以上のアプリは `adb forward tcp:50300 localabstract:pocketgpu-gpu`、0.1.3 以前は `adb forward tcp:50300 tcp:50060` です。`fix.sh` はどちらも正しいものとして読み、やり直すときはアプリの版に合う方でつなぎます。

試験：`bash linux/tests/run-tests.sh`。偽の usbmuxd・adb・pgpu-probe・vulkaninfo・pactl だけを使います。`--real-probe pgpu-probe の場所` を付けると、iPhone の試験を本物の pgpu-probe と偽の usbmuxd でも動かします。`--android-src kit_linux の場所` を付けると、Android の試験で、キットの本物の `start-android.sh` も偽の adb の上で動かします。bash 4.4 以上・python3・`ss`・coreutils が要ります。

### Android アプリの口

PocketGPU アプリのカメラとマイクの口は、アプリの版によって向け方が違います。

| アプリ | カメラ | マイクとスピーカー |
| --- | --- | --- |
| 0.1.2 以上 | `adb forward tcp:50064 localabstract:pocketgpu-webcam` | `adb forward tcp:50063 localabstract:pocketgpu-audio` |
| 0.1.1 以前 | `adb forward tcp:50064 tcp:50064` | `adb forward tcp:50063 tcp:50063` |

`android\android_app.cmd` は、スマートフォンからアプリの版を読んで、合う向け方でつなぎます。版が読めないときは、新しい向け方を先に使い、adb が断ったら古い向け方にします。向け方を自分で決めるときは、`PGPU_APP_FORWARD=new` か `PGPU_APP_FORWARD=old` を設定してください。0.1.2 のアプリは、設定の「古い PC のツールも受け付ける」をオンにすると古い向け方も受けますが、既定はオフです。オンにすると、スマートフォンの他のアプリからもカメラとマイクに届いてしまいます。

アプリが GPU の受け手も動かしているときは、GPU の口（PC の 50300）の向け方も、アプリの版によって違います。

| アプリ | GPU |
| --- | --- |
| 0.1.4 以上 | `adb forward tcp:50300 localabstract:pocketgpu-gpu` |
| 0.1.3 以前 | `adb forward tcp:50300 tcp:50060` |

0.1.4 のアプリは、スマートフォンの 50060 が空いていなくても動きます。版が読めないときや、adb が最初の向け方を断ったときは、`android\android_app.cmd` は `tcp:50060` を使います。自分で決めるときは、`PGPU_APP_GPU=usb` か `PGPU_APP_GPU=tcp` を設定してください。`android\start_android.cmd` のキットの受け手が動いているときは、今まで通り、そちら（`tcp:50300`）につなぎます。

### 直すのを手伝ってくれる方へ

[CONTRIBUTING.md](CONTRIBUTING.md) を見てください。試験は `tests\run-tests.ps1` で動きます。
