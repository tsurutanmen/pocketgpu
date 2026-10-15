# Kit layout / キットの中での置き場所

`kit/` の中身は、キットの一番上に、そのままの場所・そのままの中身で入ります。

| このリポジトリ | キットの中 | 中身 |
| --- | --- | --- |
| `kit/Start-PocketGPU.cmd` | 同じ場所 | 自動セットアップの入口 |
| `kit/Setup-PocketGPU.cmd` | 同じ場所 | 全部入れる・設定する入口（見る → 聞いて入れる → iPhone のアプリの案内 → 最後のチェック） |
| `kit/Run-on-iPhone-GPU.cmd` | 同じ場所 | アプリを GPU を指定して起動する入口 |
| `kit/Update-PocketGPU.cmd` | 同じ場所 | 更新の入口 |
| `kit/Fix-PocketGPU.cmd` | 同じ場所 | 自動修復の入口 |
| `kit/report.cmd`・`kit/report.ps1` | 同じ場所 | PC の様子を 1 つのテキストにまとめる |
| `kit/pocketgpu.cmd` | 同じ場所 | 起動の入口（Steam の起動オプション用） |
| `kit/pocketgpu_android.cmd`・`kit/check_android.cmd` | 同じ場所 | Android 用の入口 |
| `kit/Send-Android-Log.cmd` | 同じ場所 | Android の記録を 1 つの zip にまとめる入口。`android\_common.cmd` で adb とスマートフォンを見てから、`tools\android-log.ps1` を動かす。キットに `android\` が入るときだけ入る |
| `kit/Test-Android-Speed.cmd` | 同じ場所 | Android の USB の速さを測る入口。`android\_common.cmd` で adb とスマートフォンを見てから、`tools\android-speed.ps1` を動かす。キットに `android\` が入るときだけ入る |
| `kit/android/*.cmd` | `android\` | Android の受け手を入れる・止める。`android_app.cmd` は PocketGPU アプリ用（カメラ・マイク・スピーカー・GPU の口をつなぐ）。`_common.cmd` は共通部分（adb とスマートフォンを探す・PC の口を決める。`PGPU_FIXED_PORTS=1` なら口を決めずに環境変数のまま使う）。`_gamemode.cmd` は `start_android.cmd` と `android_app.cmd` が受け手の後に呼ぶ、ゲームモード（performance）の頼み |
| `kit/tools/*.ps1` | `tools\` | 上の入口の中身（Windows PowerShell 5.1）。`pgpu_port.ps1` は Android の PC 側の口を選んで `android\port.txt` に覚える。`android-log.ps1` は `Send-Android-Log.cmd` の中身、`android-speed.ps1` は `Test-Android-Speed.cmd` の中身 |
| `kit/tools/setup.ps1`・`kit/tools/setup-admin.ps1` | `tools\` | `Setup-PocketGPU.cmd` の中身。`setup-admin.ps1` は管理者権限が要る物（winget の iTunes・Vulkan Runtime・Visual C++・Apple のサービスの開始）を、UAC 1 回でまとめて受け取る小さな部分で、`setup.ps1` からだけ起動する。受け取るのは決まった名前だけ |
| `kit/tools/fix-data/*.cmd` | `tools\fix-data\` | Fix-PocketGPU.cmd が、古い入口と入れ替える新しい物 |

## 守ること / Rules

- `kit/tools/fix-data/` の 3 つは、`kit/` の一番上の同じ名前の 3 つと、**同じ中身**にします（バイト単位で同じ）。入口を入れ替えたあと、更新の SHA-256 の確認が合うようにするためです。
- 改行は CRLF のまま使います。`.gitattributes` が、チェックアウトで変わらないようにしています。
- `.ps1` は UTF-8（BOM 付き）、`.cmd` は ASCII だけです。`tests/Normalize-Files.ps1` で直せます。
- この中に、合言葉・人ごとの印・`update.ini`・`kit.ini`・DLL・exe・ipa は入れません。

## キットに入る、このリポジトリにない物 / Not in this repository

- `iphone_icd.dll`・`iphone_icd.json`・`x86\`・`vklist.exe`
- `PocketGPU.exe`・`extras\`（カメラ・マイク・字幕の道具と仮想カメラ）
- iPhone のアプリ（ipa）・Android の受け手（`android\server_aarch64`）・Android のアプリ（`android\pocketgpu.apk`）
- `tools\kit.ini`・`tools\update.ini`（人ごとの設定）
- `android\port.txt`（Windows が PC の口を予約していたとき、`tools\pgpu_port.ps1` が選んで覚えた口。使っているうちにできる。`gpu=47300` の形。消すと既定に戻る）
- `README.txt`・`FAQ.txt`・`dxvk\`

これらは、キットを組む側が入れます。道具（`kit/` の `.cmd` と `.ps1`）は、これらを呼び出すだけです。
