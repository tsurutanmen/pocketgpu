# PocketGPU を始める：この PC で iPhone の GPU を使うのに要る物を順に確かめ、足りない物を案内する。
# Start-PocketGPU.cmd から呼ぶ。管理者の権限は要らない。レジストリにも Windows の設定にも書かない。
# 最後に聞いて「はい」のときだけ、自分のスタートメニューと右クリックの「送る」に入口（.lnk のファイル）を作る。
#   Start-PocketGPU.cmd                    順に確かめる
#   Start-PocketGPU.cmd -RemoveShortcuts   作った入口を消す
# 確かめる順：この PC → Apple のソフト → Apple のサービス → iPhone が USB で見えるか → iPhone のアプリが答えるか
#             → USB の太さ → ドライバで iPhone の GPU が見えるか
# 画面の文は仮。言い回しは pgpu_common.ps1 と同じく M '日本語' 'English' の所で直す
. (Join-Path $PSScriptRoot 'pgpu_common.ps1')

$Total = 7
$AllOk = $true
$Dev = $null

function Finish([int]$code) {
    $p = Save-Log 'pocketgpu-start-log.txt'
    Out-Line ''
    Out-Line ((M 'このときのログ：' 'Log of this run: ') + $p)
    if (-not $script:NoPrompt -and $env:PGPU_NOPAUSE -ne '1') { [void](Read-Host (M 'Enter キーを押すと閉じます' 'Press Enter to close')) }
    exit $code
}
function Stop-Here {
    Out-Line ''
    Out-Line (M 'ここで中断しました。問題を解消してから、もう一度 Start-PocketGPU.cmd を起動してください。' 'Stopped here. After fixing it, open Start-PocketGPU.cmd again.') 'Yellow'
    if (Test-Path -LiteralPath (Join-Path $script:KitDir 'Fix-PocketGPU.cmd')) {
        Out-Line (M '直せる問題は、Fix-PocketGPU.cmd で自動的に直せることがあります。' 'Fix-PocketGPU.cmd can fix some problems automatically.')
    }
    Out-Line (M '解決しないときは、下のログファイルを Discord で送ってください。' 'If you are stuck, send the log file below to us on Discord.')
    Finish 1
}

# ---- 入口を作る・消す ----
function Get-ShortcutDirs {
    if ($env:PGPU_SHORTCUT_DIR) {
        return @{ Programs = Join-Path $env:PGPU_SHORTCUT_DIR 'Programs'; SendTo = Join-Path $env:PGPU_SHORTCUT_DIR 'SendTo' }
    }
    return @{ Programs = [Environment]::GetFolderPath('Programs'); SendTo = [Environment]::GetFolderPath('SendTo') }
}
function New-PgpuShortcuts {
    $d = Get-ShortcutDirs
    $folder = Join-Path $d.Programs 'PocketGPU'
    New-Item -ItemType Directory -Force $folder | Out-Null
    New-Item -ItemType Directory -Force $d.SendTo | Out-Null
    $run = Join-Path $script:KitDir 'Run-on-iPhone-GPU.cmd'
    $start = Join-Path $script:KitDir 'Start-PocketGPU.cmd'
    $runName = (M 'iPhone の GPU で起動' 'Run on iPhone GPU') + '.lnk'
    $items = @(
        @{ Path = Join-Path $folder $runName; Target = $run; Desc = (M 'アプリを選択して iPhone の GPU で起動します' 'Choose a program and run it on the iPhone GPU') },
        @{ Path = Join-Path $folder ((M 'PocketGPU をチェック' 'Check PocketGPU') + '.lnk'); Target = $start; Desc = (M 'iPhone の GPU を使用できるかチェックします' 'Check that the iPhone GPU can be used') },
        @{ Path = Join-Path $d.SendTo $runName; Target = $run; Desc = (M '選択したアプリを iPhone の GPU で起動します' 'Run the selected program on the iPhone GPU') }
    )
    $sh = New-Object -ComObject WScript.Shell
    $made = @()
    foreach ($it in $items) {
        $l = $sh.CreateShortcut($it.Path)
        $l.TargetPath = $it.Target
        $l.WorkingDirectory = $script:KitDir
        $l.Description = $it.Desc
        $l.Save()
        $made += $it.Path
    }
    # 消すときのために、作った物の場所を覚える
    [IO.File]::WriteAllLines((Join-Path $PSScriptRoot 'shortcuts.txt'), [string[]]($made + $folder), (New-Object Text.UTF8Encoding($true)))
    return $made
}
function Remove-PgpuShortcuts {
    $list = Join-Path $PSScriptRoot 'shortcuts.txt'
    if (-not (Test-Path $list)) { Out-Line (M '作成したショートカットはありません。' 'There are no shortcuts to remove.'); return }
    foreach ($p in [IO.File]::ReadAllLines($list)) {
        if ($p -like '*.lnk' -and (Test-Path -LiteralPath $p)) { Remove-Item -LiteralPath $p; Out-Line ((M '削除しました：' 'Removed: ') + $p) }
        elseif ((Test-Path -LiteralPath $p -PathType Container) -and -not (Get-ChildItem -LiteralPath $p -Force)) { Remove-Item -LiteralPath $p }
    }
    Remove-Item -LiteralPath $list
}

