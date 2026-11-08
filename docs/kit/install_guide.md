# Installing the iPhone app from Windows with your own free Apple ID (draft, 2026-10-10)
# Windows から自分の無料の Apple ID で iPhone のアプリを入れる手順（下書き・2026-10-10）

Facts and steps only. Wording will be edited by Tsuruta. Each step has a source link.
手順と事実だけです。言い回しは鶴田さんが直します。手順ごとに出典を付けています。

Sources used:
- [S] Sideloadly official site: https://sideloadly.io/
- [A1] AltStore official FAQ, how to install on Windows: https://faq.altstore.io/altstore-world/how-to-install-altstore-windows
- [A2] AltStore official FAQ, troubleshooting: https://faq.altstore.io/altstore-world/troubleshooting-guide

Marks: **[checked]** = stated in the source above. **[unchecked]** = I could not confirm it in an official source; check before you rely on it.
印：**[確認済み]** は出典に書いてあること。**[未確認]** は公式の出典で確かめられなかったこと。頼る前に確かめてください。

---

## English

### 0. Choose one tool / Which tool

You need one of the two. Do not install both at once unless you know why.
- **Sideloadly**: you load the `.ipa` file and sign it with your Apple ID. [S]
- **AltStore**: you install AltStore on the iPhone first, then install the `.ipa` from AltStore. AltServer on the PC re-signs the app. [A1]

### 1. What you need on the PC

1. **iTunes and iCloud from apple.com, not the Microsoft Store.** [checked]
   - Sideloadly: "requires the web version of iTunes & iCloud on Windows". If the Microsoft Store versions are installed, uninstall them first. [S]
   - AltStore: install the latest iTunes and iCloud directly from Apple, not the Microsoft Store. [A1]
   - AltStore has a workaround if you must keep the Store version of iCloud: see "Troubleshooting Guide". It is five steps and needs copying two Apple folders. [A2]
2. A USB cable, and the iPhone unlocked.

### 2. Path A: Sideloadly

The official page lists four steps and gives no detail for each. [S]
1. Download Sideloadly for Windows. [checked]
2. Load the `.ipa`: drag and drop it into Sideloadly. [checked]
3. Enter your free (or paid) Apple ID. [checked]
4. Press the install button (the page calls it "Sideload"). It installs on the device. [checked]

Also stated on the page:
- Free Apple IDs work. Apps signed with a free ID are valid for **7 days**. [checked] [S]
- For Wi-Fi sideloading, first connect by USB and sideload an app once. [checked] [S]
- The page does **not** mention two-factor authentication, how to handle the sign-in code, or the app limit for free IDs. [unchecked]

### 3. Path B: AltStore (Windows)

Steps from the AltStore FAQ. [A1]
1. Download AltServer for Windows and extract `AltInstaller.zip`. [checked]
2. Run `Setup.exe`. [checked]
3. Search the taskbar for "AltServer" and choose "Run as administrator". Allow private network access if asked. [checked]
4. Connect the iPhone, unlock it, and tap Trust on the iPhone if asked. [checked]
5. Open iTunes and sign in with your Apple ID. [checked]
6. In iTunes, turn on **Wi-Fi sync** for the iPhone. [checked]
7. Click the AltServer icon in the taskbar, choose "Install AltStore", and pick the iPhone. [checked]
8. Enter your Apple ID email and password. The FAQ says they are sent only to Apple. [checked]
9. Wait for the AltServer notice that AltStore is installed. You may need to restart the iPhone to see it on the Home Screen. [checked]
10. On the iPhone: Settings > General > Profiles & Device Management, tap Trust for your Apple ID, then confirm. [checked]
11. On iOS 16 or later, turn on **Developer Mode**: Settings > Privacy and Security > Developer Mode. [checked]

Then install the `.ipa` from inside AltStore.
- The FAQ page says you can start sideloading after step 11 but does **not** explain how to pick an `.ipa`. [A1] The exact menu (for example a "+" button in "My Apps") is [unchecked].

### 4. Every 7 days

