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
- Android scripts in `kit/android/`
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
| `android` | Two receivers, old receiver, ports in use, adb forward | Reinstalls, reconnects or stops | Permission and cable advice |
| `gpu` | The phone GPU seen through the driver | (fixed by the items above) | How to check your antivirus |

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

### 直すのを手伝ってくれる方へ

[CONTRIBUTING.md](CONTRIBUTING.md) を見てください。試験は `tests\run-tests.ps1` で動きます。
