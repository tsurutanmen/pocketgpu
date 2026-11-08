# LDCM の調べ（2026-10-10）

印：**[事実]** は出典で確かめたこと。**[要約経由]** は調べ物の担当が要約器経由で読んだこと（原文の語句は未確認）。**[推測]** は私の推論。

## 0. 結論を先に

- [事実・要約経由] LDCM は Liquid Detection and Corrosion Mitigation（液体の検知と腐食の軽減）と読んでよい。Apple の特許の題名がこの言葉で、macOS の `liquiddetectiond` の説明文も同じ言葉。
- [推測] つないでいる間に 32 秒ごとに測るのは、ふつうの見回りの可能性が高い。Apple の特許には「機器を使っているかどうかに関わらず、定期的に測れる」とある。ただし 32 秒という数の出典は見つからなかった。
- [事実・2026-10-10 に追記] Windows の帳簿では、iPhone は USB のバスの上から**一瞬**消えて、すぐ戻っている（`usbmux_drops.md` の 1 章）。最初の版では「Windows の機器としては残る」を前提に書いていたので、ここを直した。
- [推測] LDCM が「濡れている」と判断したなら、Apple の記事のとおり「充電とアクセサリの接続」が使えなくなり、警告が出て、乾くまで戻らないはず。一瞬消えてすぐ戻る今の形とは合わない。だから LDCM が「濡れ」と判断して切っている可能性は低い。
- [推測] 残る可能性は、「測るたびに一瞬、線を切り替える」こと。一瞬消える形とは合う。特許は「回路を切り離せる」とも「通信を妨げずに測れる」とも書いていて、iPhone 17 Pro の実際の動きは分からない。確かめるには、切れた時刻と LDCM の行の時刻を比べるのがいちばん早い（5 章）。

## 1. LDCM が何をするか

- [事実] Apple のサポート記事：液体の警告は「iPhone XS、iPhone XS Max、iPhone XR 以降」で出る。警告が出ると「充電とアクセサリの接続は、差し込み口とケーブルの端とアクセサリが乾くまで使えない」。緊急のときは、警告を無視して充電する選択肢がある。USB のデータやパソコンとの接続についての記述は無い。
  - 出典：https://support.apple.com/en-us/102643 （私が原文を確認）
- [要約経由] Apple の特許 US 11,658,443「Liquid detection and corrosion mitigation」：差し込み口に、相手側に対応する接点が無い「液体検知用の接点」を置き、電圧をかけて流れる電流を測る。液体が VBUS や接地とつなぐと電流が流れる。
  - 出典：https://patents.google.com/patent/US11658443B2/en
- [要約経由] Apple の別の特許 US 10,591,430：時間で変わる電圧や電流をかけて複素インピーダンスを測り、液体を判定する。
  - 出典：https://patents.google.com/patent/US10591430B2/en
- [推測] 「ピンの間のインピーダンスを測る」という理解は、特許の方式と大筋で合う。iPhone 17 Pro がどの方式かは分からない。

## 2. 「Reason: 2」の意味

- 見つからなかった。iPhone でも macOS でも、Reason の番号の一覧は見つからなかった。
- [推測] 測り始めたきっかけの種類（定期・電源を入れた後・つないだ時・落とした後など）の番号かもしれない。特許の本文に、これらのきっかけが並んで書かれているため（[要約経由]）。番号との対応は確かめていない。
- [推測] 32 秒ごとに同じ番号が出ているなら、「定期」の番号である可能性が高い。

## 3. 32 秒ごとはふつうか

- [要約経由] 特許 US 11,658,443 には「Liquid-detect measurements can occur periodically whether or not the device is being used.」「…following a power-up of the device.」とある。周期の数は書かれていない。
- [事実] macOS の解説記事（Eclectic Light）のログでは、LDCM はケーブルをつないだときに動いていた。`liquiddetectiond LDCM - Handling LDCM interrupt event for port 2` という行があり、割り込みで測り始める形にも見える。周期の記述は無い。
  - ログの例：`IOAccessoryManager IOPortFeatureLDCMUserClient::_copyData(): Copying LDCM data... (target: Port-USB-C@2/LDCM)`。iPhone の行（`Port-USB-C@1/LDCM`）と同じ形。
  - 出典：https://eclecticlight.co/2025/03/13/usb-ports-on-apple-silicon-macs-accessory-security-and-liquid-detection/ （私が原文を確認）
- 見つからなかった：液体やゴミを疑うと間隔が短くなる、という記述。

## 4. 測るときに USB の線を止めるか

