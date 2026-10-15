PocketGPU tester kit for Linux (draft)
======================================

Thank you for testing. Please do not share this kit with anyone else.
Each kit is marked for one person.
The Linux version is new. It has been tested only with a stand-in for the iPhone, not with a real iPhone yet.
Please tell us what works and what does not.

Have an Android phone instead of an iPhone? Skip the iPhone parts and read the section "Android phone" below.

What you need
- A 64-bit x86_64 Linux PC: Arch Linux, Ubuntu 22.04 or later, Fedora, or a Steam Deck with SteamOS 3.
  An X11 desktop, or a Wayland desktop with Xwayland. Windows from the iPhone GPU are shown through X11.
- An iPhone with iOS 17 or later and a USB cable. So far it has only been tested on an iPhone 17 Pro.
  USB 3 is several times faster: a USB-C cable that supports USB 3, in a USB-C or USB 3 port on the PC.
  The cable that comes with the iPhone is USB 2. Only Pro models from the iPhone 15 Pro on support USB 3.
- usbmuxd and the libimobiledevice tools. Linux needs them to talk to the iPhone over USB.
- The Vulkan loader, libvulkan.so.1. Most desktops already have it.
- A tool to install the iPhone app with your own Apple ID. Impactor and iloader both work on Linux.

Install the packages
- Arch Linux, CachyOS:         sudo pacman -S --needed usbmuxd libimobiledevice vulkan-icd-loader
- Ubuntu, Linux Mint, Debian:  sudo apt install usbmuxd libimobiledevice-utils libvulkan1
- Fedora:                      sudo dnf install usbmuxd libimobiledevice-utils vulkan-loader
- Bazzite and other rpm-ostree systems: rpm-ostree install usbmuxd libimobiledevice-utils, then restart.
- SteamOS on a Steam Deck: the system is read-only. In Desktop Mode, open Konsole and run:
      passwd                                      only if you have not set a password yet
      sudo steamos-readonly disable
      sudo pacman-key --init
      sudo pacman-key --populate archlinux holo
      sudo pacman -S --needed usbmuxd libimobiledevice
      sudo steamos-readonly enable
  A SteamOS update removes these packages. Run the commands again after each update.

Steps
1. Extract the kit into your home folder:
       tar xzf PocketGPU-linux-*.tar.gz -C ~
   Then open a terminal in the new PocketGPU-linux-... folder. Keep the folder in your home folder.
   Some other places do not allow programs to run.
2. Plug the iPhone into the PC with USB and unlock it. When it asks "Trust This Computer?", tap Trust.
   Then run:   idevicepair pair
   It should say SUCCESS. If it asks you to accept on the iPhone, do that and run it again.
3. Install the iPhone app, RpcServer-*.ipa from this folder, with your Apple ID. Two free tools work on Linux:
   - Impactor: on Flathub as dev.khcrysalis.PlumeImpactor. On a Steam Deck, install it from Discover.
     Or download the AppImage from https://github.com/khcrysalis/Impactor/releases
   - iloader: AppImage, .deb and .rpm at https://github.com/nab138/iloader/releases, and iloader-bin in the AUR.
   Open the tool, sign in with your Apple ID, choose RpcServer-*.ipa and install it.
   With a free Apple ID the app stops opening after 7 days. Install it again to continue.
   Both tools can also install SideStore, which can renew the app on the iPhone without the PC.
   AltServer-Linux has not been updated since 2022, so we do not recommend it.
   The first time, on the iPhone, open Settings > General > VPN & Device Management and trust your Apple ID.
   If iOS asks for Developer Mode, turn it on in Settings > Privacy & Security > Developer Mode and restart the iPhone.
4. Open the app on the iPhone and keep it in front with the screen on.
5. Run:   ./start-pocketgpu.sh
   It checks each part in order and tells you what to do when something is missing.
6. To run a program on the iPhone GPU:
       ./run-on-iphone-gpu.sh vkcube
       ./run-on-iphone-gpu.sh /path/to/program arguments
   Steam games, native and Proton: put this line in the game's Properties > Launch Options:
       "/home/you/PocketGPU-linux-.../run-on-iphone-gpu.sh" %command%
   start-pocketgpu.sh shows this line with the real path.
   Proton already contains DXVK and vkd3d-proton, so you do not copy any files next to the game.
   vkcube is a small test program in the vulkan-tools package of most distributions.
