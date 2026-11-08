# PocketGPU（Android）：Android の記録を 1 つの zip にまとめる。Send-Android-Log.cmd から呼ぶ。Windows PowerShell 5.1 だけで動く（Python は要らない）。
# 日本語を含むので UTF-8（BOM 付き）で保存する。BOM が無いと 5.1 は Shift_JIS として読んで壊れる。
# 画面の文は仮。M '日本語' 'English' の組で並べてあるので、言い回しはここで直す。ファイルの中は英語だけ。
#
# 集める物（adb で、下の決まった命令だけを打つ。getprop の全部・dumpsys の全部・bugreport は使わない）
#   receiver-log.txt   adb shell cat /data/local/tmp/pgpu_server.log                     キットの受け手の記録
#   logcat-crash.txt   adb logcat -d -b crash                                            Android の落ちた記録。既定は PocketGPU の受け手の行だけ
#   logcat-app.txt     adb logcat -d -s iphone-rpc-vk                                    アプリの受け手の記録（アプリが入っているときだけ）
#   info.txt           機種・Android の版・チップ・GPU の名前・アプリの版・キットの版・Windows の版・PC 側の口・adb forward の一覧
#     機種など：adb shell getprop ro.product.model / ro.product.manufacturer / ro.build.version.release / ro.build.version.sdk / ro.product.cpu.abi / ro.soc.model / ro.board.platform
#     GPU：受け手の記録の「Receiver GPU:」の行と、adb shell dumpsys SurfaceFlinger の「GLES:」の行（この 1 行だけを残す）
#     アプリの版：adb shell dumpsys package com.tsurutanmen.pocketgpu の versionName（この 1 行だけを残す）
# 集めない物：電話番号・IMEI・Google アカウント・端末のシリアル番号・tools\update.ini（合言葉）・キットの印・ほかのアプリの記録（全部を送ると答えたときの落ちた記録を除く）
# 伏せる物：PC のユーザー名・PC 名・プロフィールのフォルダ名・adb のシリアル番号・メールアドレス。report.ps1 と同じ「(hidden)」に直す（全部のファイルに最後にまとめて）
#
# 落ちた記録に PocketGPU 以外のアプリの行があるときだけ、全部を送るか y/N で聞く（既定 N = PocketGPU の受け手の行だけ）
# 終わりのコード：0 zip を作った・1 adb が無い/スマートフォンが無い/許可がまだ（.cmd が android\_common.cmd で見て止める）・2 zip を作れなかった
#
# 試す人は使わない、試験用の差し替え（pgpu_common.ps1 の物に加えて）：
#   PGPU_LOG_OUT=フォルダ          フォルダと zip をキットのフォルダの代わりにここへ作る
#   PGPU_TEST_LAUNCH_LOG=パス      エクスプローラーを開く代わりに、開くはずだった物をこのファイルに書く
. (Join-Path $PSScriptRoot 'pgpu_common.ps1')

$script:PgpuApp = 'com.tsurutanmen.pocketgpu'
$script:MaxLines = 4000
$script:Adb = $env:ADB
$script:Serial = $env:ANDROID_SERIAL
# 思わぬ誤りで止まるときも、短い 1 行を出して終わりのコード 2 で終わる
trap { Write-Host ('ERROR: ' + $_.Exception.Message) -ForegroundColor Red; exit 2 }