- [要約経由] 特許 US 11,658,443 には「コネクタの接点につながる回路を切り離せる」「過電圧のときは、検知用の接点を検知回路から切り離せる」とある。液体を検出したら CC の接点をプルダウン抵抗から切り離す、という読みも要約にあったが、要約どうしで食い違うので確定しない。
- [要約経由] 特許 US 10,591,430 は「通常の通信を妨げたり止めたりせずに、液体の試験ができる」ことを目標にしている。
- [要約経由] 別の出願 US 12,640,514（Liquid and connection detection）には、検知中に VBUS を内部の電源から切り離す、または接地につなぐ、と書かれているという要約があった。本文は取れなかった（404）。
  - 出典：https://patents.google.com/patent/US12640514B2 （検索結果の要約のみ）
- 見つからなかった：LDCM の測定で USB のデータ転送が切れた、という報告。
- [推測] 測るたびに線を切り替えるなら、32 秒ごとに切れるはず。実際は「数十秒から 30 分」でばらつく。毎回の測定で切れているわけではなさそう。測定のどれかが、まれに何かを引き起こしている可能性は残る。

## 5. 確かめる方法（iPhone にも Windows の設定にも触らずにできる順）

1. **時刻を突き合わせる**：切れた時刻と、iPhone の syslog の `Beginning LDCM measurement` の時刻を並べる。切れる直前 1〜2 秒に LDCM の行が毎回あれば、関係は強い。無ければ LDCM は外してよい。10 回ぶんほど集めると判断しやすい。[推測]
   - **切れた時刻は、Windows の帳簿の時刻を使う**（2026-10-10 に追記）。帳簿 `Microsoft-Windows-Kernel-PnP/Device Management` の Id 1010（`... has been surprise removed as it is reported as missing on the bus`）が、iPhone が USB のバスの上から消えた時刻。ドライバと受け手の記録より正確で、試験をしていない時間の分も残る。読み方（PowerShell・読むだけ）：
     ```
     Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-Kernel-PnP/Device Management'; Id = 1010 } | Where-Object { $_.Message -match 'VID_05AC' } | Select-Object TimeCreated
     ```
   - [事実] この帳簿では、iPhone は 10/5 から毎日 5〜10 回、10/10 は 28 回消えている。くわしくは `usbmux_drops.md` の 1 章。
   - PC と iPhone の時計のずれに気をつける。1 秒単位で比べるので、先に両方の時刻を合わせて控えておく。[推測]
   - [推測] 消えるのは USB の層なので、LDCM が「測るたびに一瞬、線を切り替える」なら、Id 1010 の時刻の直前に LDCM の行があるはず。
2. **20 秒ごとの充電の行も同じく突き合わせる**：`locationd adapter details ... "connected":1, "wasConnected":0`。値が毎回同じなら、つなぎ直しではなく、状態を定期的に書いているだけの可能性が高い。切れる直前に `connected` が 0 になる行があれば、電源側でつなぎ直しが起きている。[推測] この行の意味は、出典が見つからなかった。
3. **LDCM の中身を見る**：macOS のログの例では、LDCM の記録に Wet・Wet State Duration・Mitigations Status などの欄がある（[要約経由] https://eclecticlight.co/2025/03/16/last-week-on-my-mac-the-myth-of-liquid-detection/ ）。iPhone の syslog にも同じ欄が出ていれば、Wet が 0 のままかを見る。
4. **警告が出ていないか**：切れたとき、iPhone の画面に液体の警告が出ていないか。
5. **差し込み口を見る**（鶴田さんの作業）：明かりで差し込み口の中を見て、ゴミがあれば木か樹脂の細い物で取る。圧縮空気は奥に押し込むので避ける（[二次] コミュニティの回答）。Apple の記事は、外からの熱・圧縮空気・異物・米で乾かすことを避けるよう書いている（[事実]）。

## 6. 見つからなかったこと

1. Reason の番号の一覧
2. 32 秒という周期の出典と、間隔が短くなる条件
3. LDCM の測定で USB のデータや VBUS を止めるという一次情報と、それで切れたという報告
4. IOAccessoryManager・IOPortFeatureLDCMUserClient・Port-USB-C@1 の iOS 側の解説（macOS のログの例だけ）
5. LDCM を止める設定（Apple の文書には無い）
6. `batteryChargerType`・`kChargerTypeUsb`・`connected`・`wasConnected` の定義
7. 特許 US 11,658,443 の請求項の本文

調べ物のメモの全文は作業用の場所の `notes_ldcm.md` にある。
