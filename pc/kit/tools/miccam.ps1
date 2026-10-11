# PocketGPU：iPhone を PC のマイクとウェブカメラだけに使う。iPhone の GPU は使わない。Start-Mic-Camera.cmd から呼ぶ。管理者の権限は要らない。
# 日本語を含むので UTF-8（BOM 付き）で保存する。Windows PowerShell 5.1 で動く形。
#   Start-Mic-Camera.cmd                            カメラとマイクを始める
#   Start-Mic-Camera.cmd -List                      USB でつながっている iPhone の一覧
#   Start-Mic-Camera.cmd -Device 00008110-0012      iPhone が何台かあるとき、UDID（前の方 6 文字以上）か DeviceID で選ぶ
#   -NoMic・-NoCamera・-Speaker（PC の音を iPhone から出す）・-Mic "CABLE Input"（マイクの音を流す出力の機器。省略は VB-CABLE を探す）
#   -Camera auto|back|front|ultrawide|tele・-Quality auto|standard|high|max（auto は iPhone のアプリの画面の設定）
# 動かす物：webcam_rx.exe --usb --shm --nowin --reconnect（DirectShow の仮想カメラへ）と ipaudio.exe --usb --mic --mic-render（仮想ケーブルへ）。
#   どちらも、切れたら 3 秒後に起こし直す。このウィンドウを閉じる・q キーで、両方を止める。
#   2 つはこのウィンドウの console を共にする（窓を閉じれば一緒に終わる。窓を作らない形で起こすと、閉じたあとも残る）。
#   Start-Process の -RedirectStandardError は使わない：この PC で、子の書き出しが 1 行ごとに数秒止まった（0.05 秒の物が 6〜7 秒）。cmd にファイルへ書かせる。
# 仮想カメラ（dshow_cam64.dll・dshow_cam32.dll）は、動かない場所（%LOCALAPPDATA%\PocketGPU\camera）に写してから、いまのユーザー（HKCU）に登録する。聞いて「はい」のときだけ。
# 画面の文は仮。M '日本語' 'English' の組で並べてあるので、言い回しはここで直す。
#
# 試験用の差し替え（pgpu_common.ps1 の物に加えて）：
#   PGPU_MICCAM_BIN=フォルダ      webcam_rx.exe・ipaudio.exe・camera_setup.exe・dshow_cam64/32.dll をここから使う
#   PGPU_CAMERA_DIR=フォルダ      仮想カメラを写して登録する場所（既定は %LOCALAPPDATA%\PocketGPU\camera）
#   PGPU_MICCAM_MICWAV=パス       マイクを出力の機器でなく WAV に書く（機器を鳴らさずに試す）
#   PGPU_MICCAM_LOGDIR=フォルダ   webcam_rx と ipaudio の記録の置き場（既定はキットの logs）
#   PGPU_MICCAM_NOPROBE=1         手順 3 で iPhone のアプリの口をのぞかない（1 回で終わる試験の代役のため）
param(
    [string]$Device = '',
    [switch]$List,
    [switch]$NoMic,
    [switch]$NoCamera,
    [switch]$Speaker,
    [string]$Mic = '',
    [ValidateSet('auto', 'back', 'front', 'ultrawide', 'tele')][string]$Camera = 'auto',
    [ValidateSet('auto', 'standard', 'high', 'max')][string]$Quality = 'auto',
    [int]$Seconds = 0
)
. (Join-Path $PSScriptRoot 'pgpu_common.ps1')

$Total = 4
$CamClsid = '{82069F97-3CA9-4626-AD74-D2AA50BBD618}'
$CatClsid = '{860BB310-5D01-11D0-BD3B-00A0C911CE86}' # Video Capture Sources
$CamName = 'PocketGPU iPhone Camera'
$CameraPort = 50064
$AudioPort = 50063

function Finish([int]$code) {
    $p = Save-Log 'pocketgpu-miccam-log.txt'
    Out-Line ''
    Out-Line ((M 'このときのログ：' 'Log of this run: ') + $p)
    if (-not $script:NoPrompt -and $env:PGPU_NOPAUSE -ne '1') { [void](Read-Host (M 'Enter キーを押すと閉じます' 'Press Enter to close')) }
    exit $code
}
function Stop-Here {
    Out-Line ''
    Out-Line (M 'ここで中断しました。問題を解消してから、もう一度 Start-Mic-Camera.cmd を起動してください。' 'Stopped here. After fixing it, open Start-Mic-Camera.cmd again.') 'Yellow'
    Finish 1
}

