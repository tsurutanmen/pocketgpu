# Windows で iPhone へ続けて流すと切れる件の調べ（2026-10-10・同日に直した）

印：**[事実]** は出典やこの PC で読んだ値で確かめたこと。**[要約経由]** は調べ物の担当が要約器経由で読んだこと。**[二次]** は掲示板やブログ。**[推測]** は私の推論。
この PC の値は、読むだけで集めた（2026-10-10 の 14〜16 時台）。設定は何も変えていない。

> **直した点（2026-10-10）**：最初の版で「直近 2 日の Windows のイベントに USB の切断は無い」と書いたのは誤りだった。私は System と Kernel-PnP/Configuration の帳簿しか見ていなかった。iPhone RPC のセッションの指摘で **Kernel-PnP/Device Management** の帳簿を見たところ、iPhone がバスの上から消えた記録が多数あった（1 章）。結論と切り分けの順番を、それに合わせて書き直した。

## 0. 結論

- [事実] iPhone は **USB のバスの上から一瞬消えている**。Windows の帳簿に「surprise removed as it is reported as missing on the bus」が残っている（1 章）。切れているのは usbmuxd より下、USB の線か電源の層。
- [事実] 消えるのは iPhone だけ。iPhone が消えた前後 3 秒に、ほかの USB の機器が消えた記録は 99 回のうち 1 回だけ。→ コントローラーや PC 全体が落ちているのではなく、iPhone の 1 本のつながりが落ちている。
- [事実] この PC では、**別の iPhone**（8 月、番号の頭が `00008101`）も同じように消えていた。今の iPhone 17 Pro（`00008150`）は 10/5 から毎日消えている。
- [推測] 2 台の別の iPhone で起きているので、iPhone の個体だけの問題より、**PC の側の共通の要因**（口・コントローラー・省電力の設定）のほうが疑わしい。ただし 8 月の記録には、人が抜き差しした回が混じっているかもしれない。区別はできない。
- [推測] 「前の日の夕方から」ではなく、10/5 に今の iPhone をつないだときから、1 日 5〜10 回の割合で起きていた。10/10 は 28 回で、試験の時間（11:49〜12:40）に 21 回が集中している。長く流す試験を始めて、目立つようになった可能性がある。

## 1. iPhone がバスから消えた記録（読むだけで確認）

- 帳簿：`Microsoft-Windows-Kernel-PnP/Device Management`、Id 1010
- 本文の例：`Device USB\VID_05AC&PID_12A8\00008150000268220EC0401C has been surprise removed as it is reported as missing on the bus. Count of devices removed: 3`
- 日ごとの回数（帳簿は 2026-03-27 から残っている）

| 日 | 回数 | iPhone の番号 |
|---|---|---|
| 08/22 | 17 | 00008101000425C63E78001E（別の iPhone） |
| 08/23 | 9 | 同上 |
| 08/24 | 6 | 同上 |
| 10/05 | 5 | 00008150000268220EC0401C（今の iPhone 17 Pro） |
| 10/06 | 6 | 同上 |
| 10/07 | 10 | 同上 |
| 10/08 | 9 | 同上 |
| 10/09 | 9 | 同上 |
| 10/10 | 28（16 時台まで） | 同上 |

- iPhone RPC のセッションの確認：試験で切れた 3 回（10/10 の 13:25:53・13:36:10・14:15:53）と、帳簿の時刻がぴったり合う。試験をしていない夜中（10/10 の 0:28〜0:52）にも 5 回ある。
- 読み方（PowerShell・読むだけ）：
  ```
  Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-Kernel-PnP/Device Management'; Id = 1010 } | Where-Object { $_.Message -match 'VID_05AC' } | Select-Object TimeCreated
  ```

## 2. この PC の今の値（読むだけで確認）

