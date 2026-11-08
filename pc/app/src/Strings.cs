// 画面の文。日本語と英語を 1 行に並べる。XAML からは {DynamicResource s_キー}、コードからは L.T("キー")。
// 文は仮。言い回しはここだけ直せばよい。日本語は「です・ます」、ボタンと項目は体言止め。括弧の補足は書かない。
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows;

namespace PocketGPU
{
    static class L
    {
        static readonly Dictionary<string, string[]> Table = new Dictionary<string, string[]>();
        public static bool Ja = true;

        static void A(string key, string ja, string en) { Table[key] = new[] { ja, en }; }

        public static string T(string key)
        {
            string[] v;
            if (!Table.TryGetValue(key, out v)) return key;
            return Ja ? v[0] : v[1];
        }
        public static string F(string key, params object[] args)
        {
            return string.Format(Ja ? CultureInfo.GetCultureInfo("ja-JP") : CultureInfo.GetCultureInfo("en-US"), T(key), args);
        }

        // 設定の "auto" は Windows の表示言語。PGPU_LANG（キットの台本と同じ名前）があればそれ
        public static bool PickJa(string setting)
        {
            if (setting == "ja") return true;
            if (setting == "en") return false;
            string env = Environment.GetEnvironmentVariable("PGPU_LANG");
            if (!string.IsNullOrEmpty(env)) return env == "ja";
            return CultureInfo.CurrentUICulture.Name.StartsWith("ja", StringComparison.OrdinalIgnoreCase);
        }

        // XAML 用の辞書。言語を変えたら作り直して差し替える（DynamicResource がその場で変わる）
        public static ResourceDictionary MakeDictionary()
        {
            var d = new ResourceDictionary();
            foreach (var kv in Table) d["s_" + kv.Key] = Ja ? kv.Value[0] : kv.Value[1];
            return d;
        }

