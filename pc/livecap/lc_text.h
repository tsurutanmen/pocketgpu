// 画面に出る文。日本語と英語。どちらを使うかは Windows の表示の言語か --ui で決める。文は仮
#pragma once

enum {
    T_USAGE, T_UNKNOWN_ARG, T_NEED_VALUE, T_BAD_SOURCE, T_NO_LOOPBACK, T_NO_MIC, T_DEV_OPEN_FAIL, T_FORMAT_UNSUPPORTED,
    T_WAV_FAIL, T_WAV_FORMAT, T_KEEPALIVE_FAIL, T_AUDIO, T_CONNECTING, T_READY, T_MUX_DOWN, T_MUX_NOLIST, T_NO_IPHONE,
    T_PORT_REFUSED, T_ADDR_FAIL, T_START_FAIL, T_NO_ANSWER, T_IPHONE_ERROR, T_LINK_LOST, T_FINAL_LINE, T_TR_FAIL,
    T_HOTKEY_FAIL, T_MINUTES, T_SUMMARY, T_WINCHECK, T_HINT, T_WAITING, T_TITLE, T_DEFAULT, T_STOPPING, T_COM_FAIL,
    T_CAPTURE_FAIL, T_FILE_FAIL, T_NO_DEVICES,
    T_TR_DETAIL, T_TR_PREPARING, T_TR_READY, T_TR_PREP_FAIL, T_TR_HINT, T__N
};