7. To use the iPhone as a microphone and a webcam, with or without the GPU, run:
       ./start-mic-camera.sh
   In the iPhone app, turn on "Use this iPhone as a webcam" and "Use as PC speaker and mic".
   Add --speaker to also play the PC sound on the iPhone. Run ./start-mic-camera.sh --help for more.

Microphone and webcam
- The microphone shows up as "PocketGPU iPhone Mic". Pick it as the microphone in your call app.
  It needs PipeWire, which most desktops use now, or PulseAudio with the pactl command.
- With --speaker, an output called "PocketGPU iPhone Speaker" is added. Pick it to play sound on the iPhone.
- The webcam needs FFmpeg's libavcodec to show the picture:
      Ubuntu, Debian: sudo apt install libavcodec-extra
      Fedora:         sudo dnf install libavcodec-free
      Arch, SteamOS:  sudo pacman -S --needed ffmpeg
- So that Zoom, Discord, browsers and others can pick the webcam like a normal camera, v4l2loopback is needed.
  This needs administrator rights once:
      sudo apt install v4l2loopback-dkms      (Ubuntu, Debian. Fedora: RPM Fusion. Arch: v4l2loopback-dkms)
      sudo modprobe v4l2loopback exclusive_caps=1 card_label="PocketGPU iPhone Camera"
  Without it, the webcam is a PipeWire camera. Only apps that support PipeWire cameras can pick it,
  such as OBS (Video Capture Device, PipeWire). SteamOS cannot use v4l2loopback.
- The microphone and webcam on Linux have not been tested with a real iPhone yet.

Android phone
- This kit also works with an Android phone instead of an iPhone. It is an experiment.
  The Android scripts have been tested here only with a stand-in for adb, not on a real phone from Linux yet.
  Please tell us what works and what does not.
- You do not need usbmuxd, libimobiledevice, or the iPhone app. You need:
  - A 64-bit ARM Android phone, Android 11 or later, with a Vulkan GPU, and a USB cable that carries data.
  - adb, the Android command line tool:
        Arch, CachyOS:   sudo pacman -S android-tools
        Ubuntu, Linux Mint, Debian:  sudo apt install adb
        Fedora:          sudo dnf install android-tools
    Without a package, download SDK Platform-Tools for Linux from
    https://developer.android.com/tools/releases/platform-tools, unzip it, and put the platform-tools folder inside this kit folder.
  - The Vulkan loader, libvulkan.so.1, as above.
- Steps for the GPU. Extract the kit as in step 1 above, then:
  1. On the phone, turn on USB debugging. Open Settings > About phone and tap Build number 7 times.
     Then open Settings > System > Developer options and turn on USB debugging.
  2. Plug the phone into the PC with USB. When the phone asks "Allow USB debugging?", tap Allow.
     If more than one phone or emulator is connected, choose one with its serial from "adb devices":
         ./start-android.sh --serial SERIAL        or        export ANDROID_SERIAL=SERIAL
  3. Run:   ./start-android.sh
     It copies the receiver, android/server_aarch64, to /data/local/tmp on the phone and starts it. Nothing is installed on the phone and root is not needed.
     Then it connects port 50300 on the PC to the receiver with adb forward.
  4. Run:   ./check-android.sh
     It checks each part in order. At the end you should see a GPU named "PocketGPU phone".
  5. To run a program on the phone GPU:
         ./run-on-android-gpu.sh vkcube
         ./run-on-android-gpu.sh /path/to/program arguments
     Steam games, native and Proton: put this line in the game's Properties > Launch Options:
         "/home/you/PocketGPU-linux-.../run-on-android-gpu.sh" %command%
     It sets the same things as run-on-iphone-gpu.sh, with DXVK_FILTER_DEVICE_NAME=PocketGPU. It does not need the USB helper.
     Everything in "Steam and Proton" below applies to it, except --broker.
     Flatpak Steam needs only the flatpak override line from that section, not --broker.
  6. While you use it, keep the phone unlocked with the screen on.
  7. When you are finished, run:   ./stop-android.sh
     It stops the receiver and removes it from the phone. Its log stays at /data/local/tmp/pgpu_server.log on the phone.