# ---- 道具の場所 ----
function Find-Tool([string]$name) {
    $dirs = @()
    if ($env:PGPU_MICCAM_BIN) { $dirs += $env:PGPU_MICCAM_BIN }
    $dirs += (Join-Path $script:KitDir 'extras'), (Join-Path $script:KitDir 'camera'), $script:KitDir
    foreach ($d in $dirs) {
        $p = Join-Path $d $name
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return $null
}

# ---- iPhone の一覧（USB だけ。UDID つき） ----
function Get-MuxPhones {
    $c = Connect-Tcp '127.0.0.1' $script:MuxPort 1500
    if (-not $c) { return @{ Ok = $false; Phones = @() } }
    try { $r = Invoke-Mux $c 'ListDevices' } catch { return @{ Ok = $false; Phones = @() } } finally { $c.Close() }
    $list = @()
    foreach ($d in @($r['DeviceList'])) {
        if ($d -isnot [hashtable]) { continue }
        $p = $d['Properties']
        if ($p -isnot [hashtable]) { continue }
        if ([string]$p['ConnectionType'] -ne 'USB') { continue }
        $id = $(if ($null -ne $p['DeviceID']) { [int64]$p['DeviceID'] } else { [int64]$d['DeviceID'] })
        $list += [pscustomobject]@{ Id = $id; Udid = [string]$p['SerialNumber']; ProductId = $p['ProductID'] }
    }
    return @{ Ok = $true; Phones = $list }
}
# UDID の前の方（大文字・小文字と「-」は見ない。6 文字以上）。webcam_rx・ipaudio の --device と同じ決まり
function Test-UdidMatch([string]$udid, [string]$want) {
    $u = ($udid -replace '-', '').ToUpperInvariant()
    $w = ($want -replace '-', '').ToUpperInvariant()
    return ($w.Length -ge 6) -and $u.StartsWith($w)
}
# 画面とログには UDID の前の 13 文字だけ出す（選ぶにはこれで足りる）
function Format-Phone($p) {
    $u = $(if ($p.Udid.Length -gt 13) { $p.Udid.Substring(0, 13) + '…' } else { $p.Udid })
    return ('UDID {0}・DeviceID {1}' -f $u, $p.Id)
}

# ---- 仮想カメラの登録（HKCU） ----
function Get-CameraRegistration {
    $path = $null
    try { $path = (Get-ItemProperty -LiteralPath "HKCU:\Software\Classes\CLSID\$CamClsid\InprocServer32" -ErrorAction Stop).'(default)' } catch { }
    $inst = Test-Path -LiteralPath "HKCU:\Software\Classes\CLSID\$CatClsid\Instance\$CamClsid"
    return @{ Registered = [bool]($path -and $inst); Path = $path; FileOk = [bool]($path -and (Test-Path -LiteralPath $path)) }
}
function Install-Camera {
    $names = 'camera_setup.exe', 'dshow_cam64.dll', 'dshow_cam32.dll'
    $src = @()
    foreach ($n in $names) {
        $s = Find-Tool $n
        if (-not $s) { Say-NG ((M '仮想カメラのファイル {0} が見つかりません。キットを展開し直してください。' 'The camera file {0} is missing. Extract the kit again.') -f $n); return $false }
        $src += $s
    }
    $dir = $(if ($env:PGPU_CAMERA_DIR) { $env:PGPU_CAMERA_DIR } else { Join-Path $env:LOCALAPPDATA 'PocketGPU\camera' })
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($s in $src) {
        $d = Join-Path $dir (Split-Path -Leaf $s)
        if ((Test-Path -LiteralPath $d) -and (Get-FileHash -LiteralPath $d).Hash -eq (Get-FileHash -LiteralPath $s).Hash) { continue }
        try { Copy-Item -LiteralPath $s -Destination $d -Force }
        catch {
            Say-NG (M 'カメラのファイルを更新できませんでした。カメラを使っているアプリをすべて終了してから、もう一度試してください。' 'Could not update the camera files. Quit every app that uses the camera, then try again.')
            return $false
        }
    }
    & (Join-Path $dir 'camera_setup.exe') install | Out-Null
    $ok = $LASTEXITCODE -eq 0 -and (Get-CameraRegistration).Registered
    if ($ok) { Say-OK ((M '「{0}」をカメラの一覧に追加しました。カメラを使うアプリを開いている場合は、開き直してください。' 'Added "{0}" to the camera list. If an app that uses the camera is open, restart it.') -f $CamName) }
    else { Say-NG (M 'カメラを登録できませんでした。' 'Could not register the camera.') }
    return $ok
}

# ---- マイクの音を流す出力の機器（仮想ケーブル） ----
function Invoke-Quiet([string]$exe, [string]$argLine) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = $argLine
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $p = [Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync()
    $e = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(20000)) { try { $p.Kill() } catch { }; return '' }
    return $o.Result + $e.Result
}
# ipaudio --list の「出力 [動作中] CABLE Input (VB-Audio Virtual Cable)  48000Hz ...」から、使える出力の機器の名前を探す
function Find-CableDevice([string]$ipaudio) {
    $text = Invoke-Quiet $ipaudio '--list'
    foreach ($line in ($text -split "`r?`n")) {
        if ($line -match '^出力\S*\s+\[動作中\]\s+(.+?)\s{2,}' -and $Matches[1] -like 'CABLE Input*') { return 'CABLE Input' }
    }
    return $null
}