        static L()
        {
            A("on", "オン", "On");
            A("off", "オフ", "Off");
            A("copy", "コピー", "Copy");
            A("copied", "コピーしました", "Copied");
            A("refresh", "更新", "Refresh");
            A("change", "変更", "Change");
            A("open_folder", "フォルダを開く", "Open folder");
            A("start", "開始", "Start");
            A("stop", "停止", "Stop");
            A("app_title", "PocketGPU", "PocketGPU");

            A("nav_home", "ホーム", "Home");
            A("nav_gpu", "GPU", "GPU");
            A("nav_camera", "カメラ", "Camera");
            A("nav_audio", "マイクとスピーカー", "Microphone and speaker");
            A("nav_caption", "字幕", "Captions");
            A("nav_record", "録画", "Recording");
            A("nav_check", "チェック", "Check");
            A("nav_settings", "設定", "Settings");

            // 左上の iPhone
            A("dev_name", "iPhone", "iPhone");
            A("dev_none", "接続されていません", "Not connected");
            A("dev_nomux", "Apple のサービスが実行されていません", "The Apple service is not running");
            A("dev_usb3", "USB 3 で接続中", "Connected over USB 3");
            A("dev_usb2", "USB 2 で接続中", "Connected over USB 2");
            A("dev_usb", "USB で接続中", "Connected over USB");
            A("dev_wifi", "Wi-Fi で接続中", "Connected over Wi-Fi");

            // ホーム
            A("home_check", "チェック", "Check");
            A("home_features", "機能", "Features");
            A("home_usage", "使い方", "Usage");
            A("home_dev_none_title", "iPhone が接続されていません", "No iPhone is connected");
            A("home_dev_none_sub", "USB ケーブルで接続して、iPhone の PocketGPU アプリを開いてください。", "Connect the iPhone with a USB cable and open the PocketGPU app on it.");
            A("home_dev_nomux_sub", "Apple Devices か iTunes を起動してから、もう一度チェックしてください。", "Open Apple Devices or iTunes, then check again.");
            A("home_dev_ok_sub", "{0}です。iPhone のアプリは前面に表示したままにしてください。", "{0}. Keep the iPhone app in front.");
            A("mode_title", "マイクとカメラだけ使う", "Use only the camera and audio");
            A("mode_sub", "iPhone の GPU は使わずに、カメラ・マイク・スピーカー・字幕だけを使います。", "Use the camera, microphone, speaker and captions without the iPhone GPU.");
            A("home_gpu_idle", "ゲームやアプリを iPhone の GPU で起動します", "Run games and apps on the iPhone GPU");
            A("home_gpu_running", "{0} 個のアプリを iPhone の GPU で実行中です", "{0} app(s) running on the iPhone GPU");
            A("home_cam_off", "iPhone のカメラを PC のカメラとして使います", "Use the iPhone camera as a PC camera");
            A("home_audio_off", "PC の音を iPhone から出したり、iPhone のマイクを PC で使ったりします", "Play PC audio on the iPhone and use the iPhone microphone on the PC");
            A("home_cap_off", "PC の音声を iPhone で文字に起こして、字幕を表示します", "Transcribe PC audio on the iPhone and show captions");
            A("home_rec_off", "iPhone の GPU で起動したゲームを、iPhone で録画します", "Record games running on the iPhone GPU on the iPhone");

            // 道具の状態（共通）
            A("st_off", "停止中です", "Stopped");
            A("st_starting", "起動しています", "Starting");
            A("st_wait", "iPhone を待っています", "Waiting for the iPhone");
            A("st_missing", "{0} が見つかりません。設定の画面で場所を確認してください。", "{0} was not found. See Settings.");
            A("st_failed", "起動できませんでした：{0}", "Could not start: {0}");
            A("st_badargs", "設定の組み合わせが正しくありません。{0}", "The settings do not work together. {0}");

            // GPU
            A("gpu_launch", "アプリを選択して起動", "Choose an app to run");
            A("gpu_state_none", "iPhone が接続されていません", "No iPhone is connected");
            A("gpu_state_name", "iPhone の GPU", "iPhone GPU");
            A("gpu_detail", "選択したアプリだけが iPhone の GPU を使います。使用中は iPhone のアプリを前面に表示したまま、画面をオンにしておいてください。", "Only the app you choose uses the iPhone GPU. Keep the iPhone app in front with the screen on while you use it.");
            A("gpu_detail_mem", "使用可能なメモリ {0:N1} GB", "Memory {0:N1} GB");
            A("gpu_detail_media", "マイクとカメラだけ使う設定になっています。GPU を使うときは、設定の画面でオフにしてください。", "Only the camera and audio are in use. Turn this off in Settings to use the GPU.");
            A("gpu_running", "実行中のアプリ", "Running apps");
            A("gpu_running_none", "このアプリから起動したアプリはありません", "No apps have been started from here");
            A("gpu_running_since", "{0} から実行中", "Running since {0}");
            A("gpu_fps", "{0:N1} fps", "{0:N1} fps");
            A("gpu_recent", "最近起動したアプリ", "Recent apps");
            A("gpu_recent_none", "まだありません", "None yet");
            A("gpu_run", "起動", "Run");
            A("gpu_more", "その他", "More");
            A("gpu_steam", "Steam の起動オプション", "Steam launch options");
            A("gpu_steam_sub", "Steam のゲームは、ゲームのプロパティの「起動オプション」に次の 1 行を入力します。", "For a Steam game, put this line in the game's Properties under Launch Options.");
            A("gpu_heat", "iPhone の熱", "iPhone temperature");
            A("gpu_heat_sub", "熱くなると、ドライバがコマ数を下げます", "When it gets hot, the driver lowers the frame rate");
            A("gpu_heat_busy", "ゲームの実行中は更新できません", "Cannot refresh while a game is running");
            A("heat_0", "ふつう", "Normal");
            A("heat_1", "少し熱い", "Warm");
            A("heat_2", "熱い", "Hot");
            A("heat_3", "とても熱い", "Very hot");
            A("heat_unknown", "不明", "Unknown");
            A("heat_old", "iPhone のアプリが古いため取得できません", "The iPhone app is too old for this");
            A("heat_fail", "iPhone のアプリが応答しません", "The iPhone app does not answer");
            A("gpu_diag", "コマ数を記録", "Log the frame rate");
            A("gpu_diag_sub", "ドライバのログをファイルに残して、実行中のアプリのコマ数を表示します。次に起動したアプリから有効になります。", "Keep the driver log in a file and show the frame rate of running apps. Applies to apps started next.");

            // 起動の手順（Run-on-iPhone-GPU と同じ）
            A("launch_pick_title", "iPhone の GPU で起動するアプリを選択してください", "Choose the program to run on the iPhone GPU");
            A("launch_filter", "アプリ", "Programs");
            A("launch_url", "インターネットショートカットの .url は使用できません。Steam のゲームは、GPU の画面の「Steam の起動オプション」を使ってください。", "Internet shortcuts .url cannot be used. For a Steam game, use the Steam launch option on the GPU page.");
            A("launch_lnk_bad", "このショートカットのリンク先が分かりません。.exe を直接選択してください。", "Cannot find where this shortcut points. Choose the .exe itself.");
            A("launch_notfound", "見つかりません：{0}", "Not found: {0}");
            A("launch_nodriver", "iPhone の GPU のドライバが見つかりません。PocketGPU.exe はキットのフォルダに置いてください。\n\n探した場所：{0}", "The iPhone GPU driver was not found. Keep PocketGPU.exe in the kit folder.\n\nLooked in: {0}");
            A("launch_self_admin", "PocketGPU が管理者権限で実行されています。ここから起動したアプリは iPhone の GPU を認識できません。「管理者として実行」を使わずに、PocketGPU を開き直してください。", "PocketGPU is running as administrator, so programs started from it cannot see the iPhone GPU. Open PocketGPU again without \"Run as administrator\".");
            A("launch_admin_always", "このアプリは、常に管理者権限で起動する設定になっています。", "This program always starts as administrator.");
            A("launch_admin_compat", "このアプリには「管理者として実行」の設定があります。", "This program has the \"Run as administrator\" setting.");
            A("launch_admin_tail", "Windows は管理者権限で実行されるアプリに iPhone の GPU の設定を渡さないため、普通の GPU で動作します。それでも起動しますか？", "Windows does not pass the iPhone GPU settings to programs that run as administrator, so it will use the normal GPU. Start it anyway?");
            A("launch_admin_maybe", "このアプリは、管理者アカウントでは管理者権限で起動することがあります。その場合は iPhone の GPU を認識できません。", "This program may start as administrator on an administrator account. Then it cannot see the iPhone GPU.");
            A("launch_mux_down", "Apple のサービスが実行されていません。", "The Apple service is not running.");
            A("launch_no_usb", "iPhone が USB で認識されていません。", "The iPhone is not seen over USB.");
            A("launch_busy", "他のアプリが iPhone の GPU を使用しています。そのアプリを終了してから起動してください。", "Another program is using the iPhone GPU. Close it first.");
            A("launch_mismatch", "iPhone のアプリとこのキットのバージョンが一致しません。", "The iPhone app does not match this kit.");
            A("launch_noanswer", "iPhone のアプリが応答しません。前面に表示して、画面をオンにしておいてください。", "The iPhone app does not answer. Keep it in front with the screen on.");
            A("launch_anyway", "{0}\n\n詳しくはチェックの画面で順に確かめられます。このまま起動しますか？", "{0}\n\nThe Check page goes through this step by step. Start it anyway?");
            A("launch_dx12", "DirectX 12 のゲームのようです。このキットは、まだ DirectX 12 を iPhone の GPU で描画できません。", "This looks like a DirectX 12 game. This kit cannot draw DirectX 12 on the iPhone GPU yet.");
            A("launch_dx9", "DirectX 9 のゲームのようです。このキットは、まだ DirectX 9 を iPhone の GPU で描画できません。", "This looks like a DirectX 9 game. This kit cannot draw DirectX 9 on the iPhone GPU yet.");
            A("launch_unreal", "Unreal のゲームのようです。描画を行うのはこちらです：{0}", "This looks like an Unreal game. The drawing is done by: {0}");
            A("launch_dxvk_arch", "このゲームのアーキテクチャは DXVK に対応していません。", "This game's CPU type does not match DXVK.");
            A("launch_dxvk_other", "ゲームと同じフォルダに別の d3d11.dll があります。上書きしないため、DirectX 11 は普通の GPU で描画されます。", "There is another d3d11.dll next to the game. It is not replaced, so DirectX 11 will use the normal GPU.");
            A("launch_dxvk_ask", "DirectX 11 のゲームのようです。iPhone の GPU で描画するには、DXVK の 2 つのファイルをゲームと同じフォルダに配置します。\n\n配置先：{0}\n\n元に戻すときは、そのフォルダの d3d11.dll と dxgi.dll を削除してください。DXVK を配置しますか？", "This looks like a DirectX 11 game. To draw it on the iPhone GPU, two DXVK files go next to the game.\n\nWhere: {0}\n\nTo undo, delete d3d11.dll and dxgi.dll there. Put DXVK there?");
            A("launch_dxvk_dxgi", "ゲームと同じフォルダに dxgi.dll があります。上書きしないため、DXVK は配置しませんでした。", "There is a dxgi.dll next to the game. Nothing was copied, so nothing is overwritten.");
            A("launch_dxvk_fail", "配置できませんでした。管理者権限がないと書き込めない場所の可能性があります：{0}", "Could not copy. The folder may need administrator rights: {0}");
            A("launch_dxvk_missing", "キットの DXVK が見つかりません：{0}", "DXVK was not found in the kit: {0}");
            A("launch_steam", "Steam のゲームのようです。Steam から再起動されると、iPhone の GPU は使用されません。その場合は、Steam の起動オプションに次の 1 行を入力してください。\n\n{0}", "This looks like a Steam game. If Steam restarts it, the iPhone GPU is not used. Then put this line in Steam's Launch Options:\n\n{0}");
            A("launch_fail", "起動できませんでした：{0}", "Could not start it: {0}");

            // カメラ
            A("cam_preview", "プレビュー", "Preview");
            A("cam_preview_stop", "プレビューを止める", "Stop preview");
            A("cam_preview_show", "プレビューを表示", "Show preview");
            A("cam_preview_off", "プレビューを止めています", "Preview is off");
            A("cam_start", "送信を開始", "Start");
            A("cam_stop", "送信を停止", "Stop");
            A("cam_state_on", "PC に送信中です", "Sending to the PC");
            A("cam_detail_off", "開始すると、iPhone のカメラの映像をほかのアプリで使えるようになります。", "When you start, other apps can use the iPhone camera.");
            A("cam_detail_wait", "iPhone の PocketGPU アプリで、カメラをオンにしてください。", "Turn on the camera in the PocketGPU app on the iPhone.");
            A("cam_detail_on", "{0}・{1}・{2} fps", "{0}, {1}, {2} fps");
            A("cam_keep_screen", "iPhone の画面はつけたままにしてください。", "Keep the iPhone screen on.");
            A("cam_wait_video", "iPhone の映像を待っています", "Waiting for video from the iPhone");
            A("cam_video", "映像", "Video");
            A("cam_which", "使うカメラ", "Camera");
            A("cam_which_sub", "前面・広角・超広角・望遠から選びます", "Choose front, wide, ultra wide or telephoto");
            A("cam_auto", "おまかせ", "Automatic");
            A("cam_back", "背面・広角", "Back, wide");
            A("cam_front", "前面", "Front");
            A("cam_ultra", "背面・超広角", "Back, ultra wide");
            A("cam_tele", "背面・望遠", "Back, telephoto");
            A("camname_0", "背面の広角カメラ", "Back wide camera");
            A("camname_1", "前面カメラ", "Front camera");
            A("camname_2", "背面の超広角カメラ", "Back ultra wide camera");
            A("camname_3", "背面の望遠カメラ", "Back telephoto camera");
            A("cam_res", "解像度", "Resolution");
            A("cam_res_4k", "4K（OBS 向け）", "4K (for OBS)");
            A("cam_fps", "フレームレート", "Frame rate");
            A("cam_decode", "デコード", "Decoding");
            A("cam_fps_sub", "動きの多い映像は 60 fps のほうがなめらかです", "60 fps is smoother for moving pictures");
            A("cam_quality", "画質", "Quality");
            A("cam_quality_sub", "上げると USB で送るデータが増えます", "Higher quality sends more data over USB");
            A("q_0", "おまかせ", "Automatic");
            A("q_1", "標準", "Standard");
            A("q_2", "高画質", "High");
            A("q_3", "最高画質", "Highest");
            A("cam_conn", "接続", "Connection");
            A("cam_vcam", "仮想カメラ", "Virtual camera");
            A("cam_vcam_sub", "ほかのアプリでは PocketGPU iPhone Camera という名前で表示されます", "Other apps show it as PocketGPU iPhone Camera");
            A("vcam_on", "登録済み", "Added");
            A("vcam_off", "未登録", "Not added");
            A("vcam_checking", "チェック中", "Checking");
            A("vcam_btn_on", "解除", "Remove");
            A("vcam_btn_off", "登録", "Add");
            A("vcam_partial", "一部の登録が残っています。もう一度試してください。", "Part of it is still added. Try again.");
            A("vcam_done_install", "PocketGPU iPhone Camera をカメラの一覧に追加しました。カメラを使うアプリを開き直してください。", "PocketGPU iPhone Camera was added to the camera list. Reopen apps that use the camera.");
            A("vcam_done_uninstall", "PocketGPU iPhone Camera をカメラの一覧から外しました。", "PocketGPU iPhone Camera was removed from the camera list.");
            A("vcam_fail", "仮想カメラを登録できませんでした：{0}", "Could not add the virtual camera: {0}");
            A("cam_wifi", "Wi-Fi で接続", "Connect over Wi-Fi");
            A("cam_wifi_sub", "USB ケーブルを使わずに、同じ Wi-Fi の iPhone から受け取ります", "Receive from an iPhone on the same Wi-Fi without a USB cable");
            A("cam_wifi_addr", "iPhone のアドレス", "iPhone address");
            A("cam_wifi_addr_sub", "iPhone のアプリに表示されたアドレスを入力します", "Enter the address shown in the iPhone app");
            A("cam_wifi_code", "合言葉", "Passcode");
            A("cam_wifi_code_sub", "iPhone のアプリに表示された 6 桁の数字を入力します", "Enter the 6 digits shown in the iPhone app");
            A("cam_wifi_need", "Wi-Fi で接続するには、iPhone のアドレスと合言葉を入力してください。", "To connect over Wi-Fi, enter the iPhone address and passcode.");
            // カメラの画面の「音と字幕」。ホームと各画面のスイッチと同じ状態
            A("cam_quick", "音と字幕", "Sound and captions");
            A("cam_spk_off", "PC の音を iPhone から出します", "Play PC audio on the iPhone");
            A("cam_spk_on", "PC の音を iPhone から出しています", "Playing PC audio on the iPhone");
            A("cam_mic_off", "iPhone のマイクを PC で使います", "Use the iPhone microphone on the PC");
            A("cam_mic_on", "iPhone のマイクの音を {0} に流しています", "Sending the iPhone microphone to {0}");
            A("cam_mic_nodev", "マイクとスピーカーの画面で、マイクの流す先を選択してください", "Choose where to send the microphone on the Microphone and speaker page");

            // カメラの画面の「PC からの操作」（PageCamCtl.cs）。iPhone のアプリと同じ操作は、iPhone のアプリ（CameraBridge.swift）と同じ言葉
            A("ctl_g_orient", "向き", "Orientation");
            A("ctl_g_zoom", "ズームと切り抜き", "Zoom and crop");
            A("ctl_g_focus", "ピント", "Focus");
            A("ctl_g_exposure", "明るさ", "Brightness");
            A("ctl_g_color", "色", "Color");
            A("ctl_g_light", "ライト", "Light");
            A("ctl_g_stab", "手ぶれ補正", "Stabilization");
            A("ctl_g_effects", "背景ぼかしとエフェクト", "Background blur and effects");
            A("ctl_g_heat", "熱の対策", "Heat");
            A("ctl_g_screen", "画面", "Screen");
            A("ctl_g_other", "その他", "Other");
            A("ctl_17", "左右反転", "Flip horizontally");
            A("ctl_18", "上下反転", "Flip vertically");
            A("ctl_19", "回転", "Rotation");
            A("ctl_10", "デジタルズーム", "Digital zoom");
            A("ctl_20", "切り抜く", "Crop");
            A("ctl_21", "位置", "Position");
            A("ctl_22", "大きさ", "Size");
            A("ctl_23", "横の位置", "Horizontal");
            A("ctl_24", "縦の位置", "Vertical");
            A("ctl_25", "水平を保つ", "Keep level");
            A("ctl_1", "ピント", "Focus");
            A("ctl_2", "レンズの位置", "Lens position");
            A("ctl_3", "合わせる所", "Focus on");
            A("ctl_4", "明るさ", "Brightness");
            A("ctl_6", "ISO", "ISO");
            A("ctl_7", "シャッター速度", "Shutter speed");
            A("ctl_5", "露出補正", "Exposure compensation");
            A("ctl_8", "ホワイトバランス", "White balance");
            A("ctl_9", "色温度", "Color temperature");
            A("ctl_11", "ライトをつける", "Turn on the light");
            A("ctl_12", "ライトの明るさ", "Light brightness");
            A("ctl_26", "画面のライトをつける", "Use the screen as a light");
            A("ctl_27", "画面のライトを暖かい色にする", "Warm screen light");
            A("ctl_13", "手ぶれ補正", "Stabilization");
            A("ctl_14", "背景をぼかす", "Blur the background");
            A("ctl_15", "ぼかしの強さ", "Blur strength");
            A("ctl_103", "ポートレート", "Portrait");
            A("ctl_105", "スタジオ照明", "Studio Light");
            A("ctl_16", "センターフレーム", "Center Stage");
            A("ctl_28", "iPhone が熱いときはフレームレートを下げる", "Lower the frame rate when the iPhone is hot");
            A("ctl_29", "画面を暗くする", "Dim the screen");
            A("ctl_btn_26", "つける", "Turn on");
            A("ctl_btn_29", "暗くする", "Dim");
            A("ctl_btn_run", "実行", "Run");
            A("ctl_near", "近く", "Near");
            A("ctl_far", "遠く", "Far");
            A("ctl_current", "今の値", "Current");
            A("ctl_in_use", "使用中", "In use");
            A("ctl_rot_note", "90 度と 270 度では縦長の映像になります。", "At 90° and 270° the video is portrait.");
            A("ctl_crop_note", "ズームは映像の真ん中を拡大します。切り抜きは好きな位置を切り出して、選んだ解像度に拡大して送ります。", "Zoom enlarges the center of the picture. Crop cuts out any part you choose and scales it to the selected resolution.");
            A("ctl_face_note", "顔が見つかるまでは、真ん中を切り抜きます。", "Until a face is found, the center is cropped.");
            A("ctl_level_note", "iPhone が少し傾いていても、映像の水平を保ちます。6 度まで直せます。傾きを直す余白のため、映像を少し拡大します。", "Keeps the picture level even if the iPhone is slightly tilted, up to 6°. The picture is zoomed in a little to leave room for the correction.");
            A("ctl_level_now", "いま {0} 度直しています。", "Correcting {0}° now.");
            A("ctl_focus_blur", "背景をぼかしている間は、ピントは自動で合わせます。", "While the background is blurred, focus is automatic.");
            A("ctl_focus_lock", "今のピントのまま固定します。", "Keeps the current focus.");
            A("ctl_exp_lock", "今の明るさのまま固定します。", "Keeps the current brightness.");
            A("ctl_flicker", "照明のちらつきを抑えるには、東日本では 1/50 か 1/100、西日本では 1/60 か 1/120 にします。", "To reduce flicker from lights, use 1/50 or 1/100 where the power is 50 Hz, and 1/60 or 1/120 where it is 60 Hz.");
            A("ctl_wb_lock", "今の色のまま固定します。", "Keeps the current color.");
            A("ctl_wb_note", "値を小さくすると青みが、大きくすると黄みが強くなります。", "Lower values look bluer and higher values look warmer.");
            A("ctl_torch_note", "PC と接続している間だけ点きます。", "The light is on only while the PC is connected.");
            A("ctl_torch_on", "点いています。明るさは {0}% です。", "The light is on at {0}%.");
            A("ctl_torch_low", "熱を下げるため、iOS が明るさを {0}% に下げています。", "iOS has lowered the brightness to {0}% to reduce heat.");
            A("ctl_torch_off", "点いていません。", "The light is off.");
            A("ctl_screen_light", "画面を白くして明るさを最大にし、前から顔を照らします。画面をタップすると元の明るさに戻ります。", "Turns the screen white at full brightness to light your face. Tap the screen to restore the brightness.");
            A("ctl_stab_0", "揺れを抑えません。映像の遅れがいちばん少なくなります。", "No stabilization. The delay is the shortest.");
            A("ctl_stab_1", "映像の遅れを増やさずに揺れを抑えます。写る範囲は少し狭くなります。", "Reduces shake without adding delay. The picture is slightly narrower.");
            A("ctl_stab_2", "揺れを抑えます。写る範囲が少し狭くなり、映像が少し遅れることがあります。", "Reduces shake. The picture is slightly narrower and may be slightly delayed.");
            A("ctl_stab_3", "揺れを強く抑えます。写る範囲が狭くなり、映像の遅れがはっきり増えます。", "Strongly reduces shake. The picture is narrower and the delay clearly increases.");
            A("ctl_fx_note", "iPhone のレンズの F 値は変えられないため、背景のぼかしは iPhone の処理で作ります。", "The aperture of the iPhone lens cannot be changed, so the background blur is made by processing on the iPhone.");
            A("ctl_cine_note", "F 値を小さくすると、背景が強くぼけます。iPhone のシネマティックモードと同じ処理です。30 fps までになり、映像が少し遅れることがあります。ピントは自動です。", "A smaller f-number blurs the background more. This is the same processing as Cinematic mode on the iPhone. The video is limited to 30 fps and may be slightly delayed. Focus is automatic.");
            A("ctl_cine_na", "今の設定では使えません。解像度かフレームレートを変えてください。", "Not available with the current settings. Change the resolution or frame rate.");
            A("ctl_fx_cc", "ポートレートとスタジオ照明は、コントロールセンターのビデオエフェクトで入れます。入れると 30 fps までになることがあります。", "Turn on Portrait and Studio Light in Video Effects in Control Center. They may limit the video to 30 fps.");
            A("ctl_stage_note", "人が動いても画面の中に収まるように、写す範囲を自動で動かします。", "Automatically moves the framing to keep people in the picture.");
            A("ctl_heat_note", "温度が高いときは 60 fps を 30 fps に、30 fps を 24 fps にします。危険のときは 15 fps にします。温度が下がると元に戻ります。", "When hot, 60 fps becomes 30 fps and 30 fps becomes 24 fps. When critical, 15 fps. It returns when the iPhone cools down.");
            A("ctl_dim_note", "iPhone の画面を黒くして明るさを最小にします。カメラとマイクは動き続けます。画面をタップすると元に戻ります。", "Turns the iPhone screen black at the lowest brightness. The camera and mic keep working. Tap the screen to return.");
            A("ctl_sec", "カメラの操作", "Camera controls");
            A("ctl_note_off", "送信を開始すると、ここで iPhone のカメラを操作できます。", "Start sending to control the iPhone camera here.");
            A("ctl_note_connecting", "操作の準備をしています。", "Getting the controls ready.");
            A("ctl_note_wait_iphone", "iPhone と接続すると、ここでカメラを操作できます。", "Connect the iPhone to control the camera here.");
            A("ctl_note_nopipe", "カメラの受信プログラムが古いため、PC から操作できません。PocketGPU を新しくしてください。", "The camera receiver is out of date, so the camera cannot be controlled from the PC. Update PocketGPU.");
            A("ctl_note_old_app", "iPhone のアプリを 0.7.62 以降にすると、PC から操作できます。", "Update the iPhone app to 0.7.62 or later to control the camera from the PC.");
            A("ctl_note_loading", "操作の一覧を読み込んでいます。", "Loading the controls.");
            A("ctl_error", "カメラの操作でエラーが発生しました：{0}", "A camera control error occurred: {0}");
            // 温度（信号の 3 色。言葉は iPhone のアプリの ThermalSignal と同じ）
            A("ctl_temp", "iPhone の温度", "iPhone temperature");
            A("therm_0", "正常", "Normal");
            A("therm_1", "やや高い", "Warm");
            A("therm_2", "高い", "Hot");
            A("therm_3", "危険", "Critical");
            A("ctl_heat_fps", "iPhone が熱いため、フレームレートを {0} fps に下げています。", "The iPhone is hot, so the frame rate is lowered to {0} fps.");
            A("ctl_heat_critical", "iPhone の温度が危険な状態です。", "The iPhone temperature is critical.");
            A("ctl_heat_dim_hint", "iPhone の画面を暗くすると熱を下げられます。", "Dimming the iPhone screen helps reduce heat.");
            A("ctl_dim_btn", "画面を暗くする", "Dim the screen");
            // 電池の温度と残り（BatteryPoll.cs）
            A("stat_celsius", "{0:0.0}℃", "{0:0.0}°C");
            A("stat_temp", "温度", "Temperature");
            A("stat_batt", "電池 {0}%", "Battery {0}%");
            A("stat_charging", "充電中", "Charging");
            A("stat_sep", "・", ", ");

            // マイクとスピーカー
            A("audio_state_on", "接続中です", "Connected");
            A("audio_detail_off", "iPhone のアプリで、マイクとスピーカーをオンにしてから開始してください。", "Turn on the microphone and speaker in the iPhone app, then start.");
            A("audio_spk_active", "PC の音を iPhone から出しています。", "Playing PC audio on the iPhone.");
            A("audio_mic_active", "iPhone のマイクの音を {0} に流しています。", "Sending the iPhone microphone to {0}.");
            A("audio_none", "スピーカーかマイクをオンにしてください。", "Turn on the speaker or the microphone.");
            A("audio_need_micdev", "マイクの流す先を選択してください。", "Choose where to send the microphone.");
            A("audio_spk", "スピーカー", "Speaker");
            A("audio_spk_title", "PC の音を iPhone から出す", "Play PC audio on the iPhone");
            A("audio_spk_sub", "PC で鳴っている音を、USB で iPhone に送ります", "Sends the sound playing on the PC to the iPhone over USB");
            A("audio_spk_dev", "音を取る出力", "Output to capture");
            A("audio_spk_dev_sub", "通常は既定の出力を選びます", "Usually the default output");
            A("audio_default_out", "既定の出力", "Default output");
            A("audio_mic", "マイク", "Microphone");
            A("audio_mic_title", "iPhone のマイクを PC で使う", "Use the iPhone microphone on the PC");
            A("audio_mic_sub", "iPhone のマイクの音を、仮想オーディオケーブルに流します。アプリのマイクには、そのケーブルの録音側を選んでください", "Sends the iPhone microphone to a virtual audio cable. In your app, choose the recording side of that cable as the microphone");
            A("audio_mic_dev", "流す先", "Send to");
            A("audio_mic_dev_sub", "VB-CABLE や Voicemeeter などの入力を選びます", "Choose the input of VB-CABLE, Voicemeeter or similar");
            A("audio_mic_choose", "選択してください", "Choose");
            A("audio_voice", "エコーを抑える", "Reduce echo");
            A("audio_voice_sub", "iPhone の音声処理で、通話のエコーや雑音を抑えます", "Uses the iPhone voice processing to reduce echo and noise in calls");
            // スピーカーとして使う間に PC の音を消す（AudioDevices.cs の PcMute）
            A("audio_mute_pc", "iPhone から音を出す間は PC の音を消す", "Mute the PC while the iPhone plays");
            A("audio_mute_pc_sub", "音を取る出力をミュートします。PC と iPhone の両方から音が出て、エコーのように聞こえるのを防ぎます。停止すると元に戻ります", "Mutes the output to capture, so the sound does not play on both the PC and the iPhone and sound like an echo. It is unmuted when you stop");
            A("audio_mute_pc_on", "{0} をミュートしています。停止すると元に戻ります", "{0} is muted. It is unmuted when you stop");
            A("audio_mute_pc_already", "{0} はすでにミュートされているため、そのままにしています", "{0} was already muted, so it is left as it is");
            A("audio_mute_pc_active", "PC の {0} をミュートしています。", "{0} on the PC is muted.");
            // スピーカーの遅延：ipaudio の --target-ms
            A("audio_delay", "スピーカーの遅延", "Speaker delay");
            A("audio_delay_sub", "短いほど PC の映像との遅れが小さくなりますが、音が途切れやすくなります。途切れるときは標準か長いを選んでください", "Shorter keeps the sound closer to the picture on the PC, but it breaks up more easily. If it breaks up, choose Standard or Long");
            A("delay_short", "短い", "Short");
            A("delay_std", "標準", "Standard");
            A("delay_long", "長い", "Long");
            // マイクの音を聞く：ipaudio の --mic-monitor と --mic-monitor-gain
            A("audio_mon", "マイクの音を聞く", "Listen to the microphone");
            A("audio_mon_sub", "iPhone のマイクの音を、この PC のスピーカーやヘッドホンでも聞きます。マイクがオンのときに使えます", "Also plays the iPhone microphone on this PC's speakers or headphones. Works while the microphone is on");
            A("audio_mon_checking", "ipaudio.exe がこの機能に対応しているかチェックしています", "Checking whether ipaudio.exe supports this");
            A("audio_mon_old", "お使いの ipaudio.exe はこの機能に対応していません。新しいキットの ipaudio.exe に置き換えてください", "This ipaudio.exe does not support this. Replace it with the ipaudio.exe from a newer kit");
            A("audio_mon_loop", "聞く出力が音を取る出力と同じため、マイクの音が iPhone のスピーカーからも出ます。ハウリングするときは別の出力を選んでください", "The listening output is the same as the output to capture, so the microphone also plays on the iPhone speaker. If it howls, choose another output");
            A("audio_mon_muted", "聞く出力は PC の音を消す設定でミュートされるため、マイクの音は聞こえません。別の出力を選んでください", "The listening output is muted by the setting that mutes the PC, so you will not hear the microphone. Choose another output");
            A("audio_mon_dev", "聞く出力", "Listen on");
            A("audio_mon_dev_sub", "マイクの音を鳴らすスピーカーやヘッドホンを選びます", "Choose the speakers or headphones to play the microphone on");
            A("audio_mon_vol", "聞く音量", "Listening volume");
            A("audio_mon_vol_sub", "マイクの音を聞く大きさです。ほかのアプリに送るマイクの音量は変わりません", "How loud you hear the microphone. The level sent to other apps does not change");
            A("audio_mon_vol_val", "{0}%", "{0}%");
            A("audio_mon_active", "マイクの音を {0} でも鳴らしています。", "Also playing the microphone on {0}.");

            // 字幕
            A("cap_state_on", "字幕を表示中です", "Showing captions");
            A("cap_detail_off", "PC の音声を iPhone で文字に起こします。iOS 26 以降の iPhone で使えます。", "Transcribes PC audio on the iPhone. Needs iOS 26 or later.");
            A("cap_detail_window", "字幕のウィンドウは Ctrl+Alt+C で移動できます。", "Press Ctrl+Alt+C to move the caption window.");
            A("cap_detail_nowin", "字幕のウィンドウは表示しない設定です。", "The caption window is turned off.");
            A("cap_voice", "音声", "Audio");
            A("cap_source", "音源", "Source");
            A("cap_source_sub", "字幕にする音を選びます", "Choose what to caption");
            A("cap_src_pc", "PC の音", "PC audio");
            A("cap_src_mic", "マイク", "Microphone");
            A("cap_lang", "話す言語", "Spoken language");
            A("cap_lang_sub", "音声の言語を選びます", "Choose the language of the audio");
            A("lang_ja", "日本語", "Japanese");
            A("lang_en", "英語", "English");
            A("cap_translate", "翻訳", "Translation");
            A("cap_translate_sub", "確定した文を iPhone で翻訳して、下に表示します", "Translates finished sentences on the iPhone and shows them below");
            A("cap_tr_none", "しない", "None");
            A("cap_accurate", "精度を優先", "Prefer accuracy");
            A("cap_accurate_sub", "表示は少し遅れますが、文字起こしが正確になります", "Captions come a little later but are more accurate");
            A("cap_out", "表示と保存", "Display and saving");
            A("cap_window", "字幕のウィンドウ", "Caption window");
            A("cap_window_sub", "画面の最前面に字幕を表示します。クリックは下のウィンドウに届きます", "Shows captions on top of other windows. Clicks go through to the window below");
            A("cap_obs", "OBS 用のテキストファイル", "Text file for OBS");
            A("cap_obs_sub", "OBS のテキストソースで読み込めます：{0}", "Load it in an OBS text source: {0}");
            A("cap_minutes", "議事録を保存", "Save a transcript");
            A("cap_minutes_sub", "確定した文を時刻つきで .txt と .srt に保存します：{0}", "Saves finished sentences with times as .txt and .srt: {0}");

            // 録画
            A("rec_start", "録画を開始", "Start recording");
            A("rec_stop", "録画を停止", "Stop recording");
            A("rec_open", "保存先を開く", "Open folder");
            A("rec_state_off", "録画していません", "Not recording");
            A("rec_state_on", "録画中です", "Recording");
            A("rec_detail", "このアプリから iPhone の GPU で起動したゲームの画面と PC の音を、iPhone で録画します。設定は次に起動したゲームから有効になります。", "Records, on the iPhone, the screen of games started here on the iPhone GPU and the PC audio. Settings apply to games started next.");
            A("rec_detail_on", "{0} から録画中です。停止すると、設定に合わせて保存します。", "Recording since {0}. When you stop, it is saved as set below.");
            A("rec_no_game", "録画するゲームを、GPU の画面から起動してください。", "Start the game from the GPU page to record it.");
            A("rec_settings", "設定", "Settings");
            A("rec_codec", "形式", "Format");
            A("rec_codec_sub", "HEVC はファイルが小さくなります。古い編集ソフトでは H.264 を選びます", "HEVC makes smaller files. Choose H.264 for older editing software");
            A("rec_audio", "PC の音を入れる", "Include PC audio");
            A("rec_audio_sub", "マイクとスピーカーの画面で、スピーカーをオンにしているときに入ります", "Included while the speaker is on in Microphone and speaker");
            A("rec_fetch", "PC に保存", "Save to this PC");
            A("rec_folder_sub", "録画を停止したあと、iPhone から PC に移します：{0}", "After you stop, the file is moved from the iPhone to: {0}");
            A("rec_keep", "iPhone にも残す", "Keep a copy on the iPhone");
            A("rec_keep_sub", "iPhone の「ファイル」アプリから見られます", "You can find it in the Files app on the iPhone");
            A("rec_photos", "写真に追加", "Add to Photos");
            A("rec_photos_sub", "録画を iPhone の「写真」にも保存します", "Also saves the recording to Photos on the iPhone");
            A("rec_pick_folder", "録画の保存先を選択してください", "Choose where to save recordings");

            // チェック
            A("check_intro", "この PC と iPhone を順番にチェックします。管理者権限は要りません。", "Checks this PC and the iPhone one by one. No administrator rights are needed.");
            A("check_intro_media", "この PC と iPhone を順番にチェックします。マイクとカメラだけ使う設定のため、GPU の項目はチェックしません。", "Checks this PC and the iPhone one by one. The GPU items are skipped because only the camera and audio are in use.");
            A("check_again", "もう一度チェック", "Check again");
            A("check_when", "前回のチェック {0}", "Last check {0}");
            A("check_today", "今日 {0:HH:mm}", "today {0:HH:mm}");
            A("check_never", "まだチェックしていません", "Not checked yet");
            A("check_running", "チェックしています", "Checking");
            A("check_foot", "チェックの結果は PC の中だけに保存されます。", "The results are saved only on this PC.");
            A("check_copy", "結果をコピー", "Copy the results");
            A("check_ready", "準備ができました", "Ready");
            A("check_ready_title", "iPhone の GPU で起動する", "Run on the iPhone GPU");
            A("check_ready_sub", "選択したアプリだけが iPhone の GPU を使います。", "Only the app you choose uses the iPhone GPU.");
            A("check_retry", "もう一度", "Again");
            A("v_ok", "問題なし", "OK");
            A("v_wait", "未チェック", "Not checked");
            A("v_run", "チェック中", "Checking");
            A("v_skip", "使用しません", "Not used");
            A("v_warn", "注意", "Warning");
            A("c1", "この PC", "This PC");
            A("c2", "Apple Devices または iTunes", "Apple Devices or iTunes");
            A("c3", "Apple のサービス", "Apple service");
            A("c4", "USB で iPhone を認識", "iPhone over USB");
            A("c5", "iPhone のアプリ", "iPhone app");
            A("c6", "USB の速度", "USB speed");
            A("c7", "ドライバから iPhone の GPU を認識", "iPhone GPU through the driver");
            A("c1d", "Windows のバージョンと権限", "Windows version and rights");
            A("c2d", "Windows が iPhone と通信するためのソフト", "Software Windows needs to talk to the iPhone");
            A("c3d", "Apple Mobile Device Service", "Apple Mobile Device Service");
            A("c4d", "USB ケーブルで接続した iPhone", "The iPhone on the USB cable");
            A("c5d", "iPhone の PocketGPU アプリ", "The PocketGPU app on the iPhone");
            A("c6d", "iPhone と PC の間の転送速度", "Transfer speed between the iPhone and the PC");
            A("c7d", "Vulkan のアプリから見た iPhone の GPU", "The iPhone GPU as Vulkan apps see it");
            A("c1_ok", "{0}・ビルド {1}・64 ビット", "{0}, build {1}, 64-bit");
            A("c1_ng", "64 ビット版の Windows 10 または 11 が必要です。", "A 64-bit Windows 10 or 11 is needed.");
            A("c1_admin", "PocketGPU が管理者権限で実行されています。管理者権限で実行されるアプリには、Windows が iPhone の GPU の設定を渡しません。「管理者として実行」を使わずに開き直してください。", "PocketGPU is running as administrator. Windows does not pass the iPhone GPU settings to programs that run as administrator. Open it again without \"Run as administrator\".");
            A("c1_uac", "ユーザーアカウント制御が無効になっているため、すべてのアプリが管理者権限で実行されます。この状態では iPhone の GPU を認識できません。", "User Account Control is turned off, so every program runs as administrator. The iPhone GPU cannot be seen this way.");
            A("c1_path", "このフォルダの場所に、Windows の言語設定で表示できない文字が含まれています。ドライバを読み込めないことがあります。フォルダごと C:\\PocketGPU に移動してから、もう一度起動してください。", "The path of this folder has characters the Windows language setting cannot show. The driver may fail to load. Move the whole folder to C:\\PocketGPU and open it again.");
            A("c2_devices", "Apple Devices がインストールされています", "Apple Devices is installed");
            A("c2_service", "iTunes または Apple Mobile Device Support がインストールされています", "iTunes or Apple Mobile Device Support is installed");
            A("c2_store", "Microsoft Store 版の iTunes がインストールされています", "iTunes from the Microsoft Store is installed");
            A("c2_ng", "Apple Devices も iTunes もインストールされていません。", "Neither Apple Devices nor iTunes is installed.");
            A("c2_ng2", "Windows が USB で iPhone と通信するために、どちらかが必要です。Microsoft Store の Apple Devices が手軽です。", "Windows needs one of them to talk to the iPhone over USB. Apple Devices from the Microsoft Store is the easiest.");
            A("act_store", "Microsoft Store で開く", "Open the Microsoft Store");
            A("c3_ok", "Apple のサービスが実行されています", "The Apple service is running");
            A("c3_ng", "Apple のサービスが実行されていません。", "The Apple service is not running.");
            A("c3_app", "iPhone を USB ケーブルで接続してから、{0} を起動してください。起動するとサービスが開始されます。", "Plug the iPhone in with USB, then open {0}. Opening it starts the service.");
            A("act_open_app", "{0} を起動", "Open {0}");
            A("c3_stopped", "Apple Mobile Device Service が停止しています。PC を再起動してください。それでも解決しないときは、iTunes を再インストールするか、Microsoft Store の Apple Devices をインストールしてください。", "Apple Mobile Device Service is stopped. Restart the PC. If that does not help, reinstall iTunes, or install Apple Devices from the Microsoft Store.");
            A("c3_restart", "PC を再起動してから、もう一度チェックしてください。", "Restart the PC and check again.");
            A("c4_ok", "iPhone が USB で認識されています", "The iPhone is connected over USB");
            A("c4_many", "Apple 製のデバイスが 2 台以上接続されています。最初の 1 台を使用します。", "More than one Apple device is connected. The first one is used.");
            A("c4_ng", "iPhone が USB で認識されていません。", "The iPhone is not seen over USB.");
            A("c4_wifi", "Wi-Fi 経由でのみ認識されています。USB ケーブルで接続してください。", "It is only seen over Wi-Fi. Connect it with a USB cable.");
            A("c4_driver", "Windows は iPhone を認識していますが、Apple の USB ドライバが動作していません。Apple Devices または iTunes を再インストールしてください。", "Windows sees the iPhone, but the Apple USB driver is not working. Reinstall Apple Devices or iTunes.");
            A("c4_trust", "Windows は iPhone を認識していますが、Apple のサービスはまだ認識していません。iPhone のロックを解除してください。「このコンピュータを信頼しますか？」と表示されたら「信頼」をタップしてください。Apple Devices を使っている場合は、Apple Devices を一度起動してください。", "Windows sees the iPhone, but the Apple service does not see it yet. Unlock the iPhone. If it asks \"Trust This Computer?\", tap Trust. If you use Apple Devices, open it once.");
            A("c4_plug", "iPhone を USB ケーブルで接続して、ロックを解除してください。充電専用のケーブルでは接続できません。", "Plug the iPhone in with USB and unlock it. A charge-only cable does not work.");
            A("c5_ok", "iPhone のアプリが応答しました：{0}", "The iPhone app answered: {0}");
            A("c5_mem", "{0}・使用可能なメモリ {1:N1} GB", "{0}, memory {1:N1} GB");
            A("c5_busy", "iPhone のアプリは起動していますが、他のアプリが iPhone の GPU を使用しています。", "The iPhone app is open, but another program is using the iPhone GPU.");
            A("c5_busy2", "iPhone の GPU は一度に 1 つのアプリしか使用できません。そのアプリを終了するか、iPhone のアプリを終了してから起動し直してください。", "Only one program at a time can use the iPhone GPU. Close that program, or close and reopen the iPhone app.");
            A("c5_ver", "iPhone のアプリとこのキットのバージョンが一致しません。", "The iPhone app does not match this kit.");
            A("c5_ver2", "このフォルダの {0} をインストールし直してください。", "Install {0} from this folder again.");
            A("c5_refused", "iPhone のアプリが拒否しました。コード {0}", "The iPhone app refused. Code {0}");
            A("c5_none", "iPhone のアプリが応答しません。", "The iPhone app does not answer.");
            A("c5_none2", "iPhone で PocketGPU のアプリを起動して、前面に表示したまま、画面をオンにしておいてください。バックグラウンドになると GPU を使用できません。アプリに「開始」ボタンが表示されている場合は、タップしてください。", "Open the PocketGPU app on the iPhone. Keep it in front with the screen on. In the background it cannot use the GPU. If the app shows a Start button, tap it.");
            A("c5_none3", "無料の Apple ID でインストールしたアプリは、7 日で起動できなくなります。その場合は、インストールし直してください。", "With a free Apple ID the app stops opening after 7 days. Install it again then.");
            A("c6_wait", "数秒かかります", "This takes a few seconds");
            A("c6_line", "iPhone→PC {0:N0} MB/s・PC→iPhone {1:N0} MB/s・往復時間 {2:N2} ms", "iPhone to PC {0:N0} MB/s, PC to iPhone {1:N0} MB/s, round trip {2:N2} ms");
            A("c6_slow", "USB 2 の速度です。負荷の高いゲームは遅くなります。", "This is USB 2 speed. Heavy games will be slow.");
            A("c6_slow2", "USB 3 に対応した USB-C ケーブルを使い、PC の USB-C ポートまたは USB 3 ポートに接続してください。iPhone に付属のケーブルは USB 2 です。USB 3 で高速になるのは、iPhone 15 Pro 以降の Pro モデルだけです。", "Use a USB-C cable that supports USB 3, and plug it into a USB-C or USB 3 port on the PC. The cable that came with the iPhone is USB 2. Only iPhone 15 Pro and later Pro models get faster with USB 3.");
            A("c6_mid", "使用できる速度です。USB 3 のケーブルとポートを使うと、さらに高速になることがあります。", "This speed works. A USB 3 cable and port may be faster.");
            A("c6_none", "測定できませんでした。アプリのバージョンが古い可能性があります。", "Could not measure. The app may be old.");
            A("c6_err", "測定中に接続が切れました：{0}", "The connection dropped while measuring: {0}");
            A("c7_novk", "Vulkan ランタイムの vulkan-1.dll がありません。GPU のドライバを更新してください。", "The Vulkan runtime vulkan-1.dll is missing. Update the GPU driver.");
            A("c7_oldvk", "Vulkan ランタイムが古いです。バージョン {0}。GPU のドライバを更新してください。", "The Vulkan runtime is old: {0}. Update the GPU driver.");
            A("c7_admin", "管理者権限で実行されているため、この項目はチェックできません。この PC の項目を見てください。", "Cannot check this while running as administrator. See This PC.");
            A("c7_ok", "Vulkan のアプリから {0} が見えています", "Vulkan apps can see {0}");
            A("c7_ng", "ドライバ経由で iPhone の GPU を認識できません。", "The driver does not see the iPhone GPU.");
            A("c7_missing", "vklist.exe がありません。キットをすべて展開し直してください。", "vklist.exe is missing. Extract the whole kit again.");
            A("c7_timeout", "30 秒以内に完了しませんでした。iPhone のアプリを終了してから、起動し直してください。", "It did not finish in 30 seconds. Close and reopen the iPhone app.");
            A("c7_av", "ウイルス対策ソフトが iphone_icd.dll をブロックしていないかチェックしてください。", "Check that antivirus software has not blocked iphone_icd.dll.");

            // 設定
            A("set_lang", "言語", "Language");
            A("set_lang_sub", "画面の言語を選びます", "Choose the language of this app");
            A("lang_auto", "Windows に合わせる", "Match Windows");
            A("lang_ja_self", "日本語", "日本語");
            A("lang_en_self", "English", "English");
            A("set_theme", "テーマ", "Theme");
            A("set_theme_sub", "画面の色を選びます", "Choose the colors of this app");
            A("theme_auto", "Windows に合わせる", "Match Windows");
            A("theme_light", "ライト", "Light");
            A("theme_dark", "ダーク", "Dark");
            A("set_data", "設定とログの保存先", "Settings and logs");
            A("set_tools", "ツール", "Tools");
            A("tool_missing", "見つかりません。PocketGPU.exe と同じフォルダに置いてください", "Not found. Put it in the same folder as PocketGPU.exe");
            A("tool_webcam_rx", "カメラの受信", "Camera receiver");
            A("tool_camera_setup", "仮想カメラの登録", "Virtual camera setup");
            A("tool_ipaudio", "マイクとスピーカー", "Microphone and speaker");
            A("tool_livecap", "字幕", "Captions");
            A("tool_vklist", "ドライバのチェック", "Driver check");
            A("tool_kit", "iPhone の GPU のドライバ", "iPhone GPU driver");
            A("set_about", "バージョン情報", "About");
            A("set_version", "PocketGPU {0}", "PocketGPU {0}");
            A("set_about_sub", "iPhone を PC の GPU・カメラ・マイク・スピーカーとして使うための Windows アプリです。管理者権限は要りません。", "The Windows app for using an iPhone as a GPU, camera, microphone and speaker for this PC. No administrator rights are needed.");

            // アップデート（PageUpdate.cs）
            A("upd_title", "アップデート", "Updates");
            A("upd_check", "アップデートを確認", "Check for updates");
            A("upd_apply", "アップデート", "Update");
            A("upd_idle", "キットのファイルを最新のバージョンにします。変更されたファイルだけをダウンロードします。", "Brings the kit files up to the latest version. Only the files that changed are downloaded.");
            A("upd_checking", "チェックしています…", "Checking...");
            A("upd_latest", "最新の状態です。バージョン {0}", "Up to date. Version {0}");
            A("upd_norelease", "配布中の更新はまだありません。", "No update has been published yet.");
            A("upd_available", "新しいバージョン {0} があります。更新するファイルは {1} 個、{2} です。", "Version {0} is available: {1} files, {2}.");
            A("upd_available_ipa", "iPhone のアプリの新しいバージョンも、キットのフォルダに保存します。", " The new iPhone app is also saved in the kit folder.");
            A("upd_unsupported", "このキットは更新に対応していません。Discord の #get-kit で /kit と入力して、表示される手順で更新用のファイルを追加してください。", "This kit cannot update itself yet. Type /kit in #get-kit on Discord and follow the steps to add the updater.");
            A("upd_token", "このキットの更新用の情報が使えません。Discord の #get-kit で /kit と入力してください。", "The update information in this kit is not valid. Type /kit in #get-kit on Discord.");
            A("upd_network", "サーバーに接続できませんでした。インターネットの接続をチェックしてください。", "Could not connect to the server. Check the internet connection.");
            A("upd_error", "チェックできませんでした：{0}", "Could not check: {0}");
            A("upd_confirm", "PocketGPU を閉じてから更新します。カメラ・マイク・字幕は停止します。更新が終わると、PocketGPU がもう一度起動します。続けますか？", "PocketGPU closes and then updates. The camera, microphone and captions stop. PocketGPU starts again when the update is done. Continue?");
            A("upd_starting", "更新を始めています…", "Starting the update...");
            A("upd_game", "iPhone の GPU で動いているゲームを終了してから、更新してください。", "Close the games running on the iPhone GPU before updating.");
        }
    }
}