| 項目 | 値 |
|---|---|
| Apple Mobile Device Support | 20.0.0.35（2026-10-05 に入った） |
| iTunes | 12.13.11.1（2026-10-05 に入った） |
| Microsoft Store の Apple のアプリ | Apple Music（1.1540.23042.0）だけ。「Apple デバイス」は入っていない |
| Apple の USB ドライバ | 538.0.0.0（2023-06-14） |
| 電源プラン | バランス |
| USB の選択的サスペンド | **有効**（AC も DC も 1） |
| iPhone のつながり方 | ハブを挟まず、Intel の USB 3.20 のコントローラーのルートハブに直接。場所は `Port_#0024.Hub_#0002`、経路は `ACPI(RHUB)#ACPI(SS08)` |
| 「電源をオフにできる」の印 | ルートハブ 2 つは **オン**。iPhone の機器（VID_05AC PID_12A8 MI_00）はオフ |

- [推測] 経路の `SS08` は、Intel の ACPI の名前の付け方では SuperSpeed の口。この時刻には USB 3 でつながっている。
- [推測] 毎秒 30〜45 MB は USB 2 の実効の上限に近いが、つながりは USB 3。速さの頭打ちと、消える件は、別の話かもしれない。

## 3. 切り分けの手順（順番を直した）

設定を変えるのは鶴田さん。**1 度に 1 つだけ変える**。変える前に、今の値を控える（2 章の表）。

順番の考え方（[推測]）：
- 消えるのは USB の層なので、usbmuxd・Apple のサービス・Store のアプリは後ろに下げた。
- 2 台の iPhone で起きているので、PC の側で、安くて戻しやすい物（省電力の設定）を先にした。
- ただし、流している最中の機器は「休んでいない」ので、選択的サスペンドは本来かからないはず。夜中の 5 回（試験をしていない時間）とは合うかもしれない。効くかどうかは、試すまで分からない。

### 3-0. 毎回そろえて記録する物（何も変えない）

- 1 つの条件で 40 分以上、3 回ほど流す。切れる時間がばらつくので、1 回では決めない。
- 毎回控える物：流し始めた時刻・切れた時刻・**1 章の帳簿の Id 1010 の時刻**・iPhone を再起動してからの時間・切れたあと Windows の機器として残っていたか。
- iPhone の syslog の、Id 1010 の時刻の前後 10 秒：`LDCM`・`locationd adapter details`（充電の状態）・`usbmuxd`・`lockdownd`・`IOAccessoryManager`。LDCM との突き合わせは `ldcm.md` の 5 章。
- iPhone の熱の状態：受け手が `VR_GET_STATUS` で返す熱（0〜3）を、切れる直前の値として控える。
- 夜中のように流していない時間の Id 1010 も、何もしなくても数えられる。条件を変えた日と変えない日で、1 日の回数を比べられる。

### 3-1. USB の選択的サスペンドを、試験の間だけ切る

- 今の値の読み方（読むだけ）：
  ```
  powercfg /query SCHEME_CURRENT 2a737441-1930-4402-8d77-b2bebba308a3 48e6b7a6-50f5-4782-a5d4-53bb8f07e226
  ```
  今は AC も DC も `0x00000001`（有効）。
- 変える（管理者の PowerShell で）：
  ```
  powercfg /setacvalueindex SCHEME_CURRENT 2a737441-1930-4402-8d77-b2bebba308a3 48e6b7a6-50f5-4782-a5d4-53bb8f07e226 0
  powercfg /setactive SCHEME_CURRENT
  ```
- 元に戻す：最後の数を `1` にして、同じ 2 行を流す。
- 分かること：試験中と夜中の Id 1010 が減れば、選択的サスペンドが候補。Microsoft は切らないことを強く勧めているので（[事実・要約経由] https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-selective-suspend ）、試験が終わったら戻す。

### 3-2. ルートハブの「電源をオフにできる」の印を外す（3-1 とは別の回に）