# ---- 小さな道具 ----
function Convert-Eol([string]$s) { if (-not $s) { return '' }; return ($s -replace "`r+`n", "`n" -replace "`r", "`n") }
# adb を窓を出さずに動かす。標準入力は閉じる（adb が、このウィンドウの入力を読んで取らないように）
function Invoke-Adb([string]$arguments, [int]$timeoutMs = 30000) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $script:Adb
    $psi.Arguments = $arguments
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    try { $p = [Diagnostics.Process]::Start($psi) } catch { return @{ Ok = $false; Exit = -1; Out = ''; Err = $_.Exception.Message; TimedOut = $false } }
    try { $p.StandardInput.Close() } catch { }
    $o = $p.StandardOutput.ReadToEndAsync()
    $er = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($timeoutMs)) { try { $p.Kill() } catch { }; return @{ Ok = $false; Exit = -1; Out = ''; Err = 'timeout'; TimedOut = $true } }
    [void]$p.WaitForExit()
    return @{ Ok = ($p.ExitCode -eq 0); Exit = $p.ExitCode; Out = (Convert-Eol $o.Result); Err = (Convert-Eol $er.Result); TimedOut = $false }
}
function Get-Prop([string]$name) {
    $r = Invoke-Adb ('shell getprop ' + $name)
    if (-not $r.Ok) { return '' }
    return (($r.Out -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 1) + '').Trim()
}
function Format-LineCount([int]$n) {
    if ($script:Ja) { return ([string]$n + ' 行') }
    if ($n -eq 1) { return '1 line' }
    return ([string]$n + ' lines')
}
function Get-Lines([string]$text) {
    if (-not $text) { return @() }
    $l = @($text -split "`n")
    $n = $l.Count
    while ($n -gt 0 -and $l[$n - 1].Trim() -eq '') { $n-- }
    return @($l | Select-Object -First $n)
}
# 長い記録は後ろの $script:MaxLines 行だけ残す。省いたときは先頭にそう書く
function Limit-Lines([string[]]$lines) {
    $lines = @($lines)
    while ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines = @($lines | Select-Object -First ($lines.Count - 1)) }
    if ($lines.Count -le $script:MaxLines) { return $lines }
    $cut = $lines.Count - $script:MaxLines
    return @('(' + $cut + ' older lines left out)') + @($lines | Select-Object -Last $script:MaxLines)
}

# ---- 伏せる（最後に全部のファイルへ）----
# PC のユーザー名・プロフィールのフォルダ名・PC 名・adb のシリアル番号・メールアドレスを直す。
# 3 字以上の名前は、前後が英数字でなければどこでも。短い名前は、Users\ の次の場所だけ（短い名前を全部直すと、記録の文字が壊れるため）
function Get-HideNames {
    $names = New-Object System.Collections.Generic.List[string]
    $cand = @($env:USERNAME, $env:COMPUTERNAME)
    if ($env:USERPROFILE) { $cand += (Split-Path -Leaf $env:USERPROFILE) }
    foreach ($x in $cand) { if ($x -and $x.Trim() -and -not $names.Contains($x)) { $names.Add($x) } }
    return @($names)
}
function Hide-Private([string]$text) {
    if (-not $text) { return '' }
    foreach ($n in (Get-HideNames)) {
        $e = [regex]::Escape($n)
        $text = [regex]::Replace($text, '(?i)(?<=[\\/]Users[\\/])' + $e + '(?![A-Za-z0-9])', '(hidden)')
        if ($n.Length -ge 3) { $text = [regex]::Replace($text, '(?i)(?<![A-Za-z0-9])' + $e + '(?![A-Za-z0-9])', '(hidden)') }
    }
    if ($script:Serial -and $script:Serial.Length -ge 4) {
        $e = [regex]::Escape($script:Serial)
        $text = [regex]::Replace($text, '(?i)(?<![A-Za-z0-9])' + $e + '(?![A-Za-z0-9])', '(serial)')
    }
    # 最後の部分が英字 2 字以上のときだけ（GPU の版の V@0615.0 や GIT@abc は、メールアドレスではないので残す）
    $text = [regex]::Replace($text, '[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}(?![A-Za-z0-9])', '(email)')
    return $text
}

