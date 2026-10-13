# Kit layout / キットの中での置き場所

`kit/` の中身は、キットの一番上に、そのままの場所・そのままの中身で入ります。

| このリポジトリ | キットの中 | 中身 |
| --- | --- | --- |
| `kit/Start-PocketGPU.cmd` | 同じ場所 | 自動セットアップの入口 |
| `kit/Run-on-iPhone-GPU.cmd` | 同じ場所 | アプリを GPU を指定して起動する入口 |
| `kit/Update-PocketGPU.cmd` | 同じ場所 | 更新の入口 |
| `kit/Fix-PocketGPU.cmd` | 同じ場所 | 自動修復の入口 |
| `kit/report.cmd`・`kit/report.ps1` | 同じ場所 | PC の様子を 1 つのテキストにまとめる |
| `kit/pocketgpu.cmd` | 同じ場所 | 起動の入口（Steam の起動オプション用） |
| `kit/pocketgpu_android.cmd`・`kit/check_android.cmd` | 同じ場所 | Android 用の入口 |
| `kit/android/*.cmd` | `android\` | Android の受け手を入れる・止める |
| `kit/tools/*.ps1` | `tools\` | 上の入口の中身（Windows PowerShell 5.1） |
| `kit/tools/fix-data/*.cmd` | `tools\fix-data\` | Fix-PocketGPU.cmd が、古い入口と入れ替える新しい物 |

## 守ること / Rules

- `kit/tools/fix-data/` の 3 つは、`kit/` の一番上の同じ名前の 3 つと、**同じ中身**にします（バイト単位で同じ）。入口を入れ替えたあと、更新の SHA-256 の確認が合うようにするためです。
- 改行は CRLF のまま使います。`.gitattributes` が、チェックアウトで変わらないようにしています。
- `.ps1` は UTF-8（BOM 付き）、`.cmd` は ASCII だけです。`tests/Normalize-Files.ps1` で直せます。
- この中に、合言葉・人ごとの印・`update.ini`・`kit.ini`・DLL・exe・ipa は入れません。

## キットに入る、このリポジトリにない物 / Not in this repository

- `iphone_icd.dll`・`iphone_icd.json`・`x86\`・`vklist.exe`
- `PocketGPU.exe`・`extras\`（カメラ・マイク・字幕の道具と仮想カメラ）
- iPhone のアプリ（ipa）・Android の受け手（`android\server_aarch64`）
- `tools\kit.ini`・`tools\update.ini`（人ごとの設定）
- `README.txt`・`FAQ.txt`・`dxvk\`

これらは、キットを組む側が入れます。道具（`kit/` の `.cmd` と `.ps1`）は、これらを呼び出すだけです。