- 今の値：ルートハブ 2 つとも印あり（2 章）。
- 変える物：デバイスマネージャー →「表示」→「デバイス (接続別)」で、iPhone の上の「USB ルート ハブ (USB 3.0)」を開く →「電源の管理」→「電力の節約のために、コンピューターでこのデバイスの電源をオフにできるようにする」の印を外す。
- 元に戻す：印を戻す。
- 分かること：3-1 で変わらず、ここで減れば、ハブ単位の省電力が候補（[要約経由] Microsoft の KB974476 の手順。https://support.microsoft.com/help/974476 ）。

### 3-3. 別の口に挿す

- 今の値：Intel のコントローラーの SuperSpeed の口（`SS08`）に直結。
- 変える物：同じ PC の、別の USB 3 の口（ハブを挟まない物）。この PC には ASMedia の USB ハブ（VID_174C）もつながっているので、それは避ける。
- 分かること：別の口で減れば、今の口が候補。2 台の iPhone で起きているので、口の候補は上げてよい（[推測]。8 月にどの口を使っていたかは分からない）。
- 元に戻す：元の口に挿し直す。

### 3-4. ケーブルを替える

- 変える物：USB 3 に対応した、別のデータケーブル。
- 分かること：替えて減れば、ケーブルが候補。
- [推測] 8 月の iPhone（番号の頭が 8101）は Lightning の機種だと思われ、ケーブルも違ったはず。ケーブルは 2 台に共通の要因ではないので、順番を下げた。番号から機種を読むのは推測。
- 元に戻す：元のケーブルに戻す。

### 3-5. iPhone の差し込み口と、iPhone の側の設定

- 差し込み口を明かりで見て、ゴミがあれば木か樹脂の細い物で取る（`ldcm.md`）。
- 有線アクセサリを「常に許可」にする（設定 → プライバシーとセキュリティ → セキュリティ → 有線アクセサリ）。戻すときは既定の「ロック解除時に自動的に許可」へ（[事実・要約経由] https://support.apple.com/111806 ）。
- 低電力モードと、最適化されたバッテリー充電を切る。電池 100% で充電中なので、充電の止め方とつなぎ直しが関わるかを見る（[推測]）。3-0 の syslog で、Id 1010 の直前に充電の状態の行が変わっていないかも見る。

### 3-6. 後ろに下げた物（USB の層より上なので、消える件には効きにくいと見る）

- Apple Mobile Device Service を開始し直す・ドライバ（usbaapl64.inf）を入れ直す・Store の Apple Music を外す。[推測] どれも USB のバスの上から機器が消えることの原因にはなりにくい。消える件が片づいたあとも usbmuxd の層で切れるなら、そのとき試す。
  - 出典：[要約経由] https://learn.microsoft.com/en-us/answers/questions/5683422/after-update-to-win-11-pro-my-pc-no-longer-can-det ／ [二次] https://discussions.apple.com/thread/255975056

## 4. 外の報告

- [要約経由] 特定の版の Apple Mobile Device Service と、大量に流すと切れることを結んだ報告は見つからなかった。
- [要約経由] libimobiledevice/usbmuxd の issue #285（Linux・iOS 27.0・約 37 分後に切れる・open）。https://github.com/libimobiledevice/usbmuxd/issues/285
- [二次] Framework の掲示板：iPhone 15 Pro で、ケーブルを替えると通った報告と、「電力が足りない」と言って切れる報告。https://community.frame.work/t/usb-c-is-not-compatible-with-iphone-15-pro-pro-max/38349
- [要約経由] Microsoft「Fix USB-C problems in Windows」：ハブやドックを通さず直接つなぐ。https://support.microsoft.com/windows/fix-usb-c-problems-in-windows-f4e0e529-74f5-cdae-3194-43743f30eed2

## 5. 見つからなかったこと

1. iPhone 17 Pro・iOS 26・Windows・USB のバスから消える、の組み合わせの報告
2. USB PD の取り決めのやり直しがデータを切る、という一次の資料
3. 8 月の iPhone をどの口とケーブルでつないでいたか（この PC の帳簿には残っていない）
4. 8 月と 10 月の記録のうち、人が抜き差しした回がどれか

調べ物のメモの全文は作業用の場所の `notes_usbmux.md` にある。