# ---- 落ちた記録を、PocketGPU の受け手の分だけに絞る ----
# logcat の行は、同じ（PID・タグ）が続く間を 1 つのまとまりにする。「*** *** ***」の行と「FATAL EXCEPTION」の行は、新しいまとまりの始まり。
# まとまりの中のどこかに pgpu_server・libpgpu_server.so・PocketGPU のアプリ名があれば、そのまとまりを残す
function Get-LineKey([string]$l) {
    if ($l -cmatch '^\d\d-\d\d\s+\d\d:\d\d:\d\d\.\d+\s+(\d+)\s+\d+\s+[VDIWEFS]\s+(.+?)\s*:(\s|$)') { return $Matches[1] + '|' + $Matches[2] }
    if ($l -cmatch '^[VDIWEFS]/(.+?)\s*\(\s*(\d+)\)\s*:') { return $Matches[2] + '|' + $Matches[1] }
    return ''
}
function Split-CrashBlocks([string[]]$lines) {
    $groups = New-Object System.Collections.Generic.List[object]
    $cur = $null
    $curKey = ''
    foreach ($l in @($lines)) {
        if ($l -match '^-{5,}\s*beginning of') { continue }
        $key = Get-LineKey $l
        $banner = ($l -match '\*\*\* \*\*\* \*\*\*') -or ($l -match 'FATAL EXCEPTION')
        if ($null -eq $cur -or $banner -or ($key -and $curKey -and $key -ne $curKey)) {
            $cur = New-Object System.Collections.Generic.List[string]
            $groups.Add($cur)
            $curKey = $key
        } elseif ($key -and -not $curKey) { $curKey = $key }
        $cur.Add($l)
    }
    $blocks = @()
    foreach ($g in $groups) {
        $t = ($g -join "`n")
        if ($t.Trim()) { $blocks += $t }
    }
    return $blocks
}
function Test-PgpuBlock([string]$block) {
    return [bool]($block -match 'pgpu_server|libpgpu_server\.so|com\.tsurutanmen\.pocketgpu')
}
function Select-PgpuCrash([string[]]$blocks) {
    return @($blocks | Where-Object { Test-PgpuBlock $_ })
}

# ---- PC の様子 ----
function Get-WindowsText {
    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction SilentlyContinue
    if (-not $cv) { return 'not available' }
    $name = $cv.ProductName
    if ([int]$cv.CurrentBuild -ge 22000) { $name = $name -replace 'Windows 10', 'Windows 11' }
    $build = '' + $cv.CurrentBuild
    if ($cv.UBR) { $build = $build + '.' + $cv.UBR }
    return ($name + ' ' + $cv.DisplayVersion + ' (build ' + $build + ', ' + $env:PROCESSOR_ARCHITECTURE + ')').Replace('  ', ' ')
}
function Get-KitIniText {
    $f = Join-Path $PSScriptRoot 'kit.ini'
    if (-not (Test-Path -LiteralPath $f)) { return @('(no tools\kit.ini)') }
    $l = @()
    foreach ($x in [IO.File]::ReadAllLines($f)) { if ($x -match '^(proto|app)=') { $l += $x.Trim() } }
    if ($l.Count -eq 0) { return @('(empty)') }
    return $l
}
function Get-PortFileText {
    $f = Join-Path (Join-Path $script:KitDir 'android') 'port.txt'
    if (-not (Test-Path -LiteralPath $f)) { return @('(none: default ports)') }
    $l = @()
    foreach ($x in [IO.File]::ReadAllLines($f)) { if ($x -match '^\s*(gpu|cam|audio)\s*=\s*\d{1,5}\s*$') { $l += $x.Trim() } }
    if ($l.Count -eq 0) { return @('(empty)') }
    return $l
}