# ---- 子のプロセス（このウィンドウの console を共にして、cmd にログを書かせる） ----
function Quote-Arg([string]$a) { if ($a -match '[\s"]') { return '"' + ($a -replace '"', '') + '"' } else { return $a } }
function Start-Child($c) {
    $line = '"' + $c.Exe + '" ' + (($c.Args | ForEach-Object { Quote-Arg $_ }) -join ' ') + ' 1>>"' + $c.Log + '" 2>&1'
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $env:ComSpec
    $psi.Arguments = '/d /s /c "' + $line + '"'
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $script:KitDir
    $c.Proc = [Diagnostics.Process]::Start($psi)
    $c.Starts++
}
function Stop-Child($c) {
    if (-not $c.Proc) { return }
    # cmd の子（webcam_rx・ipaudio）を PID で止めてから cmd を止める
    foreach ($x in @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($c.Proc.Id)" -ErrorAction SilentlyContinue)) {
        try { Stop-Process -Id $x.ProcessId -Force -ErrorAction Stop } catch { }
    }
    if (-not $c.Proc.HasExited) { try { Stop-Process -Id $c.Proc.Id -Force -ErrorAction Stop } catch { } }
}
# ログの新しい行を読む（相手が書いている最中でも読めるように共有で開く）
function Read-NewLines($c) {
    if (-not (Test-Path -LiteralPath $c.Log)) { return @() }
    $fs = [IO.File]::Open($c.Log, 'Open', 'Read', 'ReadWrite')
    try {
        if ($fs.Length -le $c.Pos) { return @() }
        $fs.Position = $c.Pos
        $buf = New-Object byte[] ($fs.Length - $c.Pos)
        $n = $fs.Read($buf, 0, $buf.Length)
        $last = [Array]::LastIndexOf($buf, [byte]10, $n - 1)
        if ($last -lt 0) { return @() }
        $c.Pos += $last + 1
        return @([Text.Encoding]::UTF8.GetString($buf, 0, $last + 1) -split "`r?`n" | Where-Object { $_ })
    } finally { $fs.Close() }
}