static const char *const T_JA[T__N] = {
    [T_USAGE] =
        "livecap 0.1: PC の音声を USB 接続の iPhone で文字に起こし、画面に字幕を表示します。\n"
        "\n"
        "使い方:\n"
        "  livecap                                    既定の出力デバイスの音声を字幕にします\n"
        "  livecap --source mic                       既定のマイクを使います。mic:名前の一部 でも指定できます\n"
        "  livecap --source \"loopback:Steam Streaming\" 出力デバイスを名前の一部で指定します\n"
        "  livecap --lang en-US --translate ja        英語の字幕と、その日本語訳を表示します\n"
        "  livecap --obs out\\caption.txt              OBS のテキストソースが読むファイルに字幕を書き出します\n"
        "  livecap --minutes out\\minutes              確定した文を時刻つきの .txt と .srt に保存します\n"
        "  livecap --list                             使えるデバイスを一覧表示します\n"
        "\n"
        "オプション:\n"
        "  --source 取り方          loopback、loopback:名前、mic、mic:名前、wav:ファイル のどれかです。既定は loopback です\n"
        "  --lang 言語              ja-JP、en-US などです。既定は ja-JP です\n"
        "  --accurate               速さより正確さを優先します\n"
        "  --translate 言語         確定した文を iPhone で翻訳して下に表示します。例: ja、en\n"
        "  --addr ホスト:ポート     USB を使わずにこのアドレスへ接続します。テスト用です\n"
        "  --mux ホスト:ポート      usbmuxd の接続先です。既定は 127.0.0.1:27015 です\n"
        "  --no-window              字幕ウィンドウを表示しません\n"
        "  --no-keepalive           loopback で無音を再生し続ける処理を止めます。何も再生されていない間は無音で埋めます\n"
        "  --obs ファイル           今の字幕を書き続けるテキストファイルです\n"
        "  --obs-lines 行数         OBS のファイルに書く行数です。既定は 2 です\n"
        "  --minutes フォルダ       確定した文を時刻つきで保存するフォルダです\n"
        "  --lines 行数             ウィンドウに表示する確定した文の行数です。既定は 2 です\n"
        "  --font-size ポイント     文字の大きさです。既定は 28 です\n"
        "  --opacity 値             ウィンドウの不透明度です。0.1 から 1.0 で、既定は 0.82 です\n"
        "  --geometry 幅x高さ+X+Y   ウィンドウの位置と大きさです。省略すると保存した位置を使います\n"
        "  --settings ファイル      ウィンドウの位置と文字の大きさを保存するファイルです。既定は livecap.exe と同じフォルダの livecap_settings.json で、書き込めないときは %APPDATA%\\PocketGPU に保存します\n"
        "  --chunk-ms ミリ秒        iPhone へ送る音声の単位です。既定は 100 です\n"
        "  --seconds 秒             この秒数で停止します\n"
        "  --end-wait 秒            停止するときに残りの確定を待つ秒数です。既定は 5 です\n"
        "  --latency-csv ファイル   結果ごとの遅延を CSV に保存します\n"
        "  --save-audio ファイル    iPhone に送った 16 kHz の音声を WAV に保存します\n"
        "  --quiet                  確定した文をコンソールに表示しません\n"
        "  --selfcheck              2.5 秒後にウィンドウの最前面とクリックの透過をチェックします\n"
        "  --ui ja|en               表示の言語です\n"
        "  --list                   使えるデバイスを一覧表示します\n"
        "\n"
        "字幕ウィンドウ:\n"
        "  Ctrl+Alt+C  移動モードを切り替えます。ドラッグで移動、右下の角で大きさ、ホイールで文字の大きさを変えます。位置は保存されます\n"
        "  Ctrl+Alt+H  表示と非表示を切り替えます\n"
        "  Ctrl+Alt+Q  終了します\n",
    [T_UNKNOWN_ARG] = "不明なオプションです: %s。使い方は --help で表示できます。",
    [T_NEED_VALUE] = "%s には値が必要です。",
    [T_BAD_SOURCE] = "音声の取り方が正しくありません: %s。loopback、loopback:名前、mic、mic:名前、wav:ファイル のどれかを指定してください。",
    [T_NO_LOOPBACK] = "出力デバイスが見つかりません: %s。--list で一覧をチェックしてください。",
    [T_NO_MIC] = "マイクが見つかりません: %s。--list で一覧をチェックしてください。",
    [T_DEV_OPEN_FAIL] = "音声デバイスを開けません: %s。エラーコード 0x%08lx です。",
    [T_FORMAT_UNSUPPORTED] = "このデバイスの音声形式には対応していません: %d ビット",
    [T_WAV_FAIL] = "WAV ファイルを読めません: %s",
    [T_WAV_FORMAT] = "この WAV 形式には対応していません: %s。16 ビット、24 ビット、32 ビット float の PCM を使ってください。",
    [T_KEEPALIVE_FAIL] = "無音の再生を開始できませんでした。何も再生されていない間は無音で埋めます。",
    [T_AUDIO] = "音声: %s、%d Hz %d ch を %d Hz のモノラルに変換します。",
    [T_CONNECTING] = "iPhone に接続しています。初回は iPhone が言語モデルをダウンロードするため時間がかかることがあります。",
    [T_READY] = "iPhone: %s、%s、%s、準備 %.0f ms",
    [T_MUX_DOWN] = "Apple Mobile Device Service に接続できません: %s。iTunes か Apple Devices アプリが入っているかチェックしてください。",
    [T_MUX_NOLIST] = "usbmuxd がデバイスの一覧を返しません。",
    [T_NO_IPHONE] = "USB で接続された iPhone が見つかりません。ケーブルと、このコンピューターを信頼する設定をチェックしてください。",
    [T_PORT_REFUSED] = "iPhone のアプリに接続できません。アプリを開いてからもう一度試してください。usbmuxd の結果は %lld です。",
    [T_ADDR_FAIL] = "%s に接続できません。",
    [T_START_FAIL] = "iPhone で文字起こしを開始できません: %s",
    [T_NO_ANSWER] = "iPhone から応答がありません。",
    [T_IPHONE_ERROR] = "iPhone からのエラー: %s",
    [T_LINK_LOST] = "iPhone との接続が切れました。",
    [T_FINAL_LINE] = "[%7.2f-%7.2f] %s  遅延 %.0f ms",
    [T_TR_FAIL] = "翻訳できませんでした: %s",
    [T_HOTKEY_FAIL] = "Ctrl+Alt+%c はほかのアプリが使っているため登録できませんでした。",
    [T_MINUTES] = "議事録を保存しました: %s.txt と %s.srt",
    [T_SUMMARY] = "まとめ: %s",
    [T_WINCHECK] = "ウィンドウのチェック: %s",
    [T_HINT] = "移動モード: ドラッグで移動、右下の角で大きさ、ホイールで文字の大きさを変えます。右クリックか Ctrl+Alt+C で固定します。",
    [T_WAITING] = "字幕を待っています。Ctrl+Alt+C でウィンドウを移動できます。",
    [T_TITLE] = "iPhone 字幕",
    [T_DEFAULT] = "既定",
    [T_STOPPING] = "停止しています。残りの文を確定しています。",
    [T_COM_FAIL] = "Windows の音声機能を初期化できません。",
    [T_CAPTURE_FAIL] = "音声の取り込みを開始できません。エラーコード 0x%08lx です。",
    [T_FILE_FAIL] = "ファイルに書き込めません: %s",
    [T_NO_DEVICES] = "使える音声デバイスがありません。",
    [T_TR_DETAIL] = "  詳しい理由: %s",
    [T_TR_PREPARING] = "iPhone で翻訳の準備をしています: %s から %s",
    [T_TR_READY] = "翻訳の準備ができました。かかった時間は %.0f ms です。",
    [T_TR_PREP_FAIL] = "翻訳の準備ができませんでした: %s。確定した文の翻訳はそのまま試します。",
    [T_TR_HINT] = "iPhone の設定アプリで、翻訳に使う %s と %s の言語データをダウンロードしてください。",
};