- Another port: if port 50300 is used by another program, run PGPU_PORT=50301 ./start-android.sh, and put PGPU_PORT=50301 in front of the other scripts too.
- Steps for the microphone and webcam, with or without the GPU:
  1. Run:   ./start-android-mic-camera.sh
  2. If the PocketGPU app is not on the phone yet, the script offers to install android/pocketgpu.apk with adb install -r.
     If the phone asks about Google Play Protect, tap Install anyway.
  3. The script opens the app on the phone, turns on the webcam and the speaker and mic, and connects the ports with adb forward.
     When the phone asks for camera and microphone access, tap Allow.
  4. Pick "PocketGPU Android Mic" as the microphone in your call app. With --speaker, pick "PocketGPU Android Speaker" as the output.
     The camera is shown on v4l2loopback or on a PipeWire camera named "PocketGPU Android Camera".
  5. Press q in the terminal to stop. The script closes the ports and turns the app features off. With --keep-app-on it leaves them on.
  - FFmpeg, PipeWire, PulseAudio and v4l2loopback are the same as in "Microphone and webcam" above.
  - Keep the phone unlocked and do not close the PocketGPU app while you use it.
  - The ports. App 0.1.2 and later wait on an abstract socket that only adb can reach, so other apps on the phone cannot read your camera or microphone.
    The script runs these two commands, and then webcam_rx --addr 127.0.0.1:50064 and ipaudio --connect 127.0.0.1:50063:
        adb forward tcp:50064 localabstract:pocketgpu-webcam
        adb forward tcp:50063 localabstract:pocketgpu-audio
    App 0.1.1 uses the old form, adb forward tcp:50064 tcp:50064 and adb forward tcp:50063 tcp:50063. The script reads the app version and picks the right form.
    If it cannot read the version, it tries the new form first and then the old one. You do not need to do anything.
  - The receiver of this kit and the app can run together. If the receiver of this kit is running on the phone at 127.0.0.1:50300, the app does not start its own.
    ./start-android.sh --app uses the receiver inside the app instead, so nothing is copied to the phone.
    With app 0.1.4 or later it connects port 50300 to the USB socket of the app (adb forward tcp:50300 localabstract:pocketgpu-gpu). The app waits there for USB,
    and on TCP it uses port 50060, or 50061 and up if 50060 is taken. With app 0.1.3 or earlier it connects to tcp:50060 as before.
- If something fails:
  - adb says "unauthorized": unlock the phone and tap Allow on "Allow USB debugging?". Unplug and plug the cable in again if it does not appear.
  - adb says "no permissions": plug the cable in again and run adb kill-server. If it stays, install the udev rules:
    sudo pacman -S android-udev on Arch and CachyOS, or sudo apt install android-sdk-platform-tools-common on Ubuntu and Debian.
  - No phone is found: try another cable or USB port. Set the USB mode of the phone to File transfer. Check step 1.
  - The picture stops or the connection drops: run ./stop-android.sh and then ./start-android.sh again.
  - To send the logs: run ./send-android-log.sh and send the file it makes, a zip, or a tar.gz when the zip command is not installed (no user name, PC name, phone serial number or phone number).
  - To measure the USB speed: run ./test-android-speed.sh and paste the text it makes on Discord (DRAFT wording: no user name, PC name, serial number or phone number). It takes about 30 seconds and installs nothing on the phone.
- Reporting for Android: send pocketgpu-android-start-log.txt, pocketgpu-android-check-log.txt, or pocketgpu-android-miccam-log.txt, whichever you ran.
  They have no user name, PC name, home folder, or phone serial. Please also tell us the phone model, the Android version, and the chip, for example Pixel 10 Pro XL, Android 16, Tensor G5.
  If the receiver did not start, also send the output of:   adb shell cat /data/local/tmp/pgpu_server.log

Steam and Proton
- The launch option line sets these for the game only: the iPhone driver as the only Vulkan driver,
  DXVK_FILTER_DEVICE_NAME=iPhone, and a small USB helper. The helper lets the game reach usbmuxd from inside
  Steam's container, where /run/usbmuxd cannot be seen. It does not copy any data, and it stops with the game.
- To also see the PC's own GPU, add IPHONE_VK_ONLY=0 in front:
      IPHONE_VK_ONLY=0 "/home/you/PocketGPU-linux-.../run-on-iphone-gpu.sh" %command%