if ($args -contains '-RemoveShortcuts') { Remove-PgpuShortcuts; Finish 0 }

Out-Line (M 'PocketGPU を開始します' 'Start PocketGPU')
Out-Line (M 'iPhone の GPU を使うために必要な項目を、順にチェックします。管理者権限は必要ありません。' 'This checks, one by one, what is needed to use the iPhone GPU. No administrator rights are needed.')

# ==== 1. この PC ====
Say-Step 1 $Total (M 'この PC' 'This PC')
$os = [Environment]::OSVersion.Version
if (-not [Environment]::Is64BitOperatingSystem -or $os.Major -lt 10) {
    Say-NG (M '64 ビット版の Windows 10 または 11 が必要です。' 'A 64-bit Windows 10 or 11 is needed.')
    Stop-Here
}
$winName = $(if ($os.Build -ge 22000) { 'Windows 11' } else { 'Windows 10' })
Say-OK ((M '{0}・ビルド {1}・64 ビット' '{0}, build {1}, 64-bit') -f $winName, $os.Build)
$Elevated = Test-Elevated
if ($Elevated) {
    Say-Warn (M 'このウィンドウは管理者権限で実行されています。' 'This window is running as administrator.')
    Say-Info (M '管理者権限で実行されるアプリには、Windows が iPhone の GPU の設定を渡しません。' 'Windows does not pass the iPhone GPU settings to programs that run as administrator.')
    Say-Info (M 'このウィンドウを閉じて、「管理者として実行」を使わずに Start-PocketGPU.cmd をダブルクリックし直してください。' 'Close this window and double-click Start-PocketGPU.cmd again, without "Run as administrator".')
    $AllOk = $false
}
if (Test-UacOff) {
    Say-Warn (M 'ユーザーアカウント制御が無効になっているため、すべてのアプリが管理者権限で実行されます。この状態では iPhone の GPU を認識できません。' 'User Account Control is turned off, so every program runs as administrator. The iPhone GPU cannot be seen this way.')
    $AllOk = $false
}
if (-not (Test-AnsiPath $script:KitDir)) {
    Say-Warn (M 'このフォルダの場所に、Windows の言語設定で表示できない文字が含まれています。ドライバを読み込めないことがあります。' 'The path of this folder has characters that the Windows language setting cannot show. The driver may fail to load.')
    Say-Info (M 'フォルダごと C:\PocketGPU に移動してから、もう一度起動してください。' 'Move the whole folder to C:\PocketGPU and open it again.')
}