$CamNames = @{ '0' = (M '背面カメラ' 'back camera'); '1' = (M '前面カメラ' 'front camera'); '2' = (M '超広角' 'ultra wide'); '3' = (M '望遠' 'telephoto') }
# webcam_rx・ipaudio のログの行を、画面に出す文にする（出さない行は $null）
function Convert-CameraLine([string]$l) {
    if ($l -match '(決まった形|形が変わった)：(\d+) x (\d+)・(\d+) コマ/秒・(\d+) kbps・カメラ (\d)') {
        $what = $(if ($Matches[1] -eq '決まった形') { M '接続中です' 'Connected' } else { M '切り替わりました' 'Switched' })
        return ((M 'カメラ：{0}。{1}×{2}・{3} コマ/秒・{4}・{5} Mbps' 'Camera: {0}. {1}x{2}, {3} fps, {4}, {5} Mbps') -f $what, $Matches[2], $Matches[3], $Matches[4], $CamNames[$Matches[6]], [Math]::Round([int]$Matches[5] / 1000, 1))
    }
    if ($l -match 'iPhone を待っています|つなげない') { return (M 'カメラ：iPhone の接続を待っています。iPhone のアプリで「ウェブカメラとして使う」をオンにして、アプリを前面に表示してください。' 'Camera: waiting for the iPhone. In the iPhone app, turn on "Use this iPhone as a webcam" and keep the app in front.') }
    if ($l -match 'iPhone との接続が切れた') { return (M 'カメラ：iPhone との接続が切れました。再接続を待っています。' 'Camera: the iPhone disconnected. Waiting to reconnect.') }
    if ($l -match 'iPhone が断った（status (\d+)）') {
        switch ($Matches[1]) {
            '1' { return (M 'カメラ：iPhone のカメラへのアクセスが許可されていません。iPhone の設定アプリで許可してください。' 'Camera: camera access is not allowed. Allow it in the iPhone Settings app.') }
            '3' { return (M 'カメラ：iPhone のアプリを前面に表示してください。iOS はバックグラウンドのアプリにカメラを使わせません。' 'Camera: bring the iPhone app to the front. iOS does not let apps in the background use the camera.') }
            default { return ((M 'カメラ：iPhone が開始できませんでした。コード {0}' 'Camera: the iPhone could not start. Code {0}') -f $Matches[1]) }
        }
    }
    if ($l -match 'iPhone に頼み直す：(\d+) x (\d+)・(\d+) コマ/秒') { return ((M 'カメラ：カメラを使うアプリが {0}×{1}・{2} コマ/秒を選びました。iPhone に切り替えを依頼します。' 'Camera: an app chose {0}x{1}, {2} fps. Asking the iPhone to switch.') -f $Matches[1], $Matches[2], $Matches[3]) }
    if ($l -match 'iPhone に頼み直す：大きさはおまかせ') { return (M 'カメラ：カメラを使うアプリが終了しました。iPhone のアプリの設定に戻します。' 'Camera: the app that chose the format closed. Going back to the iPhone app settings.') }
    return $null
}
function Convert-AudioLine([string]$l) {
    if ($l -match '^つながった。iPhone：hw_rate=(\d+).*speaker=(\w+) mic=(\w+)') {
        $k = [Math]::Round([int]$Matches[1] / 1000, 1)
        if ($Matches[3] -eq 'ok') { return ((M 'マイク：接続中です。iPhone のマイクを {0} kHz で取り込んでいます。' 'Microphone: connected. Recording the iPhone microphone at {0} kHz.') -f $k) }
        return ((M 'スピーカー：接続中です。{0} kHz' 'Speaker: connected. {0} kHz') -f $k)
    }
    if ($l -match 'マイクはアプリを前に') { return (M 'マイク：iPhone のアプリを前面に表示すると開始します。' 'Microphone: it starts when the iPhone app is in front.') }
    return $null
}

# ==================================================================== ここから
Out-Line (M 'iPhone をマイクとカメラとして使います' 'Use the iPhone as a microphone and a camera')
Out-Line (M 'iPhone の GPU は使いません。管理者権限は必要ありません。' 'The iPhone GPU is not used. No administrator rights are needed.')

$wantCamera = -not $NoCamera
$wantMic = -not $NoMic
if (-not $wantCamera -and -not $wantMic -and -not $Speaker) {
    Say-NG (M 'カメラもマイクもスピーカーも使わない設定になっています。' 'Camera, microphone, and speaker are all turned off.')
    Finish 2
}
$Rx = Find-Tool 'webcam_rx.exe'
$Ipa = Find-Tool 'ipaudio.exe'
if ($wantCamera -and -not $Rx) { Say-NG (M 'webcam_rx.exe が見つかりません。キットを展開し直してください。' 'webcam_rx.exe is missing. Extract the kit again.'); Stop-Here }
if (($wantMic -or $Speaker) -and -not $Ipa) { Say-NG (M 'ipaudio.exe が見つかりません。キットを展開し直してください。' 'ipaudio.exe is missing. Extract the kit again.'); Stop-Here }