- For a log, add PROTON_LOG=1 IPHONE_VK_LOG=1 in front. Proton writes it to ~/steam-<appid>.log.
- Steam from Flatpak, com.valvesoftware.Steam, has not been tested. It needs two more things:
  1. Once:   flatpak override --user --filesystem=/path/to/this/folder:ro com.valvesoftware.Steam
  2. Every time, before you start the game, run this in a terminal and keep it open:
         ./run-on-iphone-gpu.sh --broker
  If you can, use Steam from your distribution instead of Flatpak.
- Steam from Snap has not been tested. Games in it can most likely not reach the iPhone over USB.
  Use the .deb from Valve or the Steam package of your distribution.
- Steam Deck: try it in Desktop Mode first. Game Mode has not been tested yet.
- Each start gets its own USB helper with a new secret, given only to that game. The helper gives the connection
  only to programs of the same user that know the secret. With --broker, the secret is kept in tools/broker-token,
  which only you can read, and it is deleted when you stop --broker.

Good to know
- Only programs started this way see the iPhone GPU. Nothing is installed and nothing is written outside this folder.
- Only one program at a time can use the iPhone GPU.
- Do not use games with anti-cheat such as Easy Anti-Cheat, BattlEye, VAC or Vanguard. Your account may be banned.
- 32-bit games use lib/i386. This has not been tested yet.
- Not supported yet on Linux: programs that use Wayland only, captions, and the PocketGPU PC app.
- The window picture is sent as JPEG. To try H.264, which sends less data, put IPHONE_VK_VIDEO=1 in front.
  It needs FFmpeg's libavcodec on the PC. If it is missing, or the game runs in Steam's container without it,
  the picture stays JPEG.
- Kernel 6.7 or later uses less CPU. Older kernels work but are slower.

Reporting
- In the iPhone app, tap "Send logs" and send the text to us on Discord or by email.
- On the PC, send pocketgpu-start-log.txt, which start-pocketgpu.sh writes. It has no user name, PC name or home folder.
- To see what the driver does:   IPHONE_VK_LOG=1 ./run-on-iphone-gpu.sh program 2> driver-log.txt
- Please also tell us the distribution, the desktop, the program or game, the resolution, and what happened.

Third-party software
- lib/x86_64/libiphone_icd.so and lib/i386/libiphone_icd.so contain LZ4, bcdec and stb_image. See THIRD-PARTY.txt.
- FFmpeg, PipeWire and PulseAudio are not included. The programs use the ones installed on the PC.

--------------------------------------------
PocketGPU 試験用キット Linux 版。この説明は下書きです

試していただき、ありがとうございます。このキットは、他の人に渡さないでください。1 人ずつ識別用の印が含まれています。
Linux 版は新しく作ったものです。iPhone の代わりのプログラムでテストしただけで、実際の iPhone ではまだテストしていません。
動作したこと、動作しなかったことを教えてください。

iPhone ではなく Android のスマートフォンを使う場合は、iPhone の説明は読まずに、下の「Android のスマートフォン」を読んでください。

必要なもの
- 64 ビットの x86_64 の Linux の PC。Arch Linux、Ubuntu 22.04 以降、Fedora、または SteamOS 3 の Steam Deck
  X11 のデスクトップ、または Xwayland が使える Wayland のデスクトップ。iPhone の GPU のウィンドウは X11 で表示します
- iOS 17 以降の iPhone と USB ケーブル。今のところ、テストしたのは iPhone 17 Pro だけです
  USB 3 を使うと、転送速度が数倍になります。USB 3 に対応した USB-C ケーブルを、PC の USB-C ポートまたは USB 3 ポートに接続してください。
  iPhone に付属のケーブルは USB 2 です。USB 3 に対応しているのは、iPhone 15 Pro 以降の Pro モデルだけです
- usbmuxd と libimobiledevice のツール。Linux が USB で iPhone と通信するために必要です
- Vulkan のローダー libvulkan.so.1。多くのデスクトップには、すでにインストールされています
- 自分の Apple ID で iPhone にアプリをインストールするためのツール。Impactor と iloader が Linux で使えます