# ==== 2. Apple のソフト ====
while ($true) {
    Say-Step 2 $Total (M 'Apple Devices または iTunes' 'Apple Devices or iTunes')
    $Sw = Get-AppleSoftware
    if ($Sw.AppleDevices) { Say-OK (M 'Apple Devices がインストールされています。' 'Apple Devices is installed.') }
    if ($Sw.Service) { Say-OK (M 'iTunes または Apple Mobile Device Support がインストールされています。' 'iTunes or Apple Mobile Device Support is installed.') }
    if ($Sw.StoreITunes -and -not $Sw.AppleDevices -and -not $Sw.Service) { Say-OK (M 'Microsoft Store 版の iTunes がインストールされています。' 'iTunes from the Microsoft Store is installed.') }
    if ($Sw.AppleDevices -or $Sw.Service -or $Sw.StoreITunes) { break }
    Say-NG (M 'Apple Devices も iTunes もインストールされていません。' 'Neither Apple Devices nor iTunes is installed.')
    Say-Info (M 'Windows が USB で iPhone と通信するために、どちらかが必要です。Microsoft Store の Apple Devices が手軽です。' 'Windows needs one of them to talk to the iPhone over USB. Apple Devices from the Microsoft Store is the easiest.')
    if (Ask-YesNo (M 'Microsoft Store で Apple Devices のページを開きますか？' 'Open the Apple Devices page in the Microsoft Store?')) {
        Start-Process 'ms-windows-store://pdp/?ProductId=9NP83LWLPZ9K'
    }
    if (-not (Ask-Retry)) { Stop-Here }
}