# ==== 1. Apple のサービス ====
while ($true) {
    Say-Step 1 $Total (M 'Apple のサービス' 'Apple service')
    if (Test-MuxListening) { Say-OK (M 'Apple のサービスが実行されています。' 'The Apple service is running.'); break }
    Say-NG (M 'Apple のサービスが実行されていません。' 'The Apple service is not running.')
    Say-Info (M 'iPhone を USB ケーブルで接続してから、Apple Devices または iTunes を起動してください。インストールされていないときは、Start-PocketGPU.cmd で手順を確認できます。' 'Plug the iPhone in with USB, then open Apple Devices or iTunes. If neither is installed, Start-PocketGPU.cmd shows how.')
    if (-not (Ask-Retry)) { Stop-Here }
}

# ==== 2. iPhone ====
$Phone = $null
while ($true) {
    Say-Step 2 $Total 'iPhone'
    $r = Get-MuxPhones
    $phones = @($r.Phones)
    if ($List) {
        if ($phones.Count -eq 0) { Out-Line ('      ' + (M 'USB で接続されている iPhone はありません。' 'No iPhone is connected over USB.')) }
        foreach ($p in $phones) { Out-Line ('      ' + 'UDID ' + $p.Udid + '・DeviceID ' + $p.Id) }
        Finish 0
    }
    if ($phones.Count -eq 0) {
        Say-NG (M 'USB で接続されている iPhone が見つかりません。' 'No iPhone is connected over USB.')
        Say-Info (M 'iPhone を USB ケーブルで接続して、ロックを解除してください。「このコンピュータを信頼しますか？」と表示されたら「信頼」をタップしてください。' 'Plug the iPhone in with USB and unlock it. If it asks "Trust This Computer?", tap Trust.')
        if (-not (Ask-Retry)) { Stop-Here }
        continue
    }
    if ($Device) {
        $hit = @($(if ($Device -match '^\d+$') { $phones | Where-Object { $_.Id -eq [int64]$Device } } else { $phones | Where-Object { Test-UdidMatch $_.Udid $Device } }))
        if ($hit.Count -eq 0) {
            Say-NG ((M '指定された iPhone「{0}」が見つかりません。接続されている iPhone：' 'The iPhone "{0}" was not found. Connected iPhones:') -f $Device)
            foreach ($p in $phones) { Say-Info (Format-Phone $p) }
            if (-not (Ask-Retry)) { Stop-Here }
            continue
        }
        $Phone = $hit[0]
    } elseif ($phones.Count -eq 1) {
        $Phone = $phones[0]
    } else {
        Say-Info ((M '{0} 台の iPhone が接続されています。' '{0} iPhones are connected.') -f $phones.Count)
        for ($i = 0; $i -lt $phones.Count; $i++) { Say-Info ('{0}. {1}' -f ($i + 1), (Format-Phone $phones[$i])) }
        if ($script:NoPrompt) { $Phone = $phones[0]; Say-Warn (M '1 番目の iPhone を使います。選ぶときは -Device に UDID を指定してください。' 'Using the first iPhone. To choose, pass its UDID with -Device.') }
        else {
            $a = Read-Host ('      ' + (M '使う iPhone の番号を入力して Enter キーを押してください' 'Type the number of the iPhone to use and press Enter'))
            $script:Log.Add('      -> ' + $a)
            if ($a -match '^\d+$' -and [int]$a -ge 1 -and [int]$a -le $phones.Count) { $Phone = $phones[[int]$a - 1] } else { Say-Warn (M '番号が正しくありません。1 番目の iPhone を使います。' 'Not a valid number. Using the first iPhone.'); $Phone = $phones[0] }
        }
    }
    Say-OK ((M '使用する iPhone：{0}' 'iPhone to use: {0}') -f (Format-Phone $Phone))
    break
}
$DevArg = $(if ($Phone.Udid) { $Phone.Udid } else { [string]$Phone.Id })

