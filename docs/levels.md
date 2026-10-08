# iPhone を GPU に：Lv 1〜100

目標は「iPhone を USB でつなぐと、Windows の PC が GPU を 1 枚増やしたように使える」。
比べる相手は RTX ではなく、GPU の無い PC やノート（CPU と内蔵 GPU）。そこで目に見えて得をすることを目指す。
2026-10-08 夕方の見立ては **Lv 12**。Lv 10 の残りだった DXVK 3 の流体も iPhone で直り、Lv 10〜30 のうち DirectX 12 と OpenGL が iPhone の実機で動いた。

## 段階と、クリアの条件

### Lv 1〜10 試作（いまここ）
- [x] Windows の Vulkan のドライバとして iPhone の GPU が見える（vulkaninfo）
- [x] 改造なしのアプリが動く：llama.cpp・whisper.cpp・stable-diffusion.cpp・vkcube
- [x] Vulkan 1.3 と名乗れる
- [x] 窓に絵を出せる（JPEG で運ぶ・先送り）
- [x] DirectX 11 が DXVK を通して iPhone で描ける（DXVK-macOS と DXVK 3.1.1）
- [x] iPhone の Apple の機能を Windows から呼べる（文字認識・音声認識・Apple Intelligence・翻訳・読み上げ）
- [x] テッセレーションの直しを iPhone に入れる：0.7.24 で SimpleBezier11 が 17.2→51.6dB
- [x] geometry shader の真似：点を広げる形（流体の粒）。iPhone の実機でも FluidCS11 が 48.7dB（DXVK-macOS）。DXVK 3 も 48.7dB（10/8：MoltenVK が gl_WorkGroupSize を宣言しないシェーダーを、ドライバで組の大きさの定数を足して直した）
- [x] geometry shader の真似：三角・線を受ける形、番号つき・インスタンスつきの描画、層への描き分け。iPhone のふりで試験 6 場面がふつうの DirectX と同じ。間接の描画と transform feedback はまだ・iPhone の実機はこれから

### Lv 10〜30 正しさと広さ
- [ ] Vulkan の公式の適合試験（CTS）の合格率を数字で出す：62,081 件で NotSupported を除いた合格率は RTX 直 99.7%・代役 92.7%・iPhone のふり 94.1%（10/8）。見つかった不具合を直して、synchronization は受け手の落ちが 181→4 件に。iPhone の実機はこれから
- [x] DirectX 11 の機能 29 場面の自前の試験（vk/tests/feat）：DXVK 1.10 で RTX 直と違うのは、任意のボーダー色（iPhone の Metal の限界）と stream output（未対応）だけ。見つかったドライバの不具合 3 つと、運んでいなかった頂点の step rate を直した
- [x] DirectX 12 の下ごしらえ：push descriptor と descriptor の写しをドライバの中で直す（vkd3d-proton はほかに、空の descriptor と大きな descriptor の数が要る）
- [ ] 試すアプリの表を 50 本にして、動く・動かないを毎回測る
- [ ] DirectX 11 のゲームを 10 本
- [x] DirectX 12（vkd3d-proton）：自作の 5 場面が iPhone の実機（0.7.26・11_0）で全部正しい。ベクトルの要素への atomic を配列への atomic にドライバで直した。12_0 には tiled resources が要る
- [x] OpenGL（Zink）：OpenGL 2.1 が iPhone の実機で合格（10/8：Metal の型の名前とぶつかるシェーダーの名前をドライバで外した）。3.0 より上は transform feedback が要る
- [ ] 本物のゲーム（iPhone の実機・10/8）：vkQuake 1280×720 198.8 コマ/秒・GZDoom 232・SuperTuxKart 93.7・Godot の squash_the_creeps 558・truck_town 349・particles 63〜74。読み戻しは CPU が触るまで遅らせる（Godot の粒は代役で 24→163）

