# PocketGPU の Windows アプリ（pcapp4）：pcapp3 に、PC から iPhone のカメラを操作する画面・温度・電池・アップデートを足した物（10/12）

`D:\iphone-rpc\work\pcapp3\` を写して、ここで作業した。pcapp3 は書き換えていない。git・本物の iPhone・RTX 3060 は使っていない。
ウィンドウは出していない（--offscreen）。CEO のキットの PocketGPU と webcam_rx が動いていたので、試験のパイプと共有メモリは別の名前にした。

## 足した物
| ファイル | 中身 |
|---|---|
| `src\CamCtl.cs` | webcam_rx の名前付きパイプ `\\.\pipe\pocketgpu-camctl` につなぐ。1 行 1 つの UTF-8 の JSON。0.5 秒ごとに試し直し（30 回をすぎたら 2 秒ごと）、切れたらつなぎ直す。読み書きは裏の糸 |
| `src\PageCamCtl.cs` | caps の項目だけを、iPhone のアプリと同じまとまりと順と言葉で出す。値の同期・温度の 3 色・熱の知らせ・案内の文 |
| `src\BatteryPoll.cs` | 10 秒ごとに裏の糸で電池の温度と残りを読む。カメラの見出しの横とホームの iPhone の行に出す |
| `src\IphoneBattery.cs` | 別の係の部品（`work\battemp\IphoneBattery.cs`）をそのまま写した物。差し替えるときもそのまま写す。試験の差し替えは BatteryPoll.cs に置いた |
| `src\PageUpdate.cs` ほか | 別の係の差分 `work\updater\pcapp.patch`（設定の「アップデート」）を手で当てた。git は使っていない。足した 239 行が全部入ったことを照らし合わせた |
| `test\fakectl\` | 偽の webcam_rx（パイプのサーバー・プレビューの絵・`--fake-mux` で機器 0 台の usbmuxd） |

### まとまりの順（iPhone のアプリの CameraControlsView と同じ）
映像（PC 側の設定）・向き 17 18 19・ズームと切り抜き 10 20 21 22 23 24 25・ピント 1 2 3・明るさ 4 6 7 5・色 8 9・ライト 11 12 26 27・手ぶれ補正 13・
背景ぼかしとエフェクト 14 15 ポートレート 103 スタジオ照明 105 16・熱の対策 28・画面 29・その他（表に無い番号。届いた英語の名前）。
- 上の行で決まる行は、iPhone と同じく、要るときだけ出す：レンズの位置はピントが手動・合わせる所は自動・ISO とシャッター速度は明るさが手動・露出補正は手動以外・色温度は色温度のとき・ライトの明るさは点けているとき・位置と大きさは切り抜くとき・横と縦の位置は手動のとき・ぼかしの強さはぼかすとき。背景をぼかしている間はピントを押せない。
- 選択肢は caps の label_ja・label_en。iPhone のアプリと言葉が違う物だけ置き換える（21 の 3：webcam_rx の表は「顔」、iPhone は「顔に合わせる」）。
- range の choices は倍率のボタン（webcam_rx の JSON の形）。ズーム（10）と ISO（6）のつまみは対数。

### 値の同期
- 届いた state で画面を直す。つまみをつかんでいる間と、最後に送ってから 0.7 秒は、その番号の値を動かさない。保留が切れたら最後に届いた値に合わせる。
- つまみは 1 つの番号につき 0.1 秒に 1 回までにまとめて送り、最後の値は必ず送る。選ぶ・スイッチ・ボタンはすぐ送る。
- つないで 2 秒たっても caps が無ければ get_caps と get_state を頼み直す。切れてから 5 秒は前の操作を押せないまま出しておく。

### 温度
- 色は iPhone の 4 段階（event thermal か state 102）：正常は緑・やや高いは黄・高いと危険は赤。危険は太字の赤で「危険」、点が 0.6 秒ごとに点滅する。
- 電池の温度が読めれば横に「36.9℃」。読めなければ度数だけ出さない。4 段階がまだ無いときだけ、℃ で仮の色（40℃ 未満は緑・45℃ 未満は黄・それ以上は赤。PROTOCOL.md の案）。
- 熱でフレームレートを下げたら（event fps_lowered）「iPhone が熱いため、フレームレートを 24 fps に下げています。」。戻ったのは state 108 が上がったことで知る。危険のときは「画面を暗くする」のボタン（29 があるとき）。

### 試験用の環境変数（試す人は使わない）
PGPU_TEST_CTL_PIPE（パイプの名前。webcam_rx に `--ctl-pipe` も渡す）・PGPU_TEST_SHM（プレビューの共有メモリの名前）・PGPU_TEST_BATTERY（電池の仮の値）・
PGPU_TEST_BATTERY_ALWAYS（iPhone が見えなくても部品を呼ぶ）・PGPU_TEST_CTL_NOHOLD と PGPU_TEST_CTL_NOTHROTTLE（対照用）。

## 確かめたこと（`test\run_ctl.ps1`・`test\run_int.ps1`。記録は `test\ctl_*.log`）
| 台本 | 結果 |
|---|---|
| `t_ctl_full.txt` | 29 個を全部出した。つまみを 1 秒動かすと 10 回送った・跳ね 0。偽の iPhone は 0.15 秒遅れて返すが、保留が切れたあと画面と届いた値が同じ |
| `t_ctl_control.txt` | 対照：保留を外すと跳ね 8 回、まとめ送りを外すと 1 秒に 60 回。測り方が違いを拾えることを確かめた |
| `t_ctl_limited.txt` | 15 個だけの caps で、出た行が caps と同じ 15 個。caps が 17・29・31 に変わると 31 は「その他」。本物の電池の部品＋偽の usbmuxd で「iPhone が USB で認識されていません」→ 度数なし・色だけ |
| `t_ctl_old.txt` | ctrl が false で操作の欄を出さず「iPhone のアプリを 0.7.62 以降にすると、PC から操作できます。」。4 段階が来る前は 46.0℃ で仮の赤 |
| `t_ctl_nopipe.txt` | パイプが無いと「操作の準備をしています。」、映像が流れて 10 秒で「カメラの受信プログラムが古いため…」 |
| `t_ctl_heat.txt` | やや高い・高い（24 fps）・危険（15 fps・点滅・画面を暗くする→29=1）・state 108 で戻す・切れてつなぎ直す（2 回目）・エラーの文 |
| `t_ctl_int.txt`（run_int） | 本物の webcam_rx（camctl2・17:46 の組）と偽の iPhone：caps 28 個、20・21・9・4・11 を送って同じ値が state で戻った。ただし PC が重い時間で、最初の status まで 13 秒・caps まで 64 秒かかった。途中でほかの係の試験が同じ口 50528 を使い始め、切れた |
| `t_settings_update.txt` | 設定の「アップデート」の行。キットに update.ps1 が無いので、押すと「このキットは更新に対応していません。…」（何も起動しない） |

---

# PocketGPU の Windows アプリ（pcapp3）：pcapp2 に音の設定を足した物（10/12）

`D:\iphone-rpc\work\pcapp2\` を写して、ここで作業した。pcapp2 は書き換えていない。git・本物の iPhone・RTX 3060 は使っていない。
音の試験は、誰も聞いていない「Steam Streaming Speakers」だけで行った。試験の間は usbmuxd の口を使われていない番号にした（IPHONE_VK_MUX=1）。

## pcapp3 で足した設定
マイクとスピーカーの画面と、設定の画面の「マイクとスピーカー」の両方にある。どちらで変えても同じ値（settings.json）。
| 設定 | キー | ipaudio に渡す物 |
|---|---|---|
| マイクの音を聞く・聞く出力・聞く音量 | audio.monitor・audio.monitorDev・audio.monitorVol | `--mic-monitor 機器名か default`・`--mic-monitor-gain dB`（100% が 0 dB・50% が -6 dB）。マイクを使っているときだけ |
| iPhone から音を出す間は PC の音を消す | audio.spkMutePc | 渡さない。アプリが音を取る出力をミュートする（`src\AudioDevices.cs` の PcMute） |
| スピーカーの遅延 | audio.buf | `--target-ms`。短い 20・標準 40・長い 80。前の「バッファ」の 120 は長いにまとめた |

- ipaudio が `--mic-monitor` に対応しているかは、起動したときに `ipaudio --list --mic-monitor default --mic-monitor-gain 0` で見る。
  対応していれば一覧を出して 0、前の ipaudio は「知らない引数」で 2。対応していなければスイッチを押せない。
  それでも起動した ipaudio に断られたら、対応していないと覚えて、付けずに起動し直す。
- PC の音を消すのは、iPhone とつながっている間だけ。切れてから 5 秒で戻す。停止・閉じる・設定をオフで戻す。
  もともとミュートだった機器には触らない。ミュートした機器の ID を settings.json の audio.mutedDev に書き、落ちても次に開いたときに戻す。
- 聞く出力が音を取る出力と同じときは、注意を出す：PC の音を消す設定ならマイクの音が聞こえない、消さないならハウリングするかもしれない。

## 確かめたこと（10/12）
| 台本 | 結果 |
|---|---|
| `run_loopback_mute.ps1`（`t10_loopback_mute.txt`） | ミュートでは loopback が音を取れる。音量 0 では取れない。下の表 |
| `run_spk_delay.ps1`（`t11_spk_delay.txt`） | 偽の iPhone で 20・40・80 ms。12 秒で途切れ 0。20 ms は ためが最小 6 ms まで減る |
| `run_audio_pages.ps1`（`t12.log`） | 今の ipaudio：対応していないと出て、押せない。ライト・ダーク・英語で撮った |
| `run_audio_live.ps1`（`t13.log`・`t13_stub_args.log`） | 代役 `test\monstub` で、引数・音量・遅延・ミュートと戻し・注意の文。ミュートの間も偽の iPhone に届く音の大きさは同じ |
| `run_audio_fallback.ps1`（`t14.log`） | 断られたら付けずに起動し直して、つながった |
| `run_audio_leftover.ps1`（`t15*.log`） | ミュート中に強制終了 → 次に開いたら戻した。利用者が先にミュートしていた機器は触らず、停止しても戻さない |

ミュートと音量（ipaudio --capture・epvol の既定の取り方・epvol の音量の後の取り方。440 Hz・振幅 0.1）：
| 機器 | 既定の取り方 | 音量の後で取る |
|---|---|---|
| ふつう | 0.100 | 0.100 |
| ミュート | 0.100 | 0 |
| 音量 0 | 0 | 0 |
| ミュートと音量 0 | 0.100 | 0 |

---

# PocketGPU の Windows アプリ（pcapp2）：案 A の並びに、案 B の暗い色とカメラの画面の操作を足した物（10/11）

`D:\iphone-rpc\work\pcapp\`（案 A）を写して、ここで作業した。A と B（`pcapp_b`）のフォルダは書き換えていない。
git には触っていない。本物の iPhone と RTX 3060 は使っていない（試験の台本 `--auto` の間は WPF をソフトで描く）。

## pcapp2 で足したこと

### ライトとダーク
- 色は全部 `src\ui\ThemeLight.xaml`・`src\ui\ThemeDark.xaml` の名前付きの色。2 つは同じ名前を全部そろえる。
  ライトは A の色そのまま。ダークは B（`pcapp_b\src\MainView.xaml`）の色。B は強調の青を使わないので、
  オンのスイッチと主なボタンは B のスイッチと同じ明るい灰色の地に墨の文字。
- `Styles.xaml` とページは色を `{DynamicResource 名前}` で引く。`StaticResource` で色を引くと読み込みで止まる。
  コードで色を塗るときは `SetResourceReference`（`PageCheck.cs`）。
- `Shell.ApplyTheme`：設定の `theme`（auto・light・dark）から決めて、中身の外枠（Root）の Resources の色の辞書を差し替える。
- `Theme.cs`：auto のときは HKCU `Software\Microsoft\Windows\CurrentVersion\Themes\Personalize` の `AppsUseLightTheme` を読む。
  裏の糸が `RegNotifyChangeKeyValue` で変わったのに気づいて、その場で切り替える。10 秒ごとにも読み直す。
  試験用：`PGPU_THEME_KEY` に HKCU の下の別の場所を書くと、そこを読む（本物の設定は書き換えない）。
- タイトルバー：`DWMWA_USE_IMMERSIVE_DARK_MODE` は 20（Microsoft の DWMWINDOWATTRIBUTE の資料）。
  20H1 より前の Windows 10 は 19 なので、20 が失敗したら 19 で頼み直す（Microsoft の Terminal と同じやり方）。
  タイトルバーの地は `DWMWA_CAPTION_COLOR`（35・Windows 11 から）で中身の Bg と同じ色にする。
- 設定の画面に「テーマ」：Windows に合わせる・ライト・ダーク。

### カメラの画面の「音と字幕」
- スピーカー・マイク・字幕のスイッチ。スイッチは「いま実際に動いているか」を見せる。
  スピーカー＝ipaudio が動いていて `audio.spk` がオン。マイク＝ipaudio が動いていて `audio.mic` がオンで流す先が選ばれている。
  字幕＝livecap が動いている。ホームの「マイクとスピーカー」「字幕」、マイクとスピーカーの画面のスイッチ、字幕の画面の開始と停止と同じ値。
- 止まっているところからスピーカーかマイクを入れたときは、押した方だけ使う（もう片方の設定はオフにする）。
  動いている間は、マイクとスピーカーの画面のスイッチと同じ値になる。
- マイクの流す先が選ばれていないときは、オンにならずに案内を出す。

### ついでに直した A の不具合
- 止めたあとに届いた出力の行（livecap の残りの文）で、道具の状態が「動作中」に戻っていた。
  字幕の画面が、止めたのに「iPhone を待っています」と出ていた。`Procs.cs` の MarkRunning・MarkWaiting は Want のときだけにした。

### 試験（`test\`）
| 台本 | 中身 |
|---|---|
| `run_pages.ps1`（`t1_pages.txt`・`t1b_dark_start.txt`） | 全ページをライトで撮り、同じプロセスのままダークに切り替えて撮る。起動からダークの絵とも比べる |
| `run_theme.ps1`（`t8_theme_live.txt`） | 試験用の HKCU の場所で、動いている間の切り替え。終わったらその場所を消す |
| `run_window.ps1`（`t9_window.txt`） | 本物のウィンドウを、どのモニターにも載らない位置に前に出さずに出して、タイトルバーを撮る |
| `run_cam.ps1`（`t2_live.txt`） | 偽の iPhone でカメラ・音・字幕をカメラの画面から入れて、ライトとダークで撮る。ほかの画面のスイッチの値も見る |
| `run_close.ps1`（`t3_close.txt`・`t4_kill.txt`） | 閉じたとき・強制終了したときに子が残らないか |
| `run_capstop.ps1`（`t2c_caption.txt`） | 字幕を止めたあとの表示 |
| `make_compare.ps1` | ライトとダークを並べた比べ用の 1 枚 `test\shots\compare_light_dark.png` |

画面の外のウィンドウは WPF が中身を描かない（PrintWindow の中身は白）。`winshot` は、枠とタイトルバーは PrintWindow の絵のまま、
中身の所だけ WPF に同じ中身を描かせて重ねる。

---

以下は案 A のときの記録（`pcapp`）。

## 組み方と置き方

- `build.cmd`：Windows に入っている csc.exe（.NET Framework 4.8・C# 5）で `out\PocketGPU.exe` を作る。SDK もダウンロードも要らない。
- XAML（`src\ui\*.xaml`）は exe に埋め込む（`ui.名前.xaml`）。exe 1 つで配れて、コードと XAML がずれない。
  組み直さずに XAML だけ直して見るときは `PGPU_XAML_DIR=src\ui`。
- アイコン `src\app.ico` は、初回の build.cmd がアプリ自身に描かせて作る（`--make-icon`）。
- キットの中では `PocketGPU.exe` をキットのフォルダ（`iphone_icd.json`・`dxvk\`・`pocketgpu.cmd` のある所）に置く。
  道具（webcam_rx・camera_setup・ipaudio・livecap・vklist）は exe の横か `tools\`・`extras\`、または横の `tools.ini` で場所を書く（`out\tools.ini` は作業用の例）。

## 中身

| ファイル | 中身 |
|---|---|
| `src\Tools.cs` | 道具の探し方と、引数・環境変数の組み立て。camset の係の変更はここだけ直す |
| `src\Procs.cs` | 子プロセス。ジョブに入れて、アプリが落ちても子を残さない。止めるときは Ctrl+C → 待って → PID で止める。切れたら 3 秒後に起動し直す |
| `src\Mux.cs` | usbmuxd・受け手のあいさつ（HELLO）・熱（VR_GET_STATUS=78）・USB の速さ・lockdownd の ProductType |
| `src\Checks.cs` | start.ps1 の 7 項目を C# に移した物。試験用の差し替えは pgpu_common.ps1 と同じ名前 |
| `src\CamPreview.cs` | 共有メモリ `Local\pocketgpu-webcam-v1` を読んで、NV12 を縮めて描く |
| `src\GameLaunch.cs` | run.ps1 と同じ順の起動（管理者・受け手・DirectX 12/9・DXVK・Steam） |
| `src\Page*.cs`・`src\ui\*.xaml` | 8 つの画面 |
| `src\Strings.cs` | 画面の文（日本語と英語を 1 行に）。文は仮 |
| `src\Auto.cs` | 試験用の台本（`--auto`）。ウィンドウを出さずに操作して PNG に撮る |

決めたこと：
- 設定は `%LOCALAPPDATA%\PocketGPU\settings.json`（試験は `PGPU_DATA_DIR` で `test\data*` に）。
- 言語は Windows の表示言語（.NET の CurrentUICulture）。この PC は ja になる（PowerShell の窓の中では en-US と出るが、それは PowerShell の都合）。設定の画面で選び直せて、その場で切り替わる。
- 閉じると道具は全部止める。GPU の画面から起動したゲームは止めない（ジョブに入れない）。
- 録画は、このアプリから起動したゲームにだけ `IPHONE_VK_RECORD_*` を渡し、ボタンで印のファイル（`recording.flag`）を作る・消す。閉じるときと次に開いたときに印を消す。

## 確かめたこと（`test\`）

| 台本 | 結果 |
|---|---|
| `t1_pages.txt` | 8 画面を撮った（`test\shots\ja_*.png`）。チェックは 1〜3 が問題なし、4 で「iPhone が USB で認識されていません。」と止まり、5〜7 は未チェック（本物の usbmuxd・機器 0 台） |
| `t2b_camera.txt`（`run_cam.ps1`） | 写しの偽の iPhone（`test\fake`・50541・ソフトの符号化）で、プレビューに絵が出た（`ja_camera_live.png`）。共有メモリの書き手は自分の webcam_rx の PID・1920x1080・68 コマ。止めると webcam_rx が結果を書いて終わった（Ctrl+C が届いた） |
| `t2a_audio_cap.txt` | ipaudio を音の代役（50542）につないで「接続中です」、止めた |
| `t2c_caption.txt` | livecap を字幕の代役（50543・`cap_fake.py`）につないで「字幕を表示中です」、止めると livecap が残りの文を確定してから終わった |
| `run_close.ps1` | 音と字幕を動かしたまま閉じる・PocketGPU を強制終了する、の両方で子が残らなかった |
| `t5_window.txt` | 本物のウィンドウをサブのモニター（-1920,0）に出して枠ごと撮った（`win_ja_*.png`）。モックとの並べは `compare_*_mock_vs_app.png` |
| `t6_en.txt`・`t7_record.txt` | 英語への切り替え・マイクとカメラだけ使う設定・録画の印とゲームに渡す環境変数 |

ほかの係（pcapp_b）の webcam_rx が同じ共有メモリを使うので、カメラの試験は相手が居ない時に限った（`run_cam.ps1` が待つ）。

## まだの所

- 本物の iPhone では何も試していない（機種名・USB 3/2 の表示・熱・HELLO・USB の速さ・カメラ・音・字幕）。
- ゲームの起動は試していない（キットのドライバが要る。MessageBox で止まるので台本では避けた）。
- 仮想カメラの登録・解除は押していない（camset の係が同じ HKCU の登録を試しているため。状態を見るだけ確かめた）。
- Wi-Fi のカメラ・マイクの流す先・エコーを抑える・議事録と OBS のファイルは、引数を組むところまで。
- 機種名の表のうち iPhone18,x（17 の世代）は確かめていない。
- 統計：コマ数は「コマ数を記録」（`IPHONE_VK_LOG`＋`IPHONE_VK_FPS_LOG`＋`IPHONE_VK_ERR_FILE` のログを読む）だけ。ログが多くなるので常用には向かない。
  熱は、このアプリから起動したゲームが動いていない時だけ受け手に聞ける（受け手は 1 人しか相手をしない）。
  電池は iphone_battery.py と同じ道（lockdownd のセッションと TLS・診断のサービス）が要り、C# に移していない。
  案：ドライバが `Local\pocketgpu-vk-stats-プロセス番号` の共有メモリにコマ数・熱・送った量を書き、アプリはそれを読む。電池は受け手の VR_GET_STATUS に足す（iOS の UIDevice の値）。どちらも app の変更なので未着手。
- cam4k：カメラの解像度は 720p・1080p・4K（OBS 向け）。4K を選んだときだけ、webcam_rx に環境変数 POCKETGPU_VCAM_MAX=2160 を渡し、仮想カメラが 3840x2160（30・60）も名乗る（共有メモリに 4K の口が付き、レジストリ HKCU\Software\PocketGPU\VirtualCamera の MaxHeight が 2160 になる）。選んでいないときは今までどおり 1080p まで。
  プレビューは本体の共有メモリ（1080p に縮めた絵）を読む。