# ---- 出す所・zip・エクスプローラー ----
function Get-OutRoot {
    $cands = @()
    if ($env:PGPU_LOG_OUT) { $cands += $env:PGPU_LOG_OUT }
    $cands += $script:KitDir
    try { $cands += [Environment]::GetFolderPath('Desktop') } catch { }
    $cands += [IO.Path]::GetTempPath()
    foreach ($c in $cands) {
        if (-not $c) { continue }
        try {
            if (-not (Test-Path -LiteralPath $c)) { [void](New-Item -ItemType Directory -Path $c -Force) }
            $probe = Join-Path $c ('.pgpu-write-test-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
            [IO.File]::WriteAllText($probe, 'x')
            Remove-Item -LiteralPath $probe -Force
            return $c
        } catch { }
    }
    return $script:KitDir
}
function Write-LogFile([string]$dir, [string]$name, [string]$text) {
    $t = (Hide-Private $text) -replace "`r`n", "`n" -replace "`r", "`n"
    $t = $t -replace "`n", "`r`n"
    if (-not $t.EndsWith("`r`n")) { $t += "`r`n" }
    [IO.File]::WriteAllText((Join-Path $dir $name), $t, (New-Object Text.UTF8Encoding($true)))
}
function New-LogZip([string]$dir, [string]$zip) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    $base = Split-Path -Leaf $dir
    $fs = [IO.File]::Create($zip)
    try {
        $za = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($f in (Get-ChildItem -LiteralPath $dir -File)) {
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, ($base + '/' + $f.Name), [IO.Compression.CompressionLevel]::Optimal)
            }
        } finally { $za.Dispose() }
    } finally { $fs.Dispose() }
}
function Invoke-Launch([string]$kind, [string]$target) {
    if ($env:PGPU_TEST_LAUNCH_LOG) { Add-Content -LiteralPath $env:PGPU_TEST_LAUNCH_LOG -Value ($kind + ' ' + $target); return $true }
    try {
        if ($kind -eq 'folder') { Start-Process -FilePath 'explorer.exe' -ArgumentList ('/select,"' + $target + '"') }
        else { Start-Process -FilePath $target }
        return $true
    } catch { Say-Warn ((M '開けませんでした：' 'Could not open it: ') + $_.Exception.Message); return $false }
}

# =====================================================================
# ---- 流れ / Main ----
# =====================================================================
if (-not $script:Adb -or -not (Test-Path -LiteralPath $script:Adb)) {
    Out-Line ''
    Out-Line 'ERROR: adb was not found. Run Send-Android-Log.cmd (it finds adb and checks the phone first).' 'Red'
    exit 1
}

Out-Line ''
Out-Line (M 'PocketGPU Android の記録をまとめます' 'Collect the PocketGPU Android log')

# ---- 1. スマートフォン ----
Say-Step 1 3 (M 'スマートフォンをチェック' 'Check the phone')
$model = Get-Prop 'ro.product.model'
$maker = Get-Prop 'ro.product.manufacturer'
$release = Get-Prop 'ro.build.version.release'
$sdk = Get-Prop 'ro.build.version.sdk'
$abi = Get-Prop 'ro.product.cpu.abi'
$soc = Get-Prop 'ro.soc.model'
if (-not $soc) { $soc = Get-Prop 'ro.board.platform' }

$gles = ''
$sf = Invoke-Adb 'shell dumpsys SurfaceFlinger' 60000
foreach ($l in (Get-Lines $sf.Out)) { if ($l -cmatch '^\s*GLES:\s*(.+?)\s*$') { $gles = $Matches[1]; break } }

$pk = Invoke-Adb ('shell dumpsys package ' + $script:PgpuApp)
$appVer = ''
if ($pk.Out -cmatch '(?m)^\s*versionName=(\S+)') { $appVer = $Matches[1] }

$pr = Invoke-Adb 'shell pidof pgpu_server'
$running = $pr.Ok -and [bool]($pr.Out.Trim())

$rl = Invoke-Adb 'shell cat /data/local/tmp/pgpu_server.log' 60000
$recvMissing = (-not $rl.Ok) -or ($rl.Out -match 'No such file or directory')
$recvLines = $(if ($recvMissing) { @() } else { Get-Lines $rl.Out })
$recvGpu = ''
foreach ($l in $recvLines) { if ($l -cmatch '(?:Receiver GPU|受け手の GPU):\s*(.+?)\s*$') { $recvGpu = $Matches[1]; break } }