# ==== 3. iPhone のアプリ ====
Say-Step 3 $Total (M 'iPhone のアプリ' 'iPhone app')
$probe = $env:PGPU_MICCAM_NOPROBE -ne '1'
if ($wantCamera -and $probe) {
    if ((Connect-Phone $Phone.Id $CameraPort).Ok) { Say-OK (M 'iPhone のアプリでウェブカメラがオンになっています。' 'The webcam is on in the iPhone app.') }
    else { Say-Warn (M 'iPhone のアプリで「ウェブカメラとして使う」をオンにしてください。オンになると自動で接続します。' 'Turn on "Use this iPhone as a webcam" in the iPhone app. It connects automatically when it is on.') }
}
if (($wantMic -or $Speaker) -and $probe) {
    if ((Connect-Phone $Phone.Id $AudioPort).Ok) { Say-OK (M 'iPhone のアプリでスピーカーとマイクがオンになっています。' 'Speaker and mic are on in the iPhone app.') }
    else { Say-Warn (M 'iPhone のアプリで「PC のスピーカーとマイクとして使う」をオンにしてください。オンになると自動で接続します。' 'Turn on "Use as PC speaker and mic" in the iPhone app. It connects automatically when it is on.') }
}
Say-Info (M 'GPU を使わない場合は、iPhone のアプリで「GPU を使わずにマイクとカメラだけ使う」をオンにしてください。' 'If you do not use the GPU, turn on "Use only the mic and camera, not the GPU" in the iPhone app.')

# ==== 4. PC のカメラとマイク ====
Say-Step 4 $Total (M 'PC のカメラとマイク' 'Camera and microphone on this PC')
if ($wantCamera) {
    $reg = Get-CameraRegistration
    if ($reg.Registered -and $reg.FileOk) { Say-OK ((M '「{0}」がカメラの一覧に登録されています。' '"{0}" is in the camera list.') -f $CamName) }
    else {
        if ($reg.Registered) { Say-Warn (M 'カメラは登録されていますが、ファイルが見つかりません。登録し直します。' 'The camera is registered, but its file is missing. It will be registered again.') }
        else { Say-Warn ((M '「{0}」はまだカメラの一覧にありません。' '"{0}" is not in the camera list yet.') -f $CamName) }
        Say-Info (M 'このユーザーの設定にだけ登録します。管理者権限は必要ありません。Windows のカメラアプリには表示されませんが、Zoom・Discord・OBS・ブラウザなどで選べます。' 'It is added for this user only, with no administrator rights. It does not show in the Windows Camera app, but Zoom, Discord, OBS, and browsers can use it.')
        if (Ask-YesNo (M 'カメラを登録しますか？' 'Register the camera?')) { [void](Install-Camera) }
        else { Say-Warn (M 'カメラを登録しないため、ほかのアプリからは選べません。' 'The camera is not registered, so other apps cannot select it.') }
    }
    if (Test-Elevated) { Say-Warn (M 'このウィンドウは管理者権限で実行されています。管理者権限で実行されるアプリには、このカメラは表示されません。' 'This window runs as administrator. Apps that run as administrator do not see this camera.') }
}
$MicArgs = @()
if ($wantMic) {
    if ($env:PGPU_MICCAM_MICWAV) { $MicArgs = @('--mic', '--mic-wav', $env:PGPU_MICCAM_MICWAV) }
    else {
        $dev = $(if ($Mic) { $Mic } else { Find-CableDevice $Ipa })
        if ($dev) {
            $MicArgs = @('--mic', '--mic-render', $dev)
            Say-OK ((M 'iPhone のマイクの音を「{0}」に流します。通話アプリのマイクには「CABLE Output」を選んでください。' 'The iPhone microphone goes to "{0}". In your call app, choose "CABLE Output" as the microphone.') -f $dev)
        } else {
            Say-Warn (M 'マイクとして使うには、仮想オーディオケーブルが必要です。VB-CABLE が見つかりません。' 'To use it as a microphone, a virtual audio cable is needed. VB-CABLE was not found.')
            Say-Info (M 'VB-CABLE を vb-audio.com/Cable からインストールして、PC を再起動してから、もう一度起動してください。今回はマイクを使わずに続けます。' 'Install VB-CABLE from vb-audio.com/Cable, restart the PC, and open this again. This time it continues without the microphone.')
            $wantMic = $false
        }
    }
}