- **Sideloadly**: sideloaded apps are valid for 7 days. Auto-refresh re-signs them in the background before they expire. [checked] [S] If the app stops opening, load the same `.ipa` in Sideloadly and install it again. (The re-install step itself is the same as step 2 above; the page does not give a separate procedure.)
- **AltStore**: "AltServer must be running on a computer connected to the same WiFi as AltStore in order to sideload or refresh apps." [checked] [A2] If your network blocks devices from finding each other, try a phone hotspot or plug the iPhone in by USB. [checked] [A2]

### 5. App limit with a free Apple ID

- Three apps at a time, and a 7-day signing period, are reported by a third-party guide for the free account (https://xevvy.itch.io/sileo/devlog/508018/ios-detailed-installation-instructions). **[unchecked]** I found no Apple or tool page that states the number of apps. Check it before you say it to testers.

### 6. Common problems

From the AltStore troubleshooting page. [A2]
- **"Could not find AltServer"** while sideloading or refreshing: check that both devices are on the same Wi-Fi, check the Trust prompt on the iPhone, allow AltServer through the Windows firewall, replace Store versions of iTunes and iCloud, or use USB. [checked]
- **"JIT could not be enabled for [App]"**: keep the app open in the foreground, then work through the same network, Trust, firewall and USB checks. (This message is about JIT; PocketGPU does not use it, as far as I know. [unchecked])
- **Sign-in freezes**: check the network, the Trust prompt, the firewall, and whether iTunes and iCloud are running. [checked]
- **Cannot install AltStore**: tap Trust on both devices, try a different Apple ID, keep iTunes and iCloud running, and on Windows run AltServer as administrator. [checked]

### 7. Questions that are open

- Whether a brand-new Apple ID works. A 2022 third-party wiki says a new ID will not work with Sideloadly. **[unchecked]** (https://github.com/qnblackcat/uYouPlus/wiki/Sideloadly-(macOS-&-Windows)/Building)
- Whether Sideloadly asks for a two-factor code. **[unchecked]**
- What happens to an app's data when it is re-signed. **[unchecked]**

---

## 日本語

### 0. どちらの道具にするか

どちらか 1 つで足ります。理由がなければ両方は入れないでください。
- **Sideloadly**：`.ipa` を読み込んで、自分の Apple ID で署名して入れる。[S]
- **AltStore**：先に iPhone に AltStore を入れ、AltStore の中から `.ipa` を入れる。PC の AltServer が署名し直す。[A1]

### 1. PC に要るもの

1. **iTunes と iCloud は apple.com から入れたもの。Microsoft Store 版ではない。** [確認済み]
   - Sideloadly：Windows では「iTunes と iCloud の web 版」が要る。Microsoft Store 版が入っているなら、先に消す。[S]
   - AltStore：iTunes と iCloud の最新版を Apple から直接入れる。Microsoft Store 版ではない。[A1]
   - Store 版の iCloud をどうしても残したい人のために、AltStore には回り道がある。「Troubleshooting Guide」にある 5 手順で、Apple の 2 つのフォルダを写す作業が要る。[A2]
2. USB ケーブルと、ロックを外した iPhone。

### 2. 道 A：Sideloadly

公式のページには 4 手順だけが書いてあり、それぞれの詳しい説明はありません。[S]
1. Windows 版の Sideloadly をダウンロードする。[確認済み]
2. `.ipa` を読み込む：Sideloadly にドラッグ＆ドロップする。[確認済み]
3. 無料（か有料）の Apple ID を入れる。[確認済み]
4. 入れるボタン（ページでは「Sideload」）を押す。iPhone に入る。[確認済み]

ページにはこれも書いてあります。
- 無料の Apple ID でも使える。無料の ID で署名したアプリは **7 日**有効。[確認済み] [S]
- Wi-Fi で入れるには、先に USB でつないでアプリを 1 回入れる。[確認済み] [S]
- 2 段階認証、確認コードの扱い、無料の ID で入れられるアプリの数は、ページに**書かれていません**。[未確認]

### 3. 道 B：AltStore（Windows）

AltStore の FAQ の手順です。[A1]
1. Windows 版の AltServer をダウンロードして、`AltInstaller.zip` を展開する。[確認済み]
2. `Setup.exe` を実行する。[確認済み]
3. タスクバーで「AltServer」を検索し、「管理者として実行」を選ぶ。プライベートネットワークへの許可を聞かれたら許可する。[確認済み]
4. iPhone をつないでロックを外し、聞かれたら iPhone で「信頼」を押す。[確認済み]
5. iTunes を開いて、Apple ID でサインインする。[確認済み]
6. iTunes で、iPhone の **Wi-Fi 同期**をオンにする。[確認済み]
7. タスクバーの AltServer のアイコンを押して「Install AltStore」を選び、iPhone を選ぶ。[確認済み]
8. Apple ID のメールとパスワードを入れる。FAQ は「Apple にだけ送られる」と書いている。[確認済み]
9. AltServer の通知で、AltStore が入ったことを確かめる。ホーム画面に出るには iPhone の再起動が要ることがある。[確認済み]
10. iPhone で 設定 > 一般 > Profiles & Device Management（FAQ の表記のまま）を開き、自分の Apple ID を「信頼」して確定する。[確認済み]
11. iOS 16 以上では **デベロッパモード**をオンにする：設定 > プライバシーとセキュリティ > デベロッパモード。[確認済み]

そのあと、AltStore の中から `.ipa` を入れます。
- FAQ のページは、11 のあとに入れられるようになるとは書いていますが、**`.ipa` をどう選ぶかは書いていません**。[A1] 具体的なメニュー（たとえば「My Apps」の「+」ボタン）は[未確認]です。

### 4. 7 日ごとにやること

- **Sideloadly**：入れたアプリは 7 日有効。期限の前に、自動更新がバックグラウンドで署名し直す。[確認済み] [S] アプリが開かなくなったら、同じ `.ipa` を Sideloadly で読み込んで入れ直す。（入れ直しの手順そのものは上の 2 と同じ。ページに別の手順は書かれていません。）
- **AltStore**：「AltServer は、AltStore と同じ Wi-Fi につながった PC で動いている必要がある」（入れるときも更新するときも）。[確認済み] [A2] ネットワークが機器どうしを見つけさせない場合は、スマホのテザリングを試すか、iPhone を USB でつなぐ。[確認済み] [A2]

### 5. 無料の Apple ID で入れられるアプリの数

- 「同時に 3 本」「署名は 7 日」という数は、第三者の解説（https://xevvy.itch.io/sileo/devlog/508018/ios-detailed-installation-instructions）にだけ書かれています。**[未確認]** 数を書いた Apple や道具の公式ページは見つかりませんでした。試す人に伝える前に確かめてください。

### 6. よくあるつまずき

AltStore の公式のトラブル対処のページから。[A2]
- **「Could not find AltServer」**（入れるとき・更新するとき）：2 台が同じ Wi-Fi にいるか、iPhone の「信頼」の確認、Windows のファイアウォールで AltServer を許可、Store 版の iTunes と iCloud の入れ替え、USB でつなぐ。[確認済み]
- **「JIT could not be enabled for [アプリ名]」**：アプリを前面で開いたままにし、同じ Wi-Fi・信頼・ファイアウォール・USB の確認をする。（JIT の話で、私の知る限り PocketGPU は JIT を使いません。[未確認]）
- **サインインが固まる**：ネットワーク・「信頼」・ファイアウォール、iTunes と iCloud が動いているかを確かめる。[確認済み]
- **AltStore が入らない**：両方の機器で「信頼」を押す・別の Apple ID を試す・iTunes と iCloud を動かしておく・Windows では AltServer を管理者として実行する。[確認済み]

### 7. まだ決まっていないこと

- 作ったばかりの Apple ID で使えるか。2022 年の第三者の wiki は「新しい ID は Sideloadly で使えない」と書いています。**[未確認]**（https://github.com/qnblackcat/uYouPlus/wiki/Sideloadly-(macOS-&-Windows)/Building）
- Sideloadly が 2 段階認証のコードを聞くか。**[未確認]**
- 署名し直したとき、アプリの中のデータがどうなるか。**[未確認]**
