# PocketGPU（Android）：USB の速さを測って、Discord に貼れる txt を 1 つ作る。Test-Android-Speed.cmd から呼ぶ。Windows PowerShell 5.1 だけで動く（Python は要らない）。
# 日本語を含むので UTF-8（BOM 付き）で保存する。BOM が無いと 5.1 は Shift_JIS として読んで壊れる。
# 画面の文は仮。M '日本語' 'English' の組で並べてあるので、言い回しはここで直す。ファイルの中は英語だけ。
# 画面に出す文は、ラベルと事実だけ。説明の文章は 1 か所だけ（USB 2 のとき）で、【仮文】の印を付けた。あとで書き直す。
#
# 端末には何も入れない・root も要らない。端末に最初から入っている nc（toybox）を、数十秒だけ使う。
# 測る物（adb で、下の決まった命令だけを打つ。getprop の全部・dumpsys の全部は使わない）
#   端末の名前・Android の版・チップ      adb shell getprop ro.product.model / ro.product.marketname / ro.product.manufacturer / ro.build.version.release
#                                          / ro.build.version.sdk / ro.product.cpu.abi / ro.soc.model / ro.board.platform
#   USB の速さ（端末側）                    adb shell cat /sys/class/udc/*/current_speed（maximum_speed・state も）。読めなければ「不明」
#   USB の速さ（PC 側）                      Windows の USB の情報。端末のつながった口の名前が SS（SuperSpeed）か HS（High-Speed）か。推定。読めなければ「不明」
#   PC→端末・端末→PC の MB/s・往復 ms      adb forward tcp:口 tcp:口 を通して、nc とつなぐ（口は 47600 から順に、予約・使用中でない物）
#       PC→端末   adb shell "nc -l -s 127.0.0.1 -p 口 > /dev/null"      PC が送った量が全部届いて nc が終わるまでの時間
#       端末→PC   adb shell "head -c バイト数 /dev/zero | nc -l -s 127.0.0.1 -p 口"      PC が読み終わるまでの時間
#       往復      adb shell "nc -l -s 127.0.0.1 -p 口 -e cat"（返ってこなければ mkfifo の形）。1 バイトを送って 1 バイトが返るまで
#     -s 127.0.0.1 が断られる nc のときは、-s 無し（端末のどの口でも待つ）でもう一度。そのときはファイルにそう書く
#   adb forward は終わりに消す。前からある adb forward（受け手の口）には触らない
#
# 集めない物：電話番号・IMEI・Google アカウント・端末のシリアル番号
# 伏せる物：PC のユーザー名・PC 名・プロフィールのフォルダ名・adb のシリアル番号・メールアドレス（「(hidden)」「(serial)」「(email)」。android-log.ps1 と同じ）
#
# 終わりのコード：0 測れた・1 adb が無い（.cmd が android\_common.cmd で見て止める）・2 txt を作れなかった・3 txt は作ったが、MB/s が測れなかった（nc が使えないなど）
#
# 試す人は使わない、試験用の差し替え（pgpu_common.ps1 の物に加えて）：
#   PGPU_SPEED_OUT=フォルダ        txt をキットのフォルダの代わりにここへ作る
#   PGPU_SPEED_PORT=番号           口を探し始める番号（既定 47600）
#   PGPU_SPEED_MB=数               1 回に流す量（MB）を決め打ちにする（既定は、最初の 4 MB の速さから 8〜192 MB に決める）
#   PGPU_SPEED_PINGS=数            往復を測る回数（既定 200）
#   PGPU_SPEED_ENDWAIT_MS=数       PC→端末で、端末の nc が終わるのを待つ長さ（ミリ秒。既定 15000）
#   PGPU_TEST_PC_USB=super|high|none  PC 側の USB の読み取りの代わりにこの答えにする（none は読めなかったことにする）
#   PGPU_TEST_CLIP=パス            クリップボードの代わりに、コピーするはずだった物をこのファイルに書く
. (Join-Path $PSScriptRoot 'pgpu_common.ps1')
. (Join-Path $PSScriptRoot 'pgpu_port.ps1')

$script:Adb = $env:ADB
$script:Serial = $env:ANDROID_SERIAL
$script:BindOpt = '-s 127.0.0.1 '
$script:BindFellBack = $false
$script:ForceKilled = $false
# 思わぬ誤りで止まるときも、短い 1 行を出して終わりのコード 2 で終わる。口の forward は消してから
trap { Write-Host ('ERROR: ' + $_.Exception.Message) -ForegroundColor Red; try { Remove-SpeedForward } catch { }; exit 2 }

