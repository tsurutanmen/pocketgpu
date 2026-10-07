# DirectX 11 で試す本物のソフトの候補（2026-10-07 調べ・まだ取ってきていない）

条件：無料・合法に取れる・不正対策（チート対策）が無い・ネットにつながなくてよい・同じ場面を毎回流して速さを測れる・なるべく 2GB 以下。
無料でオフラインで不正対策の無い DirectX 11 のゲームはほとんど無く、ベンチマークのソフトが中心になる。

| 順 | 名前 | 大きさ | 決まり | 測り方 | メモ |
|---|---|---|---|---|---|
| 1 | Unigine Heaven 4.0（benchmark.unigine.com/heaven） | 247 MB | 無料版は個人利用だけ | 決まったカメラの道筋。F12 で絵を保存 | テッセレーションを使う → 受け手 0.7.24 から |
| 2 | Unigine Valley 1.0（benchmark.unigine.com/valley） | 342 MB | 同上 | 同上 | |
| 3 | Unigine Superposition | 1.3 GB | 同上 | `-video_app direct3d11` で DirectX 11 | |
| 4 | FFXIV 黄金のレガシー ベンチマーク | 4.0 GB | 無料・アカウント不要 | 約 6 分の決まった場面 | 大きい |
| 5 | Dear ImGui のデモ（example_win32_directx11） | 2.8 MB | MIT | 速さは測らない。動くかだけ | |

コマンドで回す引数（Phoronix の設定から・無料版で効くかは版による）：
`Heaven.exe -video_app direct3d11 -data_path ../ -sound_app null -engine_config ../data/heaven_4.0.cfg -system_script heaven/unigine.cpp -video_mode -1 -extern_define PHORONIX,RELEASE`

注意：Unigine の無料版は個人利用に限る。会社の名前で使うと商用の扱いになるおそれがあるので、使う前に決まりを読む。

外した物：World of Tanks enCore（公式の入手先が無い）・Neon Noir（4.35 GB）・Street Fighter 6 のベンチ（DirectX 12）・Diligent Engine（組んだ物の配布が無い）。

調べは Sonnet の助手。大きさと決まりは助手が公式の頁で読んだ物で、確かめていない所は元の報告に「未確認」とある。