パッケージのインストール
- Arch Linux・CachyOS：         sudo pacman -S --needed usbmuxd libimobiledevice vulkan-icd-loader
- Ubuntu・Linux Mint・Debian：  sudo apt install usbmuxd libimobiledevice-utils libvulkan1
- Fedora：                      sudo dnf install usbmuxd libimobiledevice-utils vulkan-loader
- Bazzite など rpm-ostree のシステム：rpm-ostree install usbmuxd libimobiledevice-utils を実行してから、再起動してください
- Steam Deck の SteamOS：システムが読み取り専用です。デスクトップモードで Konsole を開いて、次を実行してください。
      passwd                                      パスワードをまだ設定していない場合だけ
      sudo steamos-readonly disable
      sudo pacman-key --init
      sudo pacman-key --populate archlinux holo
      sudo pacman -S --needed usbmuxd libimobiledevice
      sudo steamos-readonly enable
  SteamOS を更新すると、これらのパッケージは削除されます。更新のたびに、もう一度実行してください

手順
1. キットをホームフォルダに展開してください：
       tar xzf PocketGPU-linux-*.tar.gz -C ~
   展開された PocketGPU-linux-… フォルダでターミナルを開いてください。フォルダはホームフォルダに置いてください。
   ほかの場所では、プログラムの実行が禁止されていることがあります
2. iPhone を USB で PC に接続して、ロックを解除してください。「このコンピュータを信頼しますか？」と表示されたら「信頼」をタップしてください。
   次に、idevicepair pair を実行してください。SUCCESS と表示されれば完了です。
   iPhone で許可するように表示された場合は、iPhone で許可してから、もう一度実行してください
3. このフォルダの RpcServer-*.ipa を、自分の Apple ID で iPhone にインストールしてください。Linux では、次の 2 つの無料のツールが使えます。
   - Impactor：Flathub に dev.khcrysalis.PlumeImpactor という名前であります。Steam Deck では Discover からインストールできます。
     AppImage は https://github.com/khcrysalis/Impactor/releases からダウンロードできます
   - iloader：AppImage・.deb・.rpm が https://github.com/nab138/iloader/releases にあります。AUR には iloader-bin があります
   ツールを開いて Apple ID でサインインし、RpcServer-*.ipa を選んでインストールしてください。
   無料の Apple ID の場合は、7 日でアプリが起動できなくなります。その場合は、インストールし直してください。
   どちらのツールも SideStore をインストールできます。SideStore を使うと、PC が無くても iPhone でアプリを更新できます。
   AltServer-Linux は 2022 年から更新されていないため、おすすめしません。
   初めてのときは、iPhone の「設定」>「一般」>「VPN とデバイス管理」で、自分の Apple ID を信頼してください。
   デベロッパモードを求められた場合は、「設定」>「プライバシーとセキュリティ」>「デベロッパモード」をオンにして、iPhone を再起動してください
4. iPhone でアプリを起動して、前面に表示したまま画面をオンにしておいてください
5. ./start-pocketgpu.sh を実行してください。必要な項目を順にチェックして、足りない物があれば対処方法を表示します
6. プログラムを iPhone の GPU で動かすには、次のように実行してください：
       ./run-on-iphone-gpu.sh vkcube
       ./run-on-iphone-gpu.sh /プログラムの場所/プログラム 引数
   Steam のゲームは、Linux 版も Proton も、ゲームのプロパティの「起動オプション」に次の 1 行を入力してください：
       "/home/ユーザー名/PocketGPU-linux-…/run-on-iphone-gpu.sh" %command%
   start-pocketgpu.sh は、実際のパスに置き換えたこの 1 行を表示します。
   Proton には DXVK と vkd3d-proton が含まれているので、ゲームのフォルダにファイルをコピーする必要はありません。
   vkcube は、多くのディストリビューションの vulkan-tools パッケージに含まれている、小さなテスト用のプログラムです
7. iPhone をマイクとウェブカメラとして使うには、次を実行してください。GPU を使っていても、使っていなくても使えます。
       ./start-mic-camera.sh
   iPhone のアプリで「ウェブカメラとして使う」と「PC のスピーカーとマイクとして使う」をオンにしてください。
   PC の音も iPhone から出すには --speaker を付けてください。ほかの設定は ./start-mic-camera.sh --help で確認できます

マイクとウェブカメラ
- マイクは「PocketGPU iPhone Mic」という名前で表示されます。通話アプリのマイクで選んでください。
  PipeWire（今の多くのデスクトップ）か、pactl コマンドが使える PulseAudio が必要です
- --speaker を付けると「PocketGPU iPhone Speaker」という出力が追加されます。音を iPhone から出すには、これを選んでください
- ウェブカメラの映像を表示するには、FFmpeg の libavcodec が必要です：
      Ubuntu・Debian：sudo apt install libavcodec-extra
      Fedora：        sudo dnf install libavcodec-free
      Arch・SteamOS： sudo pacman -S --needed ffmpeg