# ---- 小さな道具（android-log.ps1 と同じ作り）----
function Convert-Eol([string]$s) { if (-not $s) { return '' }; return ($s -replace "`r+`n", "`n" -replace "`r", "`n") }
# adb を窓を出さずに動かす。標準入力は閉じる
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
function Get-Lines([string]$text) {
    if (-not $text) { return @() }
    $l = @($text -split "`n")
    $n = $l.Count
    while ($n -gt 0 -and $l[$n - 1].Trim() -eq '') { $n-- }
    return @($l | Select-Object -First $n)
}
function Get-HideNames {
    $names = New-Object System.Collections.Generic.List[string]
    $cand = @($env:USERNAME, $env:COMPUTERNAME)
    if ($env:USERPROFILE) { $cand += (Split-Path -Leaf $env:USERPROFILE) }
    foreach ($x in $cand) { if ($x -and $x.Trim() -and -not $names.Contains($x)) { $names.Add($x) } }
    return @($names)
}
# android-log.ps1 の Hide-Private と同じ。3 字以上の名前は前後が英数字でなければどこでも。短い名前は Users\ の次の場所だけ
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
    $text = [regex]::Replace($text, '[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}(?![A-Za-z0-9])', '(email)')
    return $text
}
function Get-WindowsText {
    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction SilentlyContinue
    if (-not $cv) { return 'not available' }
    $name = $cv.ProductName
    if ([int]$cv.CurrentBuild -ge 22000) { $name = $name -replace 'Windows 10', 'Windows 11' }
    $build = '' + $cv.CurrentBuild
    if ($cv.UBR) { $build = $build + '.' + $cv.UBR }
    return ($name + ' ' + $cv.DisplayVersion + ' (build ' + $build + ', ' + $env:PROCESSOR_ARCHITECTURE + ')').Replace('  ', ' ')
}
function Get-OutRoot {
    $cands = @()
    if ($env:PGPU_SPEED_OUT) { $cands += $env:PGPU_SPEED_OUT }
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

# ---- USB の速さ ----
# 端末側：/sys/class/udc/*/current_speed は、端末の USB コントローラーが PC と取り決めた速さ。読めない端末（SELinux・古い Android）もある
function Get-UdcValue([string]$file) {
    $r = Invoke-Adb ('shell cat /sys/class/udc/*/' + $file)
    $v = @(Get-Lines $r.Out | ForEach-Object { $_.Trim() } | Where-Object { $_ -and $_ -notmatch 'No such file|Permission denied|not found|^cat:|inaccessible' })
    return ($v -join ',')
}
function Get-SpeedLabel([string]$s) {
    switch -Regex ($s.ToLowerInvariant()) {
        '^super-speed-plus$' { return 'super-speed-plus (10 Gbps, USB 3.1 or newer)' }
        '^super-speed$' { return 'super-speed (5 Gbps, USB 3)' }
        '^high-speed$' { return 'high-speed (480 Mbps, USB 2)' }
        '^full-speed$' { return 'full-speed (12 Mbps)' }
        '^low-speed$' { return 'low-speed (1.5 Mbps)' }
        default { return $s }
    }
}
# 'USB 3' | 'USB 2' | '' （分からない）
function Get-SpeedClass([string]$s) {
    if ($s -match '(?i)super-speed') { return 'USB 3' }
    if ($s -match '(?i)high-speed') { return 'USB 2' }
    if ($s -match '(?i)full-speed|low-speed') { return 'USB 1' }
    return ''
}
# PC 側：端末の USB の口の名前（Windows が ACPI から付ける SSxx・HSxx）で、SuperSpeed の口か High-Speed の口かを推定する。
# 端末は adb のシリアル番号で探す（USB\VID_xxxx&PID_xxxx\シリアル）。見つからない・口の名前が無い PC は「不明」
function Get-PcUsb {
    if ($env:PGPU_TEST_PC_USB) {
        switch ($env:PGPU_TEST_PC_USB) {
            'super' { return @{ Level = 'USB 3'; Text = 'SuperSpeed port (USB 3), estimated from the Windows port name' } }
            'high' { return @{ Level = 'USB 2'; Text = 'High-Speed port (USB 2), estimated from the Windows port name' } }
            default { return @{ Level = ''; Text = 'unknown' } }
        }
    }
    try {
        if (-not $script:Serial) { return @{ Level = ''; Text = 'unknown (no serial)' } }
        $devs = @(Get-CimInstance Win32_PnPEntity -Filter "PNPDeviceID LIKE 'USB\\VID_%'" -ErrorAction Stop)
        $hit = @($devs | Where-Object { $_.PNPDeviceID -notmatch '&MI_\d+' -and $_.PNPDeviceID.EndsWith('\' + $script:Serial, [StringComparison]::OrdinalIgnoreCase) })
        if ($hit.Count -ne 1) { return @{ Level = ''; Text = 'unknown (phone not found in Windows USB list)' } }
        $id = $hit[0].PNPDeviceID
        $props = @(Get-PnpDeviceProperty -InstanceId $id -KeyName 'DEVPKEY_Device_BiosDeviceName', 'DEVPKEY_Device_LocationPaths' -ErrorAction SilentlyContinue)
        $joined = ($props | ForEach-Object { ($_.Data | ForEach-Object { [string]$_ }) -join ' ' }) -join ' '
        $m = [regex]::Match($joined, '[.(](SS|HS)\d+')
        if ($m.Success) {
            if ($m.Groups[1].Value -eq 'SS') { return @{ Level = 'USB 3'; Text = 'SuperSpeed port (USB 3), estimated from the Windows port name' } }
            return @{ Level = 'USB 2'; Text = 'High-Speed port (USB 2), estimated from the Windows port name' }
        }
        return @{ Level = ''; Text = 'unknown (Windows gives no port name on this PC)' }
    } catch { return @{ Level = ''; Text = 'unknown (' + ($_.Exception.Message -replace '\s+', ' ') + ')' } }
}

# ---- adb forward と、端末の nc ----
function Remove-SpeedForward {
    if ($script:FwdPort) { [void](Invoke-Adb ('forward --remove tcp:' + $script:FwdPort) 10000); $script:FwdPort = 0 }
}
# 端末の 127.0.0.1 の口が待ち状態か（/proc/net/tcp と tcp6 の 0A）。読めないときは $null
function Test-PhoneListening([int]$port) {
    $r = Invoke-Adb 'shell cat /proc/net/tcp /proc/net/tcp6' 15000
    if (-not $r.Ok -or $r.Out -notmatch '(?m)^\s*sl\s') { return $null }
    $hex = '{0:X4}' -f $port
    return [bool]([regex]::IsMatch($r.Out, '(?mi)^\s*\d+:\s+[0-9A-F]+:' + $hex + '\s+[0-9A-F:]+\s+0A\s'))
}
# 空いている口を選んで adb forward tcp:口 tcp:口 を作る。PC 側の予約・使用中・前からある adb forward・端末側の使用中を避ける
function New-SpeedForward {
    $start = 47600
    if ($env:PGPU_SPEED_PORT -and $env:PGPU_SPEED_PORT -match '^\d{1,5}$') { $start = [int]$env:PGPU_SPEED_PORT }
    $ex = Get-ExcludedPortRanges
    $fw = Invoke-Adb 'forward --list'
    $used = @([regex]::Matches($fw.Out, 'tcp:(\d+)') | ForEach-Object { [int]$_.Groups[1].Value })
    for ($i = 0; $i -lt 40; $i++) {
        $p = $start + $i
        if ($used -contains $p) { continue }
        if (Get-PortProblem $p $ex.Ranges) { continue }
        if ((Test-PhoneListening $p) -eq $true) { continue }
        $r = Invoke-Adb ('forward tcp:' + $p + ' tcp:' + $p)
        if ($r.Ok) { $script:FwdPort = $p; return $p }
    }
    return 0
}
# 端末で動かす 1 つの adb shell を、標準入力を開けたまま起こす（nc の標準入力が、始めから終わりになって nc が先に終わらないように）
function Start-PhoneCmd([string]$shellCmd) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $script:Adb
    $psi.Arguments = 'shell "' + $shellCmd + '"'
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    try { $p = [Diagnostics.Process]::Start($psi) } catch { return $null }
    return @{ P = $p; Out = $p.StandardOutput.ReadToEndAsync(); Err = $p.StandardError.ReadToEndAsync() }
}
# 終わらせる。自分が起こした adb shell だけを PID で止める
function Stop-PhoneCmd($h) {
    if (-not $h) { return '' }
    try { $h.P.StandardInput.Close() } catch { }
    if (-not $h.P.WaitForExit(3000)) { try { $h.P.Kill() } catch { }; $script:ForceKilled = $true }
    [void]$h.P.WaitForExit(2000)
    $t = ''
    try { $t = (Convert-Eol ($h.Err.Result + "`n" + $h.Out.Result)) } catch { }
    return $t
}
function Get-FirstLine([string]$t) {
    $l = @(($t -split "`n") | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    if ($l.Count -eq 0) { return '' }
    return ($l[0] -replace '\s+', ' ')
}
# nc が待ち始めるのを待つ。'listening' | 'exited'（nc が始まらずに終わった）| 'unknown'（/proc が読めない。少し待って進む）| 'timeout'
function Wait-PhoneServer([int]$port, $h, [int]$ms = 8000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $unknown = 0
    while ($sw.ElapsedMilliseconds -lt $ms) {
        if ($h.P.HasExited) { return 'exited' }
        $l = Test-PhoneListening $port
        if ($l -eq $true) { return 'listening' }
        if ($null -eq $l) { $unknown++; if ($unknown -ge 2) { Start-Sleep -Milliseconds 800; return $(if ($h.P.HasExited) { 'exited' } else { 'unknown' }) } }
        Start-Sleep -Milliseconds 150
    }
    return 'timeout'
}
function New-RandomBuffer([int]$n) {
    $b = New-Object byte[] $n
    (New-Object Random(12345)).NextBytes($b)
    return , $b
}

# 1 回の転送。戻り値：@{ Ok; MBps; Sec; Why; Detail; End }。Why：server（nc が始まらない）| connect | io
function Measure-ToPhone([int]$port, [int64]$bytes) {
    $h = Start-PhoneCmd ('nc -l ' + $script:BindOpt + '-p ' + $port + ' > /dev/null')
    if (-not $h) { return @{ Ok = $false; Why = 'server'; Detail = 'could not start adb' } }
    $c = $null
    try {
        $w = Wait-PhoneServer $port $h
        if ($w -eq 'exited' -or $w -eq 'timeout') { $d = Stop-PhoneCmd $h; $h = $null; return @{ Ok = $false; Why = 'server'; Detail = (Get-FirstLine $d) } }
        $c = Connect-Tcp '127.0.0.1' $port 5000
        if (-not $c) { return @{ Ok = $false; Why = 'connect'; Detail = 'connect failed' } }
        $c.SendBufferSize = 4MB
        $s = $c.GetStream()
        $s.WriteTimeout = 30000
        $s.ReadTimeout = 30000
        $buf = New-RandomBuffer 1MB
        $left = $bytes
        $sw = [Diagnostics.Stopwatch]::StartNew()
        try {
            while ($left -gt 0) {
                $k = [int][Math]::Min([int64]$buf.Length, $left)
                $s.Write($buf, 0, $k)
                $left -= $k
            }
        } catch { return @{ Ok = $false; Why = 'io'; Detail = ('send stopped after ' + [int](($bytes - $left) / 1MB) + ' MB') } }
        $tSent = $sw.Elapsed.TotalSeconds
        try { $c.Client.Shutdown([Net.Sockets.SocketShutdown]::Send) } catch { }
        # 端末の nc が全部受けて終わると、接続が閉じる。そこまでを時間にする。閉じるのが見えなければ送り終わりまで（実際より速く出るので、ファイルにそう書く）
        $end = 'sent'
        $sec = $tSent
        $one = New-Object byte[] 1
        $s.ReadTimeout = $(if ($env:PGPU_SPEED_ENDWAIT_MS -and $env:PGPU_SPEED_ENDWAIT_MS -match '^\d{3,6}$') { [int]$env:PGPU_SPEED_ENDWAIT_MS } else { 15000 })
        try { [void]$s.Read($one, 0, 1); $end = 'closed'; $sec = $sw.Elapsed.TotalSeconds }
        catch { if (Test-Closed $_.Exception) { $end = 'closed'; $sec = $sw.Elapsed.TotalSeconds } }
        if ($sec -le 0) { $sec = 0.000001 }
        return @{ Ok = $true; MBps = (($bytes / 1MB) / $sec); Sec = $sec; End = $end }
    } finally {
        if ($c) { $c.Close() }
        if ($h) { [void](Stop-PhoneCmd $h) }
    }
}
function Measure-FromPhone([int]$port, [int64]$bytes) {
    $h = Start-PhoneCmd ('head -c ' + $bytes + ' /dev/zero | nc -l ' + $script:BindOpt + '-p ' + $port)
    if (-not $h) { return @{ Ok = $false; Why = 'server'; Detail = 'could not start adb' } }
    $c = $null
    try {
        $w = Wait-PhoneServer $port $h
        if ($w -eq 'exited' -or $w -eq 'timeout') { $d = Stop-PhoneCmd $h; $h = $null; return @{ Ok = $false; Why = 'server'; Detail = (Get-FirstLine $d) } }
        $c = Connect-Tcp '127.0.0.1' $port 5000
        if (-not $c) { return @{ Ok = $false; Why = 'connect'; Detail = 'connect failed' } }
        $c.ReceiveBufferSize = 4MB
        $s = $c.GetStream()
        $s.ReadTimeout = 30000
        $buf = New-Object byte[] 1MB
        $got = [int64]0
        $sw = [Diagnostics.Stopwatch]::StartNew()
        try {
            while ($got -lt $bytes) {
                $k = $s.Read($buf, 0, [int][Math]::Min([int64]$buf.Length, $bytes - $got))
                if ($k -le 0) { break }
                $got += $k
            }
        } catch { }
        $sec = $sw.Elapsed.TotalSeconds
        if ($got -lt $bytes) { return @{ Ok = $false; Why = 'io'; Detail = ('received ' + [int]($got / 1MB) + ' of ' + [int]($bytes / 1MB) + ' MB') } }
        if ($sec -le 0) { $sec = 0.000001 }
        return @{ Ok = $true; MBps = (($bytes / 1MB) / $sec); Sec = $sec }
    } finally {
        if ($c) { $c.Close() }
        if ($h) { [void](Stop-PhoneCmd $h) }
    }
}
# 往復：1 バイトを送って 1 バイトが返るまで。mode：'e'（nc -e cat）| 'fifo'
function Measure-Rtt([int]$port, [string]$mode, [int]$pings) {
    $fifo = '/data/local/tmp/pgpu_rtt.fifo'
    if ($mode -eq 'e') { $cmd = 'nc -l ' + $script:BindOpt + '-p ' + $port + ' -e cat' }
    else { $cmd = 'rm -f ' + $fifo + '; mkfifo ' + $fifo + ' && cat ' + $fifo + ' | nc -l ' + $script:BindOpt + '-p ' + $port + ' > ' + $fifo + '; rm -f ' + $fifo }
    $h = Start-PhoneCmd $cmd
    if (-not $h) { return @{ Ok = $false; Why = 'server'; Detail = 'could not start adb' } }
    $c = $null
    try {
        $w = Wait-PhoneServer $port $h
        if ($w -eq 'exited' -or $w -eq 'timeout') { $d = Stop-PhoneCmd $h; $h = $null; return @{ Ok = $false; Why = 'server'; Detail = (Get-FirstLine $d) } }
        $c = Connect-Tcp '127.0.0.1' $port 5000
        if (-not $c) { return @{ Ok = $false; Why = 'connect'; Detail = 'connect failed' } }
        $s = $c.GetStream()
        $s.ReadTimeout = 4000
        $s.WriteTimeout = 4000
        $out = New-Object byte[] 1
        $out[0] = 0x55
        $in = New-Object byte[] 1
        $ms = New-Object System.Collections.Generic.List[double]
        $freq = [double][Diagnostics.Stopwatch]::Frequency
        $total = [Diagnostics.Stopwatch]::StartNew()
        $warm = 20
        for ($i = 0; $i -lt ($warm + $pings); $i++) {
            $t0 = [Diagnostics.Stopwatch]::GetTimestamp()
            try { $s.Write($out, 0, 1); $k = $s.Read($in, 0, 1) } catch { return @{ Ok = $false; Why = 'io'; Detail = 'no echo from the phone' } }
            if ($k -ne 1) { return @{ Ok = $false; Why = 'io'; Detail = 'connection closed' } }
            $t1 = [Diagnostics.Stopwatch]::GetTimestamp()
            if ($i -ge $warm) { $ms.Add(($t1 - $t0) * 1000.0 / $freq) }
            # 1 回が遅い端末でも 12 秒で切り上げる（20 回は測る）
            if ($total.Elapsed.TotalSeconds -gt 12 -and $ms.Count -ge 20) { break }
        }
        $a = @($ms | Sort-Object)
        $n = $a.Count
        $p95 = $a[[Math]::Min($n - 1, [int][Math]::Ceiling($n * 0.95) - 1)]
        return @{ Ok = $true; Min = $a[0]; Median = $a[[int](($n - 1) / 2)]; P95 = $p95; Max = $a[$n - 1]; Count = $n; Mode = $mode }
    } finally {
        if ($c) { $c.Close() }
        if ($h) { [void](Stop-PhoneCmd $h) }
    }
}
function Get-Median([double[]]$v) {
    $a = @($v | Sort-Object)
    if ($a.Count -eq 0) { return 0 }
    return $a[[int](($a.Count - 1) / 2)]
}
function Format-Num([double]$x) { return $x.ToString('0.0', [Globalization.CultureInfo]::InvariantCulture) }
function Format-Ms([double]$x) {
    if ($x -lt 10) { return $x.ToString('0.00', [Globalization.CultureInfo]::InvariantCulture) }
    return $x.ToString('0.0', [Globalization.CultureInfo]::InvariantCulture)
}

# 1 つの向きを、runs 回測る。bind の形が断られたら -s 無しで 1 度やり直す（最初の 1 回だけ）
function Measure-Direction([string]$dir, [int]$port, [int64]$bytes, [int]$runs) {
    $res = @()
    $fail = $null
    for ($i = 0; $i -lt $runs; $i++) {
        if ($dir -eq 'to') { $r = Measure-ToPhone $port $bytes } else { $r = Measure-FromPhone $port $bytes }
        if (-not $r.Ok -and $r.Why -eq 'server' -and $script:BindOpt -and -not $script:BindFellBack) {
            $script:BindOpt = ''
            $script:BindFellBack = $true
            if ($dir -eq 'to') { $r = Measure-ToPhone $port $bytes } else { $r = Measure-FromPhone $port $bytes }
        }
        if (-not $r.Ok) { $fail = $r; break }
        $res += $r
    }
    return @{ Runs = $res; Fail = $fail }
}

# =====================================================================
# ---- 流れ / Main ----
# =====================================================================
$script:FwdPort = 0
if (-not $script:Adb -or -not (Test-Path -LiteralPath $script:Adb)) {
    Out-Line ''
    Out-Line 'ERROR: adb was not found. Run Test-Android-Speed.cmd (it finds adb and checks the phone first).' 'Red'
    exit 1
}

Out-Line ''
Out-Line (M 'PocketGPU Android の速さを測ります' 'Measure the PocketGPU Android speed')

# ---- 1. スマートフォン ----
Say-Step 1 4 (M 'スマートフォンをチェック' 'Check the phone')
$model = Get-Prop 'ro.product.model'
$market = Get-Prop 'ro.product.marketname'
$maker = Get-Prop 'ro.product.manufacturer'
$release = Get-Prop 'ro.build.version.release'
$sdk = Get-Prop 'ro.build.version.sdk'
$abi = Get-Prop 'ro.product.cpu.abi'
$soc = Get-Prop 'ro.soc.model'
if (-not $soc) { $soc = Get-Prop 'ro.board.platform' }
$nameParts = @($maker, $model) | Where-Object { $_ }
$phoneName = ($nameParts -join ' ')
if ($market -and $phoneName -notmatch [regex]::Escape($market)) { $phoneName += ' (' + $market + ')' }
if (-not $phoneName) { $phoneName = 'unknown' }
$pr = Invoke-Adb 'shell pidof pgpu_server'
$recvRunning = $pr.Ok -and [bool]($pr.Out.Trim())
Say-Info ((M '機種：' 'Model: ') + $phoneName)
Say-Info ('Android: ' + $(if ($release) { $release } else { '-' }) + $(if ($sdk) { ' (SDK ' + $sdk + ')' } else { '' }))
Say-Info ((M 'チップ：' 'Chip: ') + $(if ($soc) { $soc } else { '-' }))
Say-Info ((M '受け手：' 'Receiver: ') + $(if ($recvRunning) { M '動いています' 'running' } else { M '止まっています' 'not running' }))

# ---- 2. USB ----
Say-Step 2 4 (M 'USB の接続の速さ' 'USB link speed')
$udcCur = Get-UdcValue 'current_speed'
$udcMax = Get-UdcValue 'maximum_speed'
$udcState = Get-UdcValue 'state'
$phoneUsbText = $(if ($udcCur) { Get-SpeedLabel $udcCur } else { 'unknown' })
$phoneClass = Get-SpeedClass $udcCur
$pc = Get-PcUsb
$linkClass = $(if ($phoneClass) { $phoneClass } elseif ($pc.Level) { $pc.Level } else { '' })
$linkBy = $(if ($phoneClass) { 'phone' } elseif ($pc.Level) { 'PC' } else { '' })
Say-Info ((M '端末側：' 'Phone side: ') + $(if ($udcCur) { $phoneUsbText } else { M '不明' 'unknown' }))
if ($udcMax) { Say-Info ((M '端末の上限：' 'Phone maximum: ') + (Get-SpeedLabel $udcMax)) }
Say-Info ((M 'PC 側：' 'PC side: ') + $pc.Text)
Say-Info ((M '接続：' 'Link: ') + $(if ($linkClass) { $linkClass + ' (from ' + $linkBy + ')' } else { M '不明' 'unknown' }))
if ($phoneClass -eq 'USB 2' -or ($phoneClass -eq '' -and $pc.Level -eq 'USB 2')) {
    # 【仮文】あとで書き直す
    Say-Warn (M '【仮文】USB 3 のポートと USB 3 のケーブルでつなぎ直すと速くなるかもしれません' 'DRAFT: it may be faster with a USB 3 port and a USB 3 cable')
}

# ---- 3. 速さ ----
Say-Step 3 4 (M 'adb forward の速さ' 'Speed through adb forward')
$port = New-SpeedForward
$toRes = $null; $fromRes = $null; $rttRes = $null
$ncProblem = ''
$mbEach = 0
$runs = 0
if ($port -eq 0) {
    $ncProblem = 'could not open an adb forward port'
    Say-NG (M 'adb forward の口を作れませんでした' 'Could not open an adb forward port')
} else {
    Say-Info ((M 'PC の口：' 'PC port: ') + $port)
    # 最初の 4 MB で速さを見て、1 回に流す量を決める（約 2 秒分。8〜192 MB）
    $probe = Measure-ToPhone $port (4MB)
    if (-not $probe.Ok -and $probe.Why -eq 'server' -and $script:BindOpt -and -not $script:BindFellBack) {
        $script:BindOpt = ''
        $script:BindFellBack = $true
        $probe = Measure-ToPhone $port (4MB)
    }
    if (-not $probe.Ok) {
        $ncProblem = $(if ($probe.Detail) { 'nc on the phone did not work: ' + $probe.Detail } else { 'nc on the phone did not work (' + $probe.Why + ')' })
        Say-NG (M 'この端末の nc が使えませんでした：' 'nc on the phone did not work: ')
        Say-Info $(if ($probe.Detail) { $probe.Detail } else { $probe.Why })
    } else {
        $mb = [int][Math]::Min(192, [Math]::Max(8, [Math]::Round($probe.MBps * 2)))
        if ($env:PGPU_SPEED_MB -and $env:PGPU_SPEED_MB -match '^\d{1,4}$' -and [int]$env:PGPU_SPEED_MB -ge 1) { $mb = [int]$env:PGPU_SPEED_MB }
        $runs = $(if ($probe.MBps -lt 4) { 1 } else { 3 })
        $mbEach = $mb
        $bytes = [int64]$mb * 1MB
        $toRes = Measure-Direction 'to' $port $bytes $runs
        if ($toRes.Runs.Count -gt 0) { Say-OK ((M 'PC → 端末：' 'PC to phone: ') + (Format-Num (Get-Median ($toRes.Runs | ForEach-Object { $_.MBps }))) + ' MB/s') }
        else { Say-NG ((M 'PC → 端末：' 'PC to phone: ') + $(if ($toRes.Fail.Detail) { $toRes.Fail.Detail } else { $toRes.Fail.Why })) }
        $fromRes = Measure-Direction 'from' $port $bytes $runs
        if ($fromRes.Runs.Count -gt 0) { Say-OK ((M '端末 → PC：' 'Phone to PC: ') + (Format-Num (Get-Median ($fromRes.Runs | ForEach-Object { $_.MBps }))) + ' MB/s') }
        else { Say-NG ((M '端末 → PC：' 'Phone to PC: ') + $(if ($fromRes.Fail.Detail) { $fromRes.Fail.Detail } else { $fromRes.Fail.Why })) }
        $pings = 200
        if ($env:PGPU_SPEED_PINGS -and $env:PGPU_SPEED_PINGS -match '^\d{1,4}$' -and [int]$env:PGPU_SPEED_PINGS -ge 5) { $pings = [int]$env:PGPU_SPEED_PINGS }
        $rttRes = Measure-Rtt $port 'e' $pings
        if (-not $rttRes.Ok) { $rttRes = Measure-Rtt $port 'fifo' $pings }
        if ($rttRes.Ok) { Say-OK ((M '往復：' 'Round trip: ') + (Format-Ms $rttRes.Median) + ' ms') }
        else { Say-NG ((M '往復：' 'Round trip: ') + $(if ($rttRes.Detail) { $rttRes.Detail } else { $rttRes.Why })) }
    }
}
Remove-SpeedForward
if ($script:ForceKilled) {
    # adb shell が止まらなかったので PID で止めた。端末の nc が残っていたら、このコマンドで消えるまでの短い間だけ口が残る（端末を再起動しても消える）
    [void](Invoke-Adb 'shell "kill $(pidof nc) 2>/dev/null; rm -f /data/local/tmp/pgpu_rtt.fifo; true"' 10000)
}

# ---- 4. txt ----
Say-Step 4 4 (M 'txt を作る' 'Write the text file')
$fwl = Invoke-Adb 'version'
$info = New-Object System.Collections.Generic.List[string]
$info.Add('PocketGPU Android speed test')
$info.Add('Created: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm'))
$info.Add('')
$info.Add('[Phone]')
$info.Add('Name: ' + $phoneName)
$info.Add('Android: ' + $(if ($release) { $release } else { '-' }) + $(if ($sdk) { ' (SDK ' + $sdk + ')' } else { '' }))
$info.Add('Chip: ' + $(if ($soc) { $soc } else { '-' }))
$info.Add('ABI: ' + $(if ($abi) { $abi } else { '-' }))
$info.Add('Kit receiver running: ' + $(if ($recvRunning) { 'yes' } else { 'no' }))
$info.Add('')
$info.Add('[USB]')
$info.Add('Phone side (current): ' + $phoneUsbText)
$info.Add('Phone side (maximum): ' + $(if ($udcMax) { Get-SpeedLabel $udcMax } else { 'unknown' }))
if ($udcState) { $info.Add('Phone USB controller state: ' + $udcState) }
$info.Add('PC side: ' + $pc.Text)
$info.Add('Link: ' + $(if ($linkClass) { $linkClass + ' (from ' + $linkBy + ')' } else { 'unknown' }))
$info.Add('')
$info.Add('[adb forward]')
if ($port -eq 0 -or $ncProblem) {
    $info.Add('Not measured: ' + $ncProblem)
} else {
    $info.Add('Method: adb forward tcp:' + $port + ' to nc on the phone (' + $(if ($script:BindFellBack) { 'nc listens on all addresses, -s was refused' } else { 'nc listens on 127.0.0.1' }) + ')')
    $info.Add('Data: ' + $mbEach + ' MB per run, ' + $runs + ' run(s) per direction. 1 MB = 1048576 bytes')
    foreach ($d in @(@{ L = 'PC to phone'; R = $toRes }, @{ L = 'Phone to PC'; R = $fromRes })) {
        $rr = $d.R
        if ($rr.Runs.Count -gt 0) {
            $v = @($rr.Runs | ForEach-Object { $_.MBps })
            $line = $d.L + ': ' + (Format-Num (Get-Median $v)) + ' MB/s (median; runs ' + ((@($v | ForEach-Object { Format-Num $_ })) -join ', ') + ')'
            if (@($rr.Runs | Where-Object { $_.End -eq 'sent' }).Count -gt 0) { $line += ' [end of transfer not seen on the phone side]' }
            $info.Add($line)
        } else {
            $info.Add($d.L + ': not measured (' + $(if ($rr.Fail.Detail) { $rr.Fail.Detail } else { $rr.Fail.Why }) + ')')
        }
    }
    if ($rttRes -and $rttRes.Ok) {
        $info.Add('Round trip (1 byte): median ' + (Format-Ms $rttRes.Median) + ' ms, min ' + (Format-Ms $rttRes.Min) + ', p95 ' + (Format-Ms $rttRes.P95) + ', max ' + (Format-Ms $rttRes.Max) + ' (' + $rttRes.Count + ' pings, ' + $(if ($rttRes.Mode -eq 'e') { 'nc -e cat' } else { 'fifo' }) + ')')
    } else {
        $info.Add('Round trip: not measured (' + $(if ($rttRes -and $rttRes.Detail) { $rttRes.Detail } else { 'no answer' }) + ')')
    }
}
$info.Add('')
$info.Add('[PC]')
$info.Add('Windows: ' + (Get-WindowsText))
foreach ($x in (Get-Lines $fwl.Out)) { if ($x.Trim() -and $x -notmatch '^Installed as') { $info.Add('adb: ' + $x.Trim()) } }
$info.Add('')
$info.Add('Not collected: phone number, IMEI, accounts, phone serial number. Hidden: PC user name, PC name, adb serial number, e-mail addresses.')

$text = Hide-Private ($info -join "`n")
$root = Get-OutRoot
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$name = 'pocketgpu-android-speed-' + $stamp
$file = Join-Path $root ($name + '.txt')
$n = 1
while (Test-Path -LiteralPath $file) { $n++; $file = Join-Path $root ($name + '-' + $n + '.txt') }
$failed = $false
try {
    $body = ($text -replace "`r`n", "`n" -replace "`r", "`n") -replace "`n", "`r`n"
    if (-not $body.EndsWith("`r`n")) { $body += "`r`n" }
    [IO.File]::WriteAllText($file, $body, (New-Object Text.UTF8Encoding($true)))
} catch { $failed = $true; Say-NG ((M 'txt を作れませんでした：' 'Could not write the text file: ') + $_.Exception.Message) }
if (-not $failed -and -not (Test-Path -LiteralPath $file)) { $failed = $true; Say-NG (M 'txt を作れませんでした' 'Could not write the text file') }
if ($failed) { exit 2 }

Out-Line ''
foreach ($l in ($text -split "`n")) { Out-Line $l }
Out-Line ''
Say-OK $file
Say-Info (M 'この txt を Discord に貼ってください' 'Paste this text on Discord')
Say-Info (M '含まない：電話番号・IMEI・アカウント・シリアル番号・PC のユーザー名・PC 名' 'Not included: phone number, IMEI, accounts, serial number, PC user name, PC name')
if (Ask-YesNo (M 'クリップボードにコピーしますか？' 'Copy it to the clipboard?')) {
    try {
        if ($env:PGPU_TEST_CLIP) { [IO.File]::WriteAllText($env:PGPU_TEST_CLIP, $text, (New-Object Text.UTF8Encoding($false))) }
        else { Set-Clipboard -Value $text }
        Say-OK (M 'コピーしました' 'Copied')
    } catch { Say-Warn ((M 'コピーできませんでした：' 'Could not copy: ') + $_.Exception.Message) }
}
$measured = ($toRes -and $toRes.Runs.Count -gt 0) -or ($fromRes -and $fromRes.Runs.Count -gt 0)
exit $(if ($measured) { 0 } else { 3 })