# ==== 3. Apple のサービス ====
while ($true) {
    Say-Step 3 $Total (M 'Apple のサービス' 'Apple service')
    if (Test-MuxListening) { Say-OK (M 'Apple のサービスが実行されています。' 'The Apple service is running.'); break }
    Say-NG (M 'Apple のサービスが実行されていません。' 'The Apple service is not running.')
    $pkg = $(if ($Sw.AppleDevices) { $Sw.AppleDevices } elseif ($Sw.StoreITunes) { $Sw.StoreITunes } else { $null })
    if ($pkg) {
        $appName = $(if ($Sw.AppleDevices) { 'Apple Devices' } else { 'iTunes' })
        Say-Info ((M 'iPhone を USB ケーブルで接続してから、{0} を起動してください。起動するとサービスが開始されます。' 'Plug the iPhone in with USB, then open {0}. Opening it starts the service.') -f $appName)
        $aumid = Get-Aumid $pkg
        if ($aumid -and (Ask-YesNo ((M '{0} を起動しますか？' 'Open {0}?') -f $appName))) {
            Start-Process ('shell:AppsFolder\' + $aumid)
            Out-Line ('      ' + (M 'サービスが開始されるまで、最大 30 秒待ちます' 'Waiting up to 30 seconds for the service'))
            for ($i = 0; $i -lt 30 -and -not (Test-MuxListening); $i++) { Start-Sleep -Seconds 1 }
            continue
        }
    } elseif ($Sw.Service -and $Sw.Service.Status -ne 'Running') {
        Say-Info (M 'Apple Mobile Device Service が停止しています。PC を再起動してください。' 'Apple Mobile Device Service is stopped. Restart the PC.')
        Say-Info (M 'それでも解決しないときは、iTunes を再インストールするか、Microsoft Store の Apple Devices をインストールしてください。' 'If that does not help, reinstall iTunes, or install Apple Devices from the Microsoft Store.')
    } else {
        Say-Info (M 'PC を再起動してから、もう一度チェックしてください。' 'Restart the PC and check again.')
    }
    if (-not (Ask-Retry)) { Stop-Here }
}

# ==== 4. iPhone が USB で見えるか ====
while ($true) {
    Say-Step 4 $Total (M 'iPhone が USB で認識されるか' 'iPhone over USB')
    $devs = @(Get-MuxDevices)
    $usb = @($devs | Where-Object { $_.Type -eq 'USB' })
    if ($usb.Count -gt 0) {
        $Dev = $usb[0]
        Say-OK (M 'iPhone が USB で認識されています。' 'The iPhone is connected over USB.')
        if ($usb.Count -gt 1) { Say-Warn (M 'Apple 製のデバイスが 2 台以上接続されています。最初の 1 台を使用します。' 'More than one Apple device is connected. The first one is used.') }
        if ($Dev.Speed) { $script:Log.Add('      usbmuxd ConnectionSpeed=' + $Dev.Speed + ' ProductID=' + $Dev.ProductId) }
        break
    }
    Say-NG (M 'iPhone が USB で認識されていません。' 'The iPhone is not seen over USB.')
    if (@($devs | Where-Object { $_.Type -eq 'Network' }).Count -gt 0) {
        Say-Info (M 'Wi-Fi 経由でのみ認識されています。USB ケーブルで接続してください。' 'It is only seen over Wi-Fi. Connect it with a USB cable.')
    }
    $pnp = Get-UsbApplePhones
    if ($pnp.Count -gt 0) {
        if (@($pnp | Where-Object { $_.ConfigManagerErrorCode -ne 0 }).Count -gt 0) {
            Say-Info (M 'Windows は iPhone を認識していますが、Apple の USB ドライバが動作していません。Apple Devices または iTunes を再インストールしてください。' 'Windows sees the iPhone, but the Apple USB driver is not working. Reinstall Apple Devices or iTunes.')
        } else {
            Say-Info (M 'Windows は iPhone を認識していますが、Apple のサービスはまだ認識していません。' 'Windows sees the iPhone, but the Apple service does not see it yet.')
            Say-Info (M 'iPhone のロックを解除してください。「このコンピュータを信頼しますか？」と表示されたら「信頼」をタップしてください。' 'Unlock the iPhone. If it asks "Trust This Computer?", tap Trust.')
            Say-Info (M 'Apple Devices を使っている場合は、Apple Devices を一度起動してください。' 'If you use Apple Devices, open it once.')
        }
    } else {
        Say-Info (M 'iPhone を USB ケーブルで接続して、ロックを解除してください。充電専用のケーブルでは接続できません。' 'Plug the iPhone in with USB and unlock it. A charge-only cable does not work.')
    }
    if (-not (Ask-Retry)) { Stop-Here }
}

# ==== 5. iPhone のアプリが答えるか ====
while ($true) {
    Say-Step 5 $Total (M 'iPhone のアプリ' 'iPhone app')
    $h = Test-Receiver $Dev.Id 3000
    if ($h.Ok) {
        $mem = $(if ($h.MemMB -gt 0) { (M '・使用可能なメモリ {0:N1} GB' ', memory {0:N1} GB') -f ($h.MemMB / 1024) } else { '' })
        Say-OK ((M 'iPhone のアプリが応答しました：' 'The iPhone app answered: ') + $h.Name + $mem)
        break
    }
    switch ($h.Why) {
        'timeout' {
            Say-NG (M 'iPhone のアプリは起動していますが、他のアプリが iPhone の GPU を使用しています。' 'The iPhone app is open, but another program is using the iPhone GPU.')
            Say-Info (M 'iPhone の GPU は一度に 1 つのアプリしか使用できません。そのアプリを終了するか、iPhone のアプリを終了してから起動し直してください。' 'Only one program at a time can use the iPhone GPU. Close that program, or close and reopen the iPhone app.')
        }
        'result' {
            if ($h.Result -eq -9) {
                Say-NG (M 'iPhone のアプリとこのキットのバージョンが一致しません。' 'The iPhone app does not match this kit.')
                $ipa = $(if ($script:AppVersion) { 'RpcServer-' + $script:AppVersion + '.ipa' } else { 'RpcServer-*.ipa' })
                Say-Info ((M 'このフォルダの {0} をインストールし直してください。' 'Install {0} from this folder again.') -f $ipa)
            } else {
                Say-NG ((M 'iPhone のアプリが拒否しました。コード {0}' 'The iPhone app refused. Code {0}') -f $h.Result)
            }
        }
        default {
            Say-NG (M 'iPhone のアプリが応答しません。' 'The iPhone app does not answer.')
            Say-Info (M 'iPhone で PocketGPU のアプリを起動して、前面に表示したまま、画面をオンにしておいてください。バックグラウンドになると GPU を使用できません。' 'Open the PocketGPU app on the iPhone. Keep it in front with the screen on. In the background it cannot use the GPU.')
            Say-Info (M 'アプリに「開始」ボタンが表示されている場合は、タップしてください。' 'If the app shows a Start button, tap it.')
            Say-Info (M 'まだインストールしていない場合は、このフォルダの RpcServer の .ipa を AltStore または Sideloadly でインストールしてください。手順は README.txt に書いてあります。' 'If it is not installed yet, install the RpcServer .ipa from this folder with AltStore or Sideloadly. See README.txt.')
            Say-Info (M '無料の Apple ID でインストールしたアプリは、7 日で起動できなくなります。その場合は、インストールし直してください。' 'With a free Apple ID the app stops opening after 7 days. Install it again then.')
            if ($Sw.AltServer -and -not $Sw.AltServerRunning) {
                Say-Info (M 'AltServer が起動していません。AltStore でインストールし直すときは、スタートメニューから AltServer を起動してください。' 'AltServer is not running. To reinstall with AltStore, start AltServer from the Start menu.')
            }
        }
    }
    if (-not (Ask-Retry)) { Stop-Here }
}

# ==== 6. USB の太さ ====
while ($true) {
    Say-Step 6 $Total (M 'USB の速度' 'USB speed')
    Out-Line ('      ' + (M '数秒かかります。' 'This takes a few seconds'))
    $bw = Measure-UsbSpeed $Dev.Id 32
    if (-not $bw) { Say-Warn (M '測定できませんでした。アプリのバージョンが古い可能性があります。次へ進みます。' 'Could not measure. The app may be old. Going on.'); break }
    if ($bw.Error) { Say-Warn ((M '測定中に接続が切れました。次へ進みます：' 'The connection dropped while measuring. Going on: ') + $bw.Error); break }
    $line = (M 'iPhone→PC {0:N0} MB/s・PC→iPhone {1:N0} MB/s・往復時間 {2:N2} ms' 'iPhone->PC {0:N0} MB/s, PC->iPhone {1:N0} MB/s, round trip {2:N2} ms') -f $bw.Down, $bw.Up, $bw.RttMs
    if ($bw.Down -lt 45 -or $bw.Up -lt 35) {
        Say-Warn $line
        Say-Info (M 'USB 2 の速度です。負荷の高いゲームは遅くなります。' 'This is USB 2 speed. Heavy games will be slow.')
        Say-Info (M 'USB 3 に対応した USB-C ケーブルを使い、PC の USB-C ポートまたは USB 3 ポートに接続してください。iPhone に付属のケーブルは USB 2 です。' 'Use a USB-C cable that supports USB 3, and plug it into a USB-C or USB 3 port on the PC. The cable that came with the iPhone is USB 2.')
        Say-Info (M 'USB 3 で高速になるのは、iPhone 15 Pro 以降の Pro モデルだけです。' 'Only iPhone 15 Pro and later Pro models get faster with USB 3.')
        if ($script:NoPrompt) { break }
        $a = Read-Host ('      ' + (M 'ケーブルかポートを変更したら、Enter キーを押すと再測定します。このまま進むときは n を入力して Enter キーを押してください。' 'After changing the cable or port, press Enter to measure again. To go on as is, type n and Enter'))
        $script:Log.Add('      -> ' + $a)
        if ($null -eq $a -or $a -eq 'n' -or ++$script:Retries -gt 50) { break }
        continue
    }
    if ($bw.Down -lt 100) {
        Say-OK $line
        Say-Info (M '使用できる速度です。USB 3 のケーブルとポートを使うと、さらに高速になることがあります。' 'This speed works. A USB 3 cable and port may be faster.')
    } else {
        Say-OK $line
    }
    break
}

# ==== 7. ドライバで iPhone の GPU が見えるか ====
Say-Step 7 $Total (M 'ドライバ経由で iPhone の GPU を認識できるか' 'iPhone GPU through the driver')
$lv = Get-VulkanLoaderVersion
if (-not $lv) {
    Say-NG (M 'Vulkan ランタイムの vulkan-1.dll がありません。GPU のドライバを更新してください。' 'The Vulkan runtime vulkan-1.dll is missing. Update the GPU driver.')
    Stop-Here
}
if ($lv -lt [Version]'1.3.234') {
    Say-Warn ((M 'Vulkan ランタイムが古いです。バージョン {0}。GPU のドライバを更新してください。' 'The Vulkan runtime is old: {0}. Update the GPU driver.') -f $lv)
}
if ($Elevated) {
    Say-NG (M '管理者権限で実行されているため、この項目はチェックできません。手順 1 の表示を見てください。' 'Cannot check this while running as administrator. See step 1.')
    Stop-Here
}
$vk = Invoke-VkList
if ($vk.Ok) {
    foreach ($g in $vk.Gpus) { Say-OK ((M 'ドライバ経由で認識できます：' 'Seen through the driver: ') + ($g -replace '^GPU\d+: ', '' -replace '\s+\(vendor .*$', '')) }
} else {
    Say-NG (M 'ドライバ経由で iPhone の GPU を認識できません。' 'The driver does not see the iPhone GPU.')
    if ($vk.Why -eq 'missing') { Say-Info ((M 'vklist.exe がありません。キットをすべて展開し直してください。' 'vklist.exe is missing. Extract the whole kit again.')) }
    elseif ($vk.Why -eq 'timeout') { Say-Info (M '30 秒以内に完了しませんでした。iPhone のアプリを終了してから、起動し直してください。' 'It did not finish in 30 seconds. Close and reopen the iPhone app.') }
    else {
        foreach ($l in @($vk.Output -split "`r?`n" | Where-Object { $_ } | Select-Object -Last 5)) { Say-Info $l }
        Say-Info (M 'ウイルス対策ソフトが iphone_icd.dll をブロックしていないかチェックしてください。' 'Check that antivirus software has not blocked iphone_icd.dll.')
    }
    Stop-Here
}

# ==== 使い方 ====
$steam = '"' + (Join-Path $script:KitDir 'pocketgpu.cmd') + '" %command%'
Out-Line ''
Out-Line (M '準備ができました。' 'Ready.') 'Green'
Out-Line ''
Out-Line (M 'iPhone の GPU で起動するには' 'To run something on the iPhone GPU')
Out-Line (M '・ゲームやアプリの .exe を Run-on-iPhone-GPU.cmd にドラッグ＆ドロップします。Run-on-iPhone-GPU.cmd をダブルクリックすると、.exe を選択できます。' '- Drop the .exe of a game or app onto Run-on-iPhone-GPU.cmd. Double-click it to choose the .exe instead.')
Out-Line (M '・Steam のゲームは、ゲームのプロパティの「起動オプション」に次の 1 行を入力します。' '- For a Steam game, put this line in the game''s Properties > Launch Options:')
Out-Line ('    ' + $steam)
Out-Line (M '・管理者権限で実行されるゲームやランチャーは、iPhone の GPU を認識できません。' '- Games and launchers that run as administrator cannot see the iPhone GPU.')
Out-Line (M '・アンチチートを導入しているゲームでは使用しないでください。アカウントが停止されることがあります。' '- Do not use games with anti-cheat. Your account may be banned.')
Out-Line (M '・使用中は、iPhone のアプリを前面に表示したまま、画面をオンにしておいてください。' '- While you use it, keep the iPhone app in front with the screen on.')
# PC アプリがあることだけを知らせる。開くかは聞かない（同じチェックをもう一度行うアプリなので、ここで続けて開く必要はない）
if (Test-Path -LiteralPath (Join-Path $script:KitDir 'PocketGPU.exe')) {
    Out-Line (M '・このフォルダの PocketGPU.exe を開くと、ゲームの起動・カメラ・マイク・字幕を 1 つのウィンドウで操作できます。' '- Open PocketGPU.exe in this folder to run games and use the camera, microphone, and captions from one window.')
}
Out-Line ''
if (Ask-YesNo (M 'スタートメニューと右クリックメニューの「送る」に「iPhone の GPU で起動」を追加しますか？' 'Add "Run on iPhone GPU" to the Start menu and to the right-click "Send to" menu?')) {
    foreach ($p in (New-PgpuShortcuts)) { Say-OK ((M '作成しました：' 'Created: ') + $p) }
    Say-Info (M 'このフォルダを移動した場合は、もう一度 Start-PocketGPU.cmd を起動して作り直してください。削除するときは Start-PocketGPU.cmd -RemoveShortcuts を実行してください。' 'If you move this folder, open Start-PocketGPU.cmd again to make them again. To remove them: Start-PocketGPU.cmd -RemoveShortcuts')
}
if (Ask-YesNo (M 'Steam の起動オプションの 1 行をコピーしますか？' 'Copy the Steam launch option line?')) {
    Set-Clipboard -Value $steam
    Say-OK (M 'コピーしました。' 'Copied.')
}
if (-not $AllOk) { Finish 2 }
Finish 0
