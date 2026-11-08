# 動きそうな iPhone と iPad の表（下書き・2026-10-10）

確かめたのは **iPhone 17 Pro（A19 Pro）だけ**。それ以外の「期待」はすべて推測です。
「確認」の列に ✅ があるものは出典で確かめた事実、💭 は推測か、出典から読み切れなかったものです。

## 先にわかったこと

1. **Metal 3 に対応するのは A14 Bionic 以降**（iPhone 12 以降、A14 を積んだ iPad 以降、M1 以降）。A13 Bionic は「Metal」だけで Metal 3 ではない。✅
   - 出典：Apple の Metal Feature Set Tables（2026年5月21日付）。https://developer.apple.com/metal/capabilities/
   - 注意：依頼文には「Metal 3 に対応か」の列がありましたが、MoltenVK の README は「Metal に対応した機器で動く」としか書いておらず、**Metal 3 が必須だとは書いていません**。✅ ここでは Metal 3 の列を「A14 以降かどうか」の目安として載せています。MoltenVK が実際に要る Metal の機能（argument buffers の段階など）は確認できていません。💭
   - 出典：https://github.com/KhronosGroup/MoltenVK
2. 受け手のアプリが iOS 17 以上を対象にしている、という点は依頼文のまま。この表では確かめていません。
3. USB の速さは「機種ごとに決まる」。**USB 3（10 Gbps）が出るのは Pro 系だけ**で、無印と Air は USB 2（480 Mbps）。PC への描画の戻りが細くなるので、速さの試験は Pro 系を先にするのがよさそうです。💭

## iPhone

| 機種 | チップ | GPU の世代 | メモリ | Metal 3 | USB | 期待（推測） | 確認 |
|---|---|---|---|---|---|---|---|
| iPhone 17 Pro / Pro Max | A19 Pro | Apple10 | 12 GB | 対応 | USB 3（10 Gbps） | 動く（実機で確認済み・17 Pro のみ） | ✅ |
| iPhone Air | A19 Pro | Apple10 | 12 GB（チップ全体の値） | 対応 | **USB 2（480 Mbps）** | 動くはず。USB が細い | ✅ チップと USB／💭 メモリは機種ごとの記載なし |
| iPhone 17 / 17e | A19 | Apple10 | 8 GB（チップ全体の値） | 対応 | 17 は **USB 2**。17e は未確認 | 動くはず。USB が細い | ✅ 17 の USB／💭 17e の USB とメモリ |
| iPhone 16 Pro / Pro Max | A18 Pro | Apple9 | 8 GB | 対応 | USB 3（10 Gbps） | 動くはず | ✅ |
| iPhone 16 / 16 Plus / 16e | A18 | Apple9 | 8 GB（16 の全機種が 8 GB という報道） | 対応 | 16 / 16 Plus は USB 2 とみられる。16e は未確認 | 動くはず。USB が細い | ✅ メモリ／💭 USB |
| iPhone 15 Pro / Pro Max | A17 Pro | Apple9 | 8 GB | 対応 | USB 3（10 Gbps） | 動くはず | ✅ |
| iPhone 15 / 15 Plus | A16 | Apple8 | 6 GB | 対応 | **USB 2（480 Mbps）** | 動く見込み。メモリは 6 GB | ✅ |
| iPhone 14 Pro / Pro Max | A16 | Apple8 | 6 GB | 対応 | Lightning（USB 2 とみられる） | 動く見込み | ✅ チップとメモリ／💭 USB |
| iPhone 14 / 14 Plus | A15 | Apple8 | 6 GB とみられる（A15 は 4〜6 GB とだけ書かれている） | 対応 | Lightning（USB 2 とみられる） | 動く見込み。メモリが少なめ | ✅ チップ／💭 メモリと USB |
| iPhone 13 / 13 mini / SE（第3世代） | A15 | Apple8 | 4 GB とみられる | 対応 | Lightning（USB 2 とみられる） | 動くかは未知。メモリが少ない | ✅ チップ／💭 メモリと USB |
| iPhone 13 Pro / Pro Max | A15 | Apple8 | 6 GB とみられる | 対応 | Lightning（USB 2 とみられる） | 動く見込み | ✅ チップ／💭 メモリと USB |
| iPhone 12 / 12 mini | A14 | Apple7 | 12 は 4 GB。12 mini は未記載 | 対応（最も古い） | Lightning（USB 2 とみられる） | 動くかは未知。いちばん古く、メモリも少ない | ✅ チップと 12 のメモリ／💭 他 |
| iPhone 12 Pro / Pro Max | A14 | Apple7 | 12 Pro は 6 GB。Pro Max は未記載 | 対応（最も古い） | Lightning（USB 2 とみられる） | 動くかは未知 | ✅ チップと 12 Pro のメモリ／💭 他 |
| iPhone 11 以前（A13 以前） | A13 以前 | Apple6 以前 | — | **非対応** | — | 動かない見込み | ✅ |