- Zoom・Discord・ブラウザなどで、ふつうのカメラとして選べるようにするには v4l2loopback が必要です。
  最初に 1 回だけ、管理者権限で次を実行してください：
      sudo apt install v4l2loopback-dkms      （Ubuntu・Debian。Fedora は RPM Fusion・Arch は v4l2loopback-dkms）
      sudo modprobe v4l2loopback exclusive_caps=1 card_label="PocketGPU iPhone Camera"
  v4l2loopback が無いときは PipeWire のカメラになります。選べるのは、OBS（PipeWire のビデオキャプチャデバイス）など、
  PipeWire のカメラに対応したアプリだけです。SteamOS では v4l2loopback を使えません
- Linux のマイクとウェブカメラは、まだ実際の iPhone ではテストしていません

Android のスマートフォン
- このキットは、iPhone の代わりに Android のスマートフォンでも使えます。実験段階です。
  Android 用の台本は、adb の代わりのプログラムでテストしただけで、Linux から実際のスマートフォンではまだテストしていません。
  動作したこと、動作しなかったことを教えてください。
- usbmuxd、libimobiledevice、iPhone のアプリは必要ありません。必要なものは次のとおりです。
  - 64 ビット ARM の Android のスマートフォン。Android 11 以上で、Vulkan に対応した GPU を搭載しているもの。データを転送できる USB ケーブル
  - adb（Android のコマンドラインツール）：
        Arch・CachyOS：   sudo pacman -S android-tools
        Ubuntu・Linux Mint・Debian：  sudo apt install adb
        Fedora：          sudo dnf install android-tools
    パッケージを使わない場合は、https://developer.android.com/tools/releases/platform-tools から SDK Platform-Tools for Linux をダウンロードして展開し、
    platform-tools フォルダをこのキットのフォルダの中に置いてください。
  - Vulkan のローダー libvulkan.so.1。上の説明のとおりです
- GPU を使う手順。上の手順 1 のとおりにキットを展開してから、次のように進めてください。
  1. スマートフォンで USB デバッグをオンにします。「設定」>「デバイス情報」を開いて、「ビルド番号」を 7 回タップします。
     次に「設定」>「システム」>「開発者向けオプション」を開いて、「USB デバッグ」をオンにします。
  2. スマートフォンを USB で PC に接続します。スマートフォンに「USB デバッグを許可しますか？」と表示されたら「許可」をタップしてください。
     スマートフォンやエミュレーターが 2 台以上つながっている場合は、adb devices のシリアルで 1 台を選んでください：
         ./start-android.sh --serial シリアル        または        export ANDROID_SERIAL=シリアル
  3. 次を実行してください：   ./start-android.sh
     受け手の android/server_aarch64 をスマートフォンの /data/local/tmp にコピーして、起動します。スマートフォンには何もインストールせず、root も必要ありません。
     そのあと adb forward で、この PC の 50300 番を受け手につなぎます。
  4. 次を実行してください：   ./check-android.sh
     必要な項目を順にチェックします。最後に「PocketGPU phone」という名前の GPU が表示されれば成功です
  5. プログラムをスマートフォンの GPU で動かすには、次のように実行してください：
         ./run-on-android-gpu.sh vkcube
         ./run-on-android-gpu.sh /プログラムの場所 引数
     Steam のゲーム（ネイティブも Proton も）は、ゲームのプロパティの「起動オプション」に次の 1 行を入力してください：
         "/home/ユーザー名/PocketGPU-linux-…/run-on-android-gpu.sh" %command%
     run-on-iphone-gpu.sh と同じ設定をして、DXVK_FILTER_DEVICE_NAME=PocketGPU を使います。USB の補助プログラムは必要ありません。
     下の「Steam と Proton」の説明は、--broker を除いて、これにも当てはまります。Flatpak 版の Steam に必要なのは、そこにある flatpak override の行だけです。--broker は要りません
  6. 使用中は、スマートフォンのロックを解除したまま、画面をオンにしておいてください
  7. 終わったら、次を実行してください：   ./stop-android.sh
     受け手を停止して、スマートフォンから削除します。受け手の記録は、スマートフォンの /data/local/tmp/pgpu_server.log に残ります
