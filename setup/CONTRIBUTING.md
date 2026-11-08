# Contributing / 貢献のしかた

## 最初に / Before you start

- このリポジトリには、入れる・直す道具だけを入れます。ドライバ・アプリ・受け手・`PocketGPU.exe` などの中核は入れません。それらを入れる変更は受け取れません。
- 合言葉・トークン・人ごとの印・メールアドレス・自分の PC のパスを、コード・ログ・Issue・PR に書かないでください。

## 開発の始め方 / Setup

- Windows 10 または 11・Windows PowerShell 5.1・Python 3
- 試験：`powershell -ExecutionPolicy Bypass -File tests\run-tests.ps1`
  - 特定の場面だけ：`-Scenario loader,files`
  - 本物の iPhone・Android・Apple のサービス・GPU・インターネットは使いません
- 秘密が入っていないかの検索：`powershell -ExecutionPolicy Bypass -File tests\Test-NoSecrets.ps1`

## 書き方 / Style

- `.ps1`：UTF-8（BOM 付き）・改行は CRLF。Windows PowerShell 5.1 で動く書き方だけ（`&&`・`?:`・`??` は使いません）
- `.cmd`：ASCII だけ・改行は CRLF（日本語の Windows の cmd は Shift_JIS で読むため）
- 画面の文は `M '日本語' 'English'` の組で書きます
- 直す動作は、必ず `y/N` で聞いてから行います。自分では何も消しません。入れ替える前の物は残します
- 試験用の環境変数は `PGPU_TEST_*` の名前にして、ファイルの先頭のコメントに書きます
- 書き方をそろえる：`powershell -ExecutionPolicy Bypass -File tests\Normalize-Files.ps1`

## 新しい項目を足す / Adding a fix

1. `kit/tools/fix.ps1` に、見る・直す・確かめ直す を足します
2. `tests/run-tests.ps1` に、わざと壊した場面を足します。次の 3 通りで動かします
   - `fix`：直ったことを、本当の状態（ファイル・プロセス・口）で確かめる
   - `decline`：「n」と答えると何も変わらない
   - `sabotage`：直す部分を外すと、確認が失敗する
3. `README.md` の表に 1 行足します

## Pull request の流れ / Pull requests

- 変更の理由と、試験の結果を書いてください。
- 小さく分けてください。

## 不具合の報告 / Issues

- 「Bug report」の欄に、機種・OS・キットの版・Fix-PocketGPU の結果を貼ってください。