# ==== 始める ====
$logDir = $(if ($env:PGPU_MICCAM_LOGDIR) { $env:PGPU_MICCAM_LOGDIR } else { Join-Path $script:KitDir 'logs' })
New-Item -ItemType Directory -Force $logDir | Out-Null
$muxArgs = $(if ($env:IPHONE_VK_MUX) { @('--mux', $env:IPHONE_VK_MUX) } else { @() })
$children = @()
if ($wantCamera) {
    $a = @('--usb', '--device', $DevArg) + $muxArgs + @('--shm', '--nowin', '--reconnect')
    if ($Camera -ne 'auto') { $a += @('--camera', $Camera) }
    if ($Quality -ne 'auto') { $a += @('--quality', $Quality) }
    $children += @{ Name = 'camera'; Exe = $Rx; Args = $a; Log = (Join-Path $logDir 'webcam_rx.txt'); Proc = $null; Starts = 0; Pos = 0; Next = [DateTime]::MinValue }
}
if ($wantMic -or $Speaker) {
    $a = @('--usb', '--device', $DevArg) + $muxArgs
    if ($Speaker) { $a += '--speaker' }
    $a += $MicArgs
    $children += @{ Name = 'audio'; Exe = $Ipa; Args = $a; Log = (Join-Path $logDir 'ipaudio.txt'); Proc = $null; Starts = 0; Pos = 0; Next = [DateTime]::MinValue }
}
foreach ($c in $children) { if (Test-Path -LiteralPath $c.Log) { Remove-Item -LiteralPath $c.Log -ErrorAction SilentlyContinue } }

Out-Line ''
$running = @()
if ($wantCamera) { $running += (M 'カメラ' 'camera') }
if ($wantMic) { $running += (M 'マイク' 'microphone') }
if ($Speaker) { $running += (M 'スピーカー' 'speaker') }
Out-Line ((M '開始します：{0}。GPU は使いません。' 'Starting: {0}. The GPU is not used.') -f ($running -join (M '・' ', '))) 'Green'
if ($Seconds -le 0 -and -not $script:NoPrompt) { Out-Line (M '終了するには、このウィンドウで q キーを押してください。' 'To stop, press q in this window.') }

$t0 = [DateTime]::Now
try {
    while ($true) {
        $now = [DateTime]::Now
        foreach ($c in $children) {
            if ($c.Proc -and $c.Proc.HasExited) {
                $c.Proc = $null
                $c.Next = $now.AddSeconds(3)
                if ($c.Name -eq 'audio') { Out-Line (M 'マイク：iPhone との接続が切れました。3 秒後に再接続します。' 'Microphone: disconnected from the iPhone. Reconnecting in 3 seconds.') }
                else { Out-Line (M 'カメラ：停止しました。3 秒後に再開します。' 'Camera: stopped. Restarting in 3 seconds.') }
            }
            if (-not $c.Proc -and $now -ge $c.Next) { Start-Child $c }
            foreach ($l in (Read-NewLines $c)) {
                $msg = $(if ($c.Name -eq 'camera') { Convert-CameraLine $l } else { Convert-AudioLine $l })
                if ($msg) { Out-Line ('  ' + $msg) }
            }
        }
        if ($Seconds -gt 0 -and ($now - $t0).TotalSeconds -ge $Seconds) { break }
        if ($Seconds -le 0 -and -not $script:NoPrompt) {
            $quit = $false
            try { while ([Console]::KeyAvailable) { $k = [Console]::ReadKey($true); if ($k.KeyChar -eq 'q' -or $k.KeyChar -eq 'Q') { $quit = $true } } } catch { }
            if ($quit) { break }
        }
        Start-Sleep -Milliseconds 500
    }
} finally {
    foreach ($c in $children) { Stop-Child $c }
}
Out-Line ''
Out-Line (M 'カメラとマイクを停止しました。' 'Stopped the camera and the microphone.')
Finish 0