- 別の番号：50300 番をほかのプログラムが使っているときは、PGPU_PORT=50301 ./start-android.sh を実行してください。ほかの台本の先頭にも PGPU_PORT=50301 を付けてください
- マイクとウェブカメラの手順。GPU を使っていても、使っていなくても使えます。
  1. 次を実行してください：   ./start-android-mic-camera.sh
  2. スマートフォンに PocketGPU のアプリが入っていない場合は、android/pocketgpu.apk を adb install -r で入れるかどうかを聞かれます。
     スマートフォンに Google Play プロテクトの確認が表示されたら、「インストールする」をタップしてください。
  3. 台本がスマートフォンのアプリを開き、ウェブカメラとスピーカー・マイクをオンにして、adb forward で口をつなぎます。
     スマートフォンにカメラとマイクの許可を求められたら、「許可」をタップしてください
  4. 通話アプリのマイクで「PocketGPU Android Mic」を選んでください。--speaker を付けたときは、出力で「PocketGPU Android Speaker」を選んでください。
     カメラは、v4l2loopback か、「PocketGPU Android Camera」という名前の PipeWire のカメラに表示されます
  5. 止めるには、ターミナルで q キーを押します。台本は口を閉じて、アプリの機能をオフにします。--keep-app-on を付けると、オンのままにします
  - FFmpeg・PipeWire・PulseAudio・v4l2loopback は、上の「マイクとウェブカメラ」と同じです
  - 使用中は、スマートフォンのロックを解除したままにして、PocketGPU のアプリを閉じないでください
  - 口について。アプリ 0.1.2 以降は、adb からしか届かない抽象名のソケットで待ちます。スマートフォンのほかのアプリは、カメラとマイクの内容を読めません。
    台本は次の 2 つのコマンドを実行してから、webcam_rx --addr 127.0.0.1:50064 と ipaudio --connect 127.0.0.1:50063 を起動します：
        adb forward tcp:50064 localabstract:pocketgpu-webcam
        adb forward tcp:50063 localabstract:pocketgpu-audio
    アプリ 0.1.1 は古い形の adb forward tcp:50064 tcp:50064 と adb forward tcp:50063 tcp:50063 を使います。台本はアプリの版を読んで、合う形を選びます。
    版が読めないときは、新しい形を先に試して、だめなら古い形にします。操作は必要ありません
  - このキットの受け手とアプリは、同時に動かせます。スマートフォンの 127.0.0.1:50300 でこのキットの受け手が動いているときは、アプリは自分の受け手を起動しません。
    ./start-android.sh --app を使うと、アプリの中の受け手を使うので、スマートフォンにファイルをコピーしません
    アプリ 0.1.4 以降では、この PC の 50300 番をアプリの USB 用の口につなぎます（adb forward tcp:50300 localabstract:pocketgpu-gpu）。アプリは USB ではそこで待ち、
    TCP では 50060 番を使い、使えなければ 50061 番から順に試します。アプリ 0.1.3 以前では、これまでどおり tcp:50060 につなぎます
- うまくいかないとき：
  - adb が「unauthorized」と表示する：スマートフォンのロックを解除して、「USB デバッグを許可しますか？」で「許可」をタップしてください。表示されないときは、ケーブルを抜いて接続し直してください
  - adb が「no permissions」と表示する：ケーブルを接続し直して、adb kill-server を実行してください。直らないときは、udev のルールを入れてください：
    Arch・CachyOS は sudo pacman -S android-udev、Ubuntu・Debian は sudo apt install android-sdk-platform-tools-common
  - スマートフォンが見つからない：別のケーブルや USB ポートを試してください。スマートフォンの USB の用途を「ファイル転送」にして、手順 1 をチェックしてください
  - 映像が止まる、または接続が切れる：./stop-android.sh を実行してから、もう一度 ./start-android.sh を実行してください
  - 記録を送るとき：./send-android-log.sh を実行して、作成されるファイルを送ってください。zip コマンドが入っていない PC では、zip の代わりに tar.gz になります。ユーザー名・PC 名・スマートフォンのシリアル番号・電話番号は含まれません
  - USB の速さを測るとき：./test-android-speed.sh を実行して、作成される txt の中身を Discord に貼ってください【仮文】ユーザー名・PC 名・シリアル番号・電話番号は含まれません。約 30 秒かかります。スマートフォンには何もインストールしません