### Lv 30〜50 速さ
- [ ] 窓の絵を H.264 か HEVC で送る：iPhone の VideoToolbox で縮めて PC の Media Foundation でほどくところまで動いた（0.7.26・1 コマ 8.9KB で JPEG の 4 分の 1）。ただ全速 92 コマ/秒・遅れ 16.9 ms で JPEG（548 コマ/秒・9.6 ms）より遅いので、既定は JPEG のまま
- [ ] USB の 90MB/s の天井を超える道を探す：Backburner と同じく、インターネット共有を USB で出した有線のネットワークでもつなげるようにした（合言葉つき・0.7.24）。usbmux の往復は中央値 0.86 ms。共有の口と比べるのはこれから
- [x] 窓の絵の返事を次のコマで受け取る（模擬の USB で影のサンプル 105→152 コマ/秒）
- [x] 送る命令の並びを LZ4 で縮める（送る量 3.6 分の 1・152→205 コマ/秒）
- [x] 作る命令の返事を待たない・問い合わせを覚える（読み込みの往復 1958→526 回）
- [x] 窓に出るまでの遅れ：60 コマ/秒のとき 51 ms → 4.6 ms（模擬の USB・代役）。区切りの待ちがあるときは今のコマをその場で受け取り、間に合わなければ先送りに戻す。前のコマの描き終わりを PC から待つのもやめた
- [x] 書き換えに合わせる出し方（FIFO）を守る：これまでは名乗るだけで全速だった。モニターの速さ（この PC は 144）で区切り、iPhone に見えないコマを描かせない
- [ ] 待ち合わせ（往復）をもっと減らす：複数の糸から同時に送る
- [x] 熱の落とし方の仕組み：受け手が iPhone の熱の状態を返し、熱い（2）以上の間はコマ数を 30 に落とす（IPHONE_VK_HEAT_FPS）。代役で熱いを真似て確かめた。0.7.24 から
- [ ] 熱で遅くなるのを iPhone で測る（長く回して、熱の状態とコマ数を並べる）
- [ ] 基準の数字：GPU の無い PC の CPU と内蔵 GPU に対して、LLM の tokens/秒・画像生成の秒・ゲームのコマ/秒で上回る

### Lv 50〜70 普通の人が使える
- [ ] 入れる手順が 1 回で済む（PC の道具と iPhone のアプリ）
- [ ] 抜き差しやアプリの再起動でつなぎ直せる
- [x] 受け手が止まったら分かる：返事が 3 秒無ければ、記録と窓の上の帯で知らせる。受け手が戻れば続きから動く（代役を一時停止して確かめた。帯の文は仮）
- [x] 受け手が落ちたら分かる：切れたと記録と帯で知らせ、この先は失敗を返す。DirectX のアプリは DXVK の中で待ち続けるので、閉じるのは人の手
- [ ] iPhone で後ろに回ったときに、実機で同じになるか確かめる。後ろに回る前に知らせる仕組みは受け手の組み直しが要る
- [ ] 7 日ごとの署名し直しを無くす（有料の開発者登録か App Store）
- [x] 細工した命令で受け手が落ちない：点検で見つけた穴（大きさを信じてヒープを壊す・巨大な確保・文字列の終わり・掛け算のあふれ）をふさぎ、揺さぶり 12,739 件で落ちない。記録の中の命令まで深く揺さぶるのはこれから
- [x] ハンドルの種類を受け手が確かめる：番号の種類を型から確かめ、描き方の無い draw・dispatch は飛ばす（0.7.25）

### Lv 70〜90 広がり
- [ ] iPhone を何台もつなぐ
- [ ] Android
- [ ] 古いスマホを計算機にする
- [ ] 公開して、手伝ってくれる人が直せる

### Lv 90〜100 ただの GPU
- [ ] 差すだけで、Windows のアプリやゲームが 2 枚目の GPU として普通に使い、目に見えて得をする

## 越えられるか分からない壁
- USB の太さ：今は 90MB/s。毎コマ大きいデータを往復する使い方は根本的に苦手
- Apple の決まり：後ろに回ると止められる・熱で遅くなる・配るには有料の登録が要る
- MoltenVK に無い物：geometry shader・cull distance・一部の robustness2 など。ドライバか受け手で真似る

## 記録
- 測った数字は `project` のメモと `vkbuild/iphone_runs/` に残す。主な物：
  - 2026-10-07 iPhone 17 Pro：vkcube 1280×720 244.8 コマ/秒・DirectX 11 の立方体 260 コマ/秒・DirectX のサンプル 6 本がふつうの DirectX と見た目で区別できない
  - 2026-10-07 夜 iPhone 17 Pro：vkQuake 1280×720 149.6 コマ/秒・DirectX のサンプル 13 本が動く・機能の 29 場面で iPhone だけ違うのは頂点の形式 A2B10G10R10_UINT（vfmt）
  - 2026-10-08 iPhone 17 Pro（0.7.25・0.7.26）：DirectX 12 と OpenGL が動いた・SuperTuxKart 93.7 コマ/秒・vkQuake 198.8・DXVK 3 の流体と vfmt が DXVK-macOS と同じ絵に（どちらも MoltenVK の穴をドライバで避けた）