static const char *const T_EN[T__N] = {
    [T_USAGE] =
        "livecap 0.1: transcribes PC audio on a USB-connected iPhone and shows captions on the screen.\n"
        "\n"
        "Usage:\n"
        "  livecap                                    Caption the audio of the default output device\n"
        "  livecap --source mic                       Use the default microphone. mic:NAME also works\n"
        "  livecap --source \"loopback:Steam Streaming\" Pick an output device by part of its name\n"
        "  livecap --lang en-US --translate ja        Show English captions and their Japanese translation\n"
        "  livecap --obs out\\caption.txt              Write captions to a file for an OBS text source\n"
        "  livecap --minutes out\\minutes              Save finished sentences with timestamps as .txt and .srt\n"
        "  livecap --list                             List the devices you can use\n"
        "\n"
        "Options:\n"
        "  --source SOURCE          loopback, loopback:NAME, mic, mic:NAME or wav:FILE. Default: loopback\n"
        "  --lang LANG              ja-JP, en-US and so on. Default: ja-JP\n"
        "  --accurate               Prefer accuracy over speed\n"
        "  --translate LANG         Translate finished sentences on the iPhone and show them below. Example: ja, en\n"
        "  --addr HOST:PORT         Connect to this address instead of USB. For testing\n"
        "  --mux HOST:PORT          Address of usbmuxd. Default: 127.0.0.1:27015\n"
        "  --no-window              Do not show the caption window\n"
        "  --no-keepalive           Do not play silence on the loopback device. Gaps are filled with silence instead\n"
        "  --obs FILE               Text file that always holds the current captions\n"
        "  --obs-lines N            Number of lines in the OBS file. Default: 2\n"
        "  --minutes FOLDER         Folder for finished sentences with timestamps\n"
        "  --lines N                Number of finished lines in the window. Default: 2\n"
        "  --font-size PT           Text size. Default: 28\n"
        "  --opacity VALUE          Window opacity from 0.1 to 1.0. Default: 0.82\n"
        "  --geometry WxH+X+Y       Window position and size. Without it, the saved position is used\n"
        "  --settings FILE          File that stores the window position and text size. Default: livecap_settings.json next to livecap.exe, or %APPDATA%\\PocketGPU if that folder is not writable\n"
        "  --chunk-ms MS            Size of each audio block sent to the iPhone. Default: 100\n"
        "  --seconds S              Stop after this many seconds\n"
        "  --end-wait S             Seconds to wait for the last results when stopping. Default: 5\n"
        "  --latency-csv FILE       Save the latency of each result as CSV\n"
        "  --save-audio FILE        Save the 16 kHz audio sent to the iPhone as WAV\n"
        "  --quiet                  Do not print finished sentences to the console\n"
        "  --selfcheck              Check after 2.5 seconds that the window stays on top and lets clicks through\n"
        "  --ui ja|en               Language of the messages\n"
        "  --list                   List the devices you can use\n"
        "\n"
        "Caption window:\n"
        "  Ctrl+Alt+C  Toggle move mode. Drag to move, drag the bottom-right corner to resize, scroll to change the text size. The position is saved\n"
        "  Ctrl+Alt+H  Show or hide the window\n"
        "  Ctrl+Alt+Q  Quit\n",
    [T_UNKNOWN_ARG] = "Unknown option: %s. Run with --help to see the usage.",
    [T_NEED_VALUE] = "%s needs a value.",
    [T_BAD_SOURCE] = "Invalid source: %s. Use loopback, loopback:NAME, mic, mic:NAME or wav:FILE.",
    [T_NO_LOOPBACK] = "Output device not found: %s. Check the list with --list.",
    [T_NO_MIC] = "Microphone not found: %s. Check the list with --list.",
    [T_DEV_OPEN_FAIL] = "Could not open the audio device: %s. Error code 0x%08lx.",
    [T_FORMAT_UNSUPPORTED] = "Unsupported audio format on this device: %d bit",
    [T_WAV_FAIL] = "Could not read the WAV file: %s",
    [T_WAV_FORMAT] = "Unsupported WAV format: %s. Use 16-bit, 24-bit or 32-bit float PCM.",
    [T_KEEPALIVE_FAIL] = "Could not start silent playback. While nothing is playing, the gap is filled with silence.",
    [T_AUDIO] = "Audio: %s, converting %d Hz %d ch to %d Hz mono.",
    [T_CONNECTING] = "Connecting to the iPhone. The first run can take a while because the iPhone downloads the language model.",
    [T_READY] = "iPhone: %s, %s, %s, ready in %.0f ms",
    [T_MUX_DOWN] = "Could not reach Apple Mobile Device Service at %s. Check that iTunes or the Apple Devices app is installed.",
    [T_MUX_NOLIST] = "usbmuxd did not return the device list.",
    [T_NO_IPHONE] = "No iPhone connected over USB was found. Check the cable and that the iPhone trusts this computer.",
    [T_PORT_REFUSED] = "Could not connect to the app on the iPhone. Open the app and try again. usbmuxd result: %lld.",
    [T_ADDR_FAIL] = "Could not connect to %s.",
    [T_START_FAIL] = "The iPhone could not start transcription: %s",
    [T_NO_ANSWER] = "No response from the iPhone.",
    [T_IPHONE_ERROR] = "Error from the iPhone: %s",
    [T_LINK_LOST] = "The connection to the iPhone was lost.",
    [T_FINAL_LINE] = "[%7.2f-%7.2f] %s  latency %.0f ms",
    [T_TR_FAIL] = "Translation failed: %s",
    [T_HOTKEY_FAIL] = "Ctrl+Alt+%c is used by another app and could not be registered.",
    [T_MINUTES] = "Saved the transcript: %s.txt and %s.srt",
    [T_SUMMARY] = "Summary: %s",
    [T_WINCHECK] = "Window check: %s",
    [T_HINT] = "Move mode: drag to move, drag the bottom-right corner to resize, scroll to change the text size. Right-click or press Ctrl+Alt+C to lock.",
    [T_WAITING] = "Waiting for captions. Press Ctrl+Alt+C to move this window.",
    [T_TITLE] = "iPhone Captions",
    [T_DEFAULT] = "default",
    [T_STOPPING] = "Stopping. Finishing the remaining text.",
    [T_COM_FAIL] = "Could not initialize Windows audio.",
    [T_CAPTURE_FAIL] = "Could not start audio capture. Error code 0x%08lx.",
    [T_FILE_FAIL] = "Could not write the file: %s",
    [T_NO_DEVICES] = "No audio devices are available.",
    [T_TR_DETAIL] = "  Details: %s",
    [T_TR_PREPARING] = "Preparing translation on the iPhone: %s to %s",
    [T_TR_READY] = "Translation is ready. It took %.0f ms.",
    [T_TR_PREP_FAIL] = "Could not prepare translation: %s. Finished sentences will still be translated.",
    [T_TR_HINT] = "On the iPhone, download the %s and %s translation languages in the Settings app.",
};

static const char *const *TX = T_EN;
#define TT(id) (TX[id])

// --ui が無ければ Windows の表示の言語。日本語なら日本語、ほかは英語
static void text_choose(const char *ui) {
    if (ui && !strcmp(ui, "ja")) TX = T_JA;
    else if (ui && !strcmp(ui, "en")) TX = T_EN;
    else TX = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE ? T_JA : T_EN;
}