Say-Info ((M '機種：' 'Model: ') + $(if ($model) { $model } else { '-' }))
Say-Info ('Android: ' + $(if ($release) { $release } else { '-' }) + $(if ($sdk) { ' (SDK ' + $sdk + ')' } else { '' }))
$gpuShown = $(if ($recvGpu) { $recvGpu } elseif ($gles) { $gles } else { '-' })
Say-Info ('GPU: ' + $gpuShown)
Say-Info ((M 'アプリ：' 'App: ') + $(if ($appVer) { $appVer } else { M '入っていません' 'not installed' }))
Say-Info ((M '受け手：' 'Receiver: ') + $(if ($running) { M '動いています' 'running' } else { M '止まっています' 'not running' }))

# ---- 2. 記録 ----
Say-Step 2 3 (M '記録を集める' 'Collect the logs')
Say-Info ((M '受け手の記録：' 'Receiver log: ') + $(if ($recvMissing) { M 'ファイルがありません' 'no file' } else { Format-LineCount $recvLines.Count }))

$cr = Invoke-Adb 'logcat -d -b crash' 60000
$crashAll = @(Split-CrashBlocks (Get-Lines $cr.Out))
$crashMine = @(Select-PgpuCrash $crashAll)
$others = $crashAll.Count - $crashMine.Count
Say-Info ((M '落ちた記録：PocketGPU ' 'Crash log: PocketGPU ') + $crashMine.Count + (M ' 件・ほかのアプリ ' ', other apps ') + $others + (M ' 件' ''))
$sendFull = $false
if ($others -gt 0) {
    $sendFull = Ask-YesNo (M '落ちた記録を全部送りますか？ほかのアプリの行も入ります（N：PocketGPU だけ）' 'Send the full crash log, with other apps? (N: PocketGPU only)')
}

$appLines = @()
$appChecked = $false
if ($appVer) {
    $al = Invoke-Adb 'logcat -d -s iphone-rpc-vk' 60000
    $appChecked = $true
    $appLines = @(Get-Lines $al.Out | Where-Object { $_ -notmatch '^-{5,}\s*beginning of' })
    Say-Info ((M 'アプリの記録：' 'App log: ') + (Format-LineCount $appLines.Count))
} else {
    Say-Info ((M 'アプリの記録：' 'App log: ') + (M 'アプリが入っていないので飛ばします' 'skipped (app not installed)'))
}

$fw = Invoke-Adb 'forward --list'
$ver = Invoke-Adb 'version'

# ---- 3. 書く・zip ----
Say-Step 3 3 (M 'zip を作る' 'Make the zip')
$root = Get-OutRoot
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$name = 'pocketgpu-android-log-' + $stamp
$dir = Join-Path $root $name
$n = 1
while (Test-Path -LiteralPath $dir) { $n++; $name = 'pocketgpu-android-log-' + $stamp + '-' + $n; $dir = Join-Path $root $name }
$zip = $dir + '.zip'