- Android の報告：実行した台本に合わせて、pocketgpu-android-start-log.txt、pocketgpu-android-check-log.txt、pocketgpu-android-miccam-log.txt のどれかを送ってください。
  ユーザー名・PC 名・ホームフォルダの場所・スマートフォンのシリアルは含まれません。スマートフォンの機種、Android のバージョン、チップも書いてください。例：Pixel 10 Pro XL、Android 16、Tensor G5
  受け手が起動しなかったときは、次の出力も送ってください：   adb shell cat /data/local/tmp/pgpu_server.log

Steam と Proton
- 起動オプションの 1 行は、そのゲームだけに次の設定をします：iPhone のドライバだけを Vulkan のドライバにする、
  DXVK_FILTER_DEVICE_NAME=iPhone、USB の補助プログラムを起動する。
  補助プログラムは、/run/usbmuxd が見えない Steam のコンテナの中から、ゲームが usbmuxd に接続できるようにします。
  データは中継しません。ゲームが終了すると、補助プログラムも終了します
- PC の GPU も表示するには、先頭に IPHONE_VK_ONLY=0 を付けてください：
      IPHONE_VK_ONLY=0 "/home/ユーザー名/PocketGPU-linux-…/run-on-iphone-gpu.sh" %command%
- ログを取るには、先頭に PROTON_LOG=1 IPHONE_VK_LOG=1 を付けてください。Proton が ~/steam-<appid>.log に書き込みます
- Flatpak 版の Steam、com.valvesoftware.Steam はまだテストしていません。次の 2 つが必要です。
  1. 最初に 1 回だけ：flatpak override --user --filesystem=/このフォルダの場所:ro com.valvesoftware.Steam
  2. ゲームを起動する前に毎回、ターミナルで次を実行して、開いたままにしてください：
         ./run-on-iphone-gpu.sh --broker
  できれば、Flatpak 版ではなく、ディストリビューションの Steam を使ってください
- Snap 版の Steam はまだテストしていません。Snap 版では、ゲームから iPhone の USB に接続できない可能性が高いです。
  Valve の .deb か、ディストリビューションのパッケージの Steam を使ってください
- Steam Deck：まずデスクトップモードで試してください。ゲームモードはまだテストしていません
- USB の補助プログラムは、起動のたびに新しい合言葉で起動し、合言葉はそのゲームにだけ渡します。
  補助プログラムは、同じユーザーで合言葉を知っているプログラムにだけ接続を渡します。
  --broker のときは、合言葉を自分だけが読める tools/broker-token に保存し、--broker を終了すると削除します

知っておくこと
- この方法で起動したプログラムだけが、iPhone の GPU を認識できます。何もインストールせず、このフォルダの外には何も書き込みません
- iPhone の GPU を一度に使用できるプログラムは 1 つだけです
- Easy Anti-Cheat、BattlEye、VAC、Vanguard などのアンチチートを導入しているゲームでは使用しないでください。アカウントが停止されることがあります
- 32 ビットのゲームは lib/i386 のドライバを使います。まだテストしていません
- Linux 版では、まだ次のものは使えません：Wayland だけで動作するプログラム、字幕、PocketGPU の PC アプリ
- ウィンドウの映像は JPEG で送ります。データ量の少ない H.264 を試すには、先頭に IPHONE_VK_VIDEO=1 を付けてください。
  PC に FFmpeg の libavcodec が必要です。無いときや、それが無い Steam のコンテナの中でゲームが動くときは、JPEG のままです
- カーネル 6.7 以降では CPU の負荷が下がります。古いカーネルでも動作しますが、遅くなります

報告
- iPhone のアプリの「ログを送信」をタップして、そのテキストを Discord かメールで送ってください
- PC では、start-pocketgpu.sh が作成する pocketgpu-start-log.txt を送ってください。ユーザー名・PC 名・ホームフォルダの場所は含まれません
- ドライバの動作を見るには：IPHONE_VK_LOG=1 ./run-on-iphone-gpu.sh プログラム 2> driver-log.txt
- ディストリビューション、デスクトップ、プログラムやゲームの名前、解像度、何が起きたかも書いてください

他の人が作ったソフトウェア
- lib/x86_64/libiphone_icd.so と lib/i386/libiphone_icd.so には、LZ4・bcdec・stb_image が含まれています。THIRD-PARTY.txt を参照してください
- FFmpeg・PipeWire・PulseAudio は含まれていません。PC にインストールされている物を使います