## iPad

| 機種 | チップ | GPU の世代 | メモリ | Metal 3 | USB | 期待（推測） | 確認 |
|---|---|---|---|---|---|---|---|
| iPad Pro（M5） | M5 | Apple10 | 12 GB（256/512 GB 版）・16 GB（1/2 TB 版） | 対応 | Thunderbolt / USB 4 とみられる | 動くはず。最も有力 | ✅ メモリ／💭 USB（一次の出典なし） |
| iPad Pro（M4） | M4 | Apple9 | 8 GB・16 GB | 対応 | Thunderbolt / USB 4 とみられる | 動くはず | ✅ メモリ／💭 USB |
| iPad Pro（M2） | M2 | Apple8 | 8 GB・16 GB | 対応 | Thunderbolt / USB 4 とみられる | 動く見込み | ✅ メモリ／💭 USB |
| iPad Pro（M1） | M1 | Apple7 | 8 GB・16 GB | 対応 | Thunderbolt 3 / USB 4（40 Gbps） | 動く見込み | ✅ |
| iPad Air（M4） | M4 | Apple9 | 12 GB | 対応 | 5 または 10 Gbps（機種ごとの割り当ては読み切れず） | 動くはず | ✅ メモリ／💭 USB |
| iPad Air（M3 / M2 / M1） | M3 / M2 / M1 | Apple9 / 8 / 7 | 8 GB とみられる | 対応 | 5 または 10 Gbps（同上） | 動く見込み | ✅ チップ／💭 メモリと USB |
| iPad Air（第4世代） | A14 | Apple7 | 4 GB | 対応（最も古い） | 5 または 10 Gbps（同上） | 動くかは未知。メモリが少ない | ✅ |
| iPad mini（A17 Pro） | A17 Pro | Apple9 | 8 GB（チップ全体の値） | 対応 | USB 3.1 Gen 2（10 Gbps） | 動くはず | ✅ |
| iPad mini（第6世代） | A15 | Apple8 | 4 GB | 対応 | 5 Gbps（USB 3.0） | 動くかは未知。メモリが少ない | ✅ |
| iPad（A16・第11世代） | A16 | Apple8 | 6 GB | 対応 | **USB 2.0** | 動く見込み。USB が細い | ✅ |
| iPad（第10世代） | A14 | Apple7 | 4 GB | 対応（最も古い） | **USB 2.0** | 動くかは未知。メモリが少なく USB も細い | ✅ |
| iPad（第9世代以前） | A13 以前 | — | — | **非対応** | — | 動かない見込み | ✅ チップ／💭 世代の対応関係 |

## 「期待」の付け方（私の推測の根拠）

- **高い**：確かめた iPhone 17 Pro と同じ世代のチップ（A17 Pro 以降・M4 以降）で、メモリが 8 GB 以上。
- **中**：A15・A16・M1〜M2 で、メモリが 6 GB 以上。
- **低い・未知**：A14 か、メモリ 4 GB。MoltenVK と DXVK を通した描画は、元のゲームよりメモリを使う可能性があります（これは推測で、測っていません）。
- USB 2 の機種は、**動いても描画の戻りの帯域が足りず遅くなる**可能性があります。測るまでは「遅いかもしれない」までしか言えません。

## 出典

- Metal の対応表（チップ・GPU の世代・Metal 3）：https://developer.apple.com/metal/capabilities/
- MoltenVK の README：https://github.com/KhronosGroup/MoltenVK
- iPhone 17 / Air は USB 2、17 Pro 系は USB 3：https://www.macrumors.com/2025/09/09/iphone-air-usb-2/ と https://wccftech.com/iphone-17-pro-and-iphone-air-usb-speed-limitations/
- チップごとの機種とメモリ：https://en.wikipedia.org/wiki/Apple_A19 、 https://en.wikipedia.org/wiki/Apple_A18 、 https://en.wikipedia.org/wiki/Apple_A17 、 https://en.wikipedia.org/wiki/Apple_A16 、 https://en.wikipedia.org/wiki/Apple_A15 、 https://en.wikipedia.org/wiki/Apple_A14
- iPad のチップ・メモリ・USB：https://en.wikipedia.org/wiki/List_of_iPad_models と https://en.wikipedia.org/wiki/IPad_Pro
- iPhone の Lightning と USB-C の速さ：https://en.wikipedia.org/wiki/List_of_iPhone_models（一覧の読み取りが機種と合わず、機種ごとには使えませんでした。Lightning 機種の USB 2 はこのページの記述からの推測です）

## 読み切れなかったところ（先に確かめたい順）

1. iPhone 13 以前の機種ごとのメモリ（Wikipedia の読み取りで機種と値が合わなかった）
2. iPad Air と iPad Pro の機種ごとの USB の速さ
3. iPhone 16e・17e の USB の種類
4. MoltenVK が実際に要る Metal の機能の下限（これが決まれば A14 の線も確かめられる）