$info = New-Object System.Collections.Generic.List[string]
$info.Add('PocketGPU Android log')
$info.Add('Created: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm'))
$info.Add('')
$info.Add('[Kit]')
$info.Add('Folder: ' + $script:KitDir)
foreach ($x in (Get-KitIniText)) { $info.Add('tools\kit.ini: ' + $x) }
$info.Add('')
$info.Add('[PC]')
$info.Add('Windows: ' + (Get-WindowsText))
foreach ($x in (Get-Lines $ver.Out)) { if ($x.Trim()) { $info.Add('adb: ' + $x.Trim()) } }
foreach ($x in (Get-PortFileText)) { $info.Add('android\port.txt: ' + $x) }
$info.Add('Port in use (PGPU_PORT): ' + $(if ($env:PGPU_PORT) { $env:PGPU_PORT } else { '(not set)' }))
$fwLines = @(Get-Lines $fw.Out | Where-Object { $_.Trim() })
if ($fwLines.Count -eq 0) { $info.Add('adb forward --list: (none)') } else { foreach ($x in $fwLines) { $info.Add('adb forward --list: ' + $x.Trim()) } }
$info.Add('')
$info.Add('[Phone]')
$info.Add('Model: ' + $model)
$info.Add('Maker: ' + $maker)
$info.Add('Android: ' + $release + ' (SDK ' + $sdk + ')')
$info.Add('ABI: ' + $abi)
$info.Add('Chip: ' + $soc)
$info.Add('GPU (receiver log): ' + $(if ($recvGpu) { $recvGpu } else { '-' }))
$info.Add('GPU (GLES): ' + $(if ($gles) { $gles } else { '-' }))
$info.Add('PocketGPU app: ' + $(if ($appVer) { $appVer } else { 'not installed' }))
$info.Add('Kit receiver running: ' + $(if ($running) { 'yes' } else { 'no' }))
$info.Add('')
$info.Add('[Files]')
$info.Add('receiver-log.txt: ' + $(if ($recvMissing) { 'no file on the phone' } else { [string]$recvLines.Count + ' lines' }))
$info.Add('logcat-crash.txt: ' + $(if ($sendFull) { 'full (other apps included), ' + $crashAll.Count + ' blocks' } else { 'PocketGPU only, ' + $crashMine.Count + ' blocks, ' + $others + ' other blocks left out' }))
$info.Add('logcat-app.txt: ' + $(if ($appChecked) { [string]$appLines.Count + ' lines' } else { 'skipped (app not installed)' }))
$info.Add('')
$info.Add('Not collected: phone number, IMEI, accounts, phone serial number, update.ini, kit mark.')
$info.Add('Hidden: PC user name, PC name, adb serial number, e-mail addresses.')

$crashHead = $(if ($sendFull) { '# adb logcat -d -b crash (full: other apps included)' } else { '# adb logcat -d -b crash (PocketGPU receiver only: ' + $others + ' other block(s) left out)' })
$crashBody = $(if ($sendFull) { @($crashAll) } else { @($crashMine) })
$crashText = $crashHead + "`n" + $(if ($crashBody.Count -gt 0) { (Limit-Lines (($crashBody -join "`n") -split "`n")) -join "`n" } else { '(no PocketGPU receiver crash found)' })

$failed = $false
try {
    [void](New-Item -ItemType Directory -Path $dir -Force)
    Write-LogFile $dir 'info.txt' ($info -join "`n")
    Write-LogFile $dir 'receiver-log.txt' ('# adb shell cat /data/local/tmp/pgpu_server.log' + "`n" + $(if ($recvMissing) { '(no file on the phone)' } else { (Limit-Lines $recvLines) -join "`n" }))
    Write-LogFile $dir 'logcat-crash.txt' $crashText
    if ($appChecked) { Write-LogFile $dir 'logcat-app.txt' ('# adb logcat -d -s iphone-rpc-vk' + "`n" + $(if ($appLines.Count -gt 0) { (Limit-Lines $appLines) -join "`n" } else { '(empty)' })) }
    New-LogZip $dir $zip
} catch { $failed = $true; Say-NG ((M 'zip を作れませんでした：' 'Could not make the zip: ') + $_.Exception.Message) }
if (-not $failed -and -not (Test-Path -LiteralPath $zip)) { $failed = $true; Say-NG (M 'zip を作れませんでした' 'Could not make the zip') }
if ($failed) { exit 2 }

Out-Line ''
Say-OK $zip
Say-Info (M 'この zip を Discord に送ってください' 'Send this zip on Discord')
Say-Info (M '含まない：電話番号・IMEI・アカウント・PC のユーザー名・PC 名' 'Not included: phone number, IMEI, accounts, PC user name, PC name')
if (Ask-YesNo (M 'エクスプローラーでこの場所を開きますか？' 'Open this place in Explorer?')) { [void](Invoke-Launch 'folder' $zip) }
exit 0
