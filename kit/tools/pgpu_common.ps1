# PocketGPU：「始める」（start.ps1）と「iPhone の GPU で起動」（run.ps1）が一緒に使う部品。Windows PowerShell 5.1 だけで動く（Python は要らない）。
# 日本語を含むので UTF-8（BOM 付き）で保存する。BOM が無いと 5.1 は Shift_JIS として読んで壊れる。
# 画面の文は仮。M '日本語' 'English' の組で並べてあるので、言い回しはここで直す。
#
# 試す人は使わない、試験用の差し替え：
#   IPHONE_VK_MUX=番号         Apple のサービス（127.0.0.1:27015）の代わりにこの番号へ（ドライバと同じ名前）
#   PGPU_VKLIST=パス            vklist.exe の代わり（GPU を使わずに試すため）
#   PGPU_TEST_ELEVATED=1        管理者で動いていることにする
#   PGPU_TEST_APPLE_SW=none|devices|itunes|service  入っている Apple のソフトをこれにする（devices は Apple Devices・itunes はストアの iTunes・service は iTunes の Windows のサービス）
#   PGPU_SHORTCUT_DIR=フォルダ   スタートメニューと「送る」の代わりに、ここへ入口を作る
#   PGPU_NOPROMPT=1             聞かずに「いいえ」で進み、最後に止まらない
#   PGPU_ANSWERS=y,n,...        PGPU_NOPROMPT のとき、はい・いいえの問いにこの順で答える（足りなければ「いいえ」）
#   PGPU_LANG=ja|en             画面の言葉を決める（無ければ Windows の表示言語）
#   PGPU_TEST_VK_LOADER=版|none  Vulkan のローダーの版をこれにする（古いローダーのふり。none は vulkan-1.dll が無い）

$ErrorActionPreference = 'Stop'
$script:KitDir = Split-Path -Parent $PSScriptRoot
$script:MuxPort = 27015
if ($env:IPHONE_VK_MUX) { $script:MuxPort = [int]$env:IPHONE_VK_MUX }
$script:RecvPort = 50060   # Vulkan の受け手の口
$script:BwPort = 50058     # USB の太さを測る受け口（アプリが開いている間いつも待つ）

# make_kit が書く tools\kit.ini（proto= と app=）。無ければ今の値
$script:ProtoVersion = 3
$script:AppVersion = ''
$ini = Join-Path $PSScriptRoot 'kit.ini'
if (Test-Path $ini) {
    foreach ($line in Get-Content $ini) {
        if ($line -match '^proto=(\d+)') { $script:ProtoVersion = [int]$Matches[1] }
        if ($line -match '^app=(.+)$') { $script:AppVersion = $Matches[1].Trim() }
    }
}

if ($env:PGPU_LANG) { $script:Ja = $env:PGPU_LANG -eq 'ja' }
else { $script:Ja = [Globalization.CultureInfo]::CurrentUICulture.Name -like 'ja*' }
$script:NoPrompt = $env:PGPU_NOPROMPT -eq '1'
$script:Answers = New-Object System.Collections.Generic.Queue[string]
if ($env:PGPU_ANSWERS) { foreach ($x in $env:PGPU_ANSWERS.Split(',')) { $script:Answers.Enqueue($x.Trim()) } }

function M([string]$ja, [string]$en) { if ($script:Ja) { $ja } else { $en } }

# ---- 画面と記録 ----
$script:Log = New-Object System.Collections.Generic.List[string]
function Out-Line([string]$s, [string]$color = '') {
    if ($color) { Write-Host $s -ForegroundColor $color } else { Write-Host $s }
    $script:Log.Add($s)
}
function Say-Step([int]$n, [int]$total, [string]$t) { Out-Line ''; Out-Line ('[{0}/{1}] {2}' -f $n, $total, $t) }
function Say-OK([string]$t)   { Out-Line ('  OK  ' + $t) 'Green' }
function Say-Warn([string]$t) { Out-Line ('  !   ' + $t) 'Yellow' }
function Say-NG([string]$t)   { Out-Line ('  NG  ' + $t) 'Red' }
function Say-Info([string]$t) { Out-Line ('      ' + $t) }

# はい・いいえを聞く。聞かない設定なら「いいえ」
function Ask-YesNo([string]$q) {
    if ($script:NoPrompt) {
        $yes = $script:Answers.Count -gt 0 -and $script:Answers.Dequeue() -eq 'y'
        Out-Line ('      ' + $q + ' -> ' + $(if ($yes) { M 'はい' 'yes' } else { M 'いいえ' 'no' }))
        return $yes
    }
    $a = Read-Host ('      ' + $q + ' ' + (M '[y/N]' '[y/N]'))
    $script:Log.Add('      ' + $q + ' -> ' + $a)
    return $a -match '^(y|yes|はい)$'
}
# 直したら Enter で同じ所を確かめ直す。q を打つと終わる。入力が尽きた（null）ときと、50 回目も終わる（回り続けない）
$script:Retries = 0
function Ask-Retry {
    if ($script:NoPrompt) { return $false }
    if (++$script:Retries -gt 50) { return $false }
    $a = Read-Host ('      ' + (M '問題を解消したら Enter キーを押してください。もう一度チェックします。やめるときは q を入力して Enter キーを押してください。' 'Press Enter to check again after fixing it, or type q and Enter to stop'))
    return ($null -ne $a) -and ($a -ne 'q')
}

# 記録をファイルに書く。ユーザー名と PC 名は伏せる
function Save-Log([string]$name) {
    $body = ($script:Log -join "`r`n")
    if ($env:USERNAME) { $body = $body -replace [regex]::Escape($env:USERNAME), '(user)' }
    if ($env:COMPUTERNAME) { $body = $body -replace [regex]::Escape($env:COMPUTERNAME), '(pc)' }
    $path = Join-Path $script:KitDir $name
    [IO.File]::WriteAllText($path, $body + "`r`n", (New-Object Text.UTF8Encoding($true)))
    return $path
}

# ---- この PC ----
function Test-Elevated {
    if ($env:PGPU_TEST_ELEVATED -eq '1') { return $true }
    $p = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
function Test-UacOff {
    try { return (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' -Name EnableLUA).EnableLUA -eq 0 } catch { return $false }
}
# Vulkan のローダーは環境変数のパスを ANSI の文字で読むので、その文字に無い字があると json を開けない見込み
function Test-AnsiPath([string]$p) {
    $enc = [Text.Encoding]::Default
    return $enc.GetString($enc.GetBytes($p)) -eq $p
}
function Get-VulkanLoaderVersion {
    # 試験用：古いローダーのふり。PGPU_TEST_VK_LOADER=1.3.204（版）か none（vulkan-1.dll が無い）
    if ($env:PGPU_TEST_VK_LOADER) {
        if ($env:PGPU_TEST_VK_LOADER -eq 'none') { return $null }
        return [Version]$env:PGPU_TEST_VK_LOADER
    }
    $dll = Join-Path $env:WINDIR 'System32\vulkan-1.dll'
    if (-not (Test-Path $dll)) { return $null }
    $v = (Get-Item $dll).VersionInfo
    return New-Object Version($v.FileMajorPart, $v.FileMinorPart, $v.FileBuildPart, $v.FilePrivatePart)
}

# ---- Apple のソフト ----
function Get-AppleSoftware {
    $r = @{ Service = $null; AppleDevices = $null; StoreITunes = $null; AltServer = $null; AltServerRunning = $false }
    if ($env:PGPU_TEST_APPLE_SW) {
        $fam = @{ devices = 'AppleInc.AppleDevices_nzyj5cx40ttqa'; itunes = 'AppleInc.iTunes_nzyj5cx40ttqa' }
        $k = $env:PGPU_TEST_APPLE_SW
        $fake = $(if ($fam[$k]) { [pscustomobject]@{ PackageFamilyName = $fam[$k]; Aumid = $fam[$k] + '!App' } } else { $null })
        if ($k -eq 'devices') { $r.AppleDevices = $fake }
        if ($k -eq 'itunes') { $r.StoreITunes = $fake }
        if ($k -eq 'service') { $r.Service = [pscustomobject]@{ Status = 'Stopped' } }
        return $r
    }
    $r.Service = Get-Service -Name 'Apple Mobile Device Service' -ErrorAction SilentlyContinue
    try { $r.AppleDevices = Get-AppxPackage -Name 'AppleInc.AppleDevices' -ErrorAction SilentlyContinue | Select-Object -First 1 } catch { }
    try { $r.StoreITunes = Get-AppxPackage -Name 'AppleInc.iTunes' -ErrorAction SilentlyContinue | Select-Object -First 1 } catch { }
    $alt = Join-Path ${env:ProgramFiles(x86)} 'AltServer\AltServer.exe'
    if (Test-Path $alt) { $r.AltServer = $alt }
    $r.AltServerRunning = [bool](Get-Process -Name AltServer -ErrorAction SilentlyContinue)
    return $r
}
# ストアのアプリを開く名前（AUMID）。表示名は言語で変わるので、manifest の Application の Id から作る
function Get-Aumid($pkg) {
    if ($pkg.PSObject.Properties['Aumid']) { return $pkg.Aumid }
    try {
        $m = Get-AppxPackageManifest $pkg
        $id = @($m.Package.Applications.Application)[0].Id
        return $pkg.PackageFamilyName + '!' + $id
    } catch { return $null }
}

# ---- TCP と usbmuxd ----
function Connect-Tcp([string]$hostName, [int]$port, [int]$ms = 2000) {
    $c = New-Object Net.Sockets.TcpClient
    try {
        $ar = $c.BeginConnect($hostName, $port, $null, $null)
        if (-not $ar.AsyncWaitHandle.WaitOne($ms)) { $c.Close(); return $null }
        $c.EndConnect($ar)
    } catch { $c.Close(); return $null }
    $c.NoDelay = $true
    return $c
}
function Test-MuxListening { $c = Connect-Tcp '127.0.0.1' $script:MuxPort 1500; if ($c) { $c.Close(); return $true }; return $false }

function Read-Exact($stream, [int]$n) {
    $buf = New-Object byte[] $n
    $got = 0
    while ($got -lt $n) {
        $k = $stream.Read($buf, $got, $n - $got)
        if ($k -le 0) { throw (New-Object IO.EndOfStreamException('closed')) }
        $got += $k
    }
    return , $buf
}

# 相手が切った（EndOfStream）のか、待ちきれなかった（IOException の時間切れ）のか。.NET の例外は包まれて届くので中までたどる
function Test-Closed($ex) {
    while ($ex) {
        if ($ex -is [IO.EndOfStreamException]) { return $true }
        if ($ex -is [Net.Sockets.SocketException] -and $ex.SocketErrorCode -ne 'TimedOut') { return $true }
        $ex = $ex.InnerException
    }
    return $false
}

function ConvertFrom-PlistNode($n) {
    switch ($n.LocalName) {
        'dict' {
            $h = @{}
            $kids = @($n.ChildNodes | Where-Object { $_.NodeType -eq 'Element' })
            for ($i = 0; $i + 1 -lt $kids.Count; $i += 2) { $h[$kids[$i].InnerText] = ConvertFrom-PlistNode $kids[$i + 1] }
            return $h
        }
        'array' { return , @($n.ChildNodes | Where-Object { $_.NodeType -eq 'Element' } | ForEach-Object { ConvertFrom-PlistNode $_ }) }
        'integer' { return [int64]$n.InnerText }
        'true' { return $true }
        'false' { return $false }
        default { return $n.InnerText }
    }
}

# usbmuxd に 1 つ頼んで、返事の plist を hashtable で返す（ドライバの usbmuxd の呼び方と同じ形）
function Invoke-Mux($client, [string]$type, [string]$extra = '') {
    $xml = '<?xml version="1.0" encoding="UTF-8"?>' + "`n" +
        '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' + "`n" +
        '<plist version="1.0"><dict><key>MessageType</key><string>' + $type + '</string>' +
        '<key>ClientVersionString</key><string>pocketgpu-start</string><key>ProgName</key><string>pocketgpu-start</string>' +
        $extra + '</dict></plist>' + "`n"
    $body = [Text.Encoding]::UTF8.GetBytes($xml)
    $head = New-Object byte[] 16
    [BitConverter]::GetBytes([uint32](16 + $body.Length)).CopyTo($head, 0)
    [BitConverter]::GetBytes([uint32]1).CopyTo($head, 4)
    [BitConverter]::GetBytes([uint32]8).CopyTo($head, 8)
    [BitConverter]::GetBytes([uint32]1).CopyTo($head, 12)
    $s = $client.GetStream()
    $s.ReadTimeout = 3000
    $s.Write($head, 0, 16)
    $s.Write($body, 0, $body.Length)
    $h = Read-Exact $s 16
    $len = [BitConverter]::ToUInt32($h, 0)
    if ($len -lt 16 -or $len -gt 1MB) { throw 'usbmuxd: bad reply length' }
    $b = Read-Exact $s ([int]$len - 16)
    $text = [Text.Encoding]::UTF8.GetString($b)
    if ($text.StartsWith('bplist')) { throw 'usbmuxd: binary plist' }
    $text = $text -replace '<!DOCTYPE[^>]*>', ''
    $doc = New-Object Xml.XmlDocument
    $doc.XmlResolver = $null
    $doc.LoadXml($text)
    $root = @($doc.DocumentElement.ChildNodes | Where-Object { $_.NodeType -eq 'Element' })[0]
    return (ConvertFrom-PlistNode $root)
}

# つながっている機器の一覧。USB と Network（Wi-Fi の同期）が混ざる。呼ぶ側は @() で囲む（0 台・1 台でも配列にする）
function Get-MuxDevices {
    $c = Connect-Tcp '127.0.0.1' $script:MuxPort 1500
    if (-not $c) { return $null }
    try { $r = Invoke-Mux $c 'ListDevices' } catch { return $null } finally { $c.Close() }
    $list = @()
    foreach ($d in @($r['DeviceList'])) {
        if ($d -isnot [hashtable]) { continue }
        $p = $d['Properties']
        if ($p -isnot [hashtable]) { $p = @{} }
        $list += [pscustomobject]@{ Id = [int64]$d['DeviceID']; Type = [string]$p['ConnectionType']; Speed = $p['ConnectionSpeed']; ProductId = $p['ProductID'] }
    }
    return $list
}

# iPhone の port 番につなぐ。そのまま USB の通り道になる
function Connect-Phone([int64]$dev, [int]$port) {
    $c = Connect-Tcp '127.0.0.1' $script:MuxPort 1500
    if (-not $c) { return @{ Ok = $false; Why = 'mux' } }
    $pn = (($port -band 0xFF) -shl 8) -bor (($port -shr 8) -band 0xFF)
    try { $r = Invoke-Mux $c 'Connect' ("<key>DeviceID</key><integer>$dev</integer><key>PortNumber</key><integer>$pn</integer>") }
    catch { $c.Close(); return @{ Ok = $false; Why = 'mux' } }
    if ($r['Number'] -ne 0) { $c.Close(); return @{ Ok = $false; Why = 'refused'; Number = $r['Number'] } }
    return @{ Ok = $true; Client = $c }
}

# 受け手にあいさつ（VR_HELLO）して、GPU の名前とメモリを聞く。受け手は一度に 1 人しか相手をしないので、
# ほかのアプリが使っている間は返事が来ない（busy）。版が違うと VK_ERROR_INCOMPATIBLE_DRIVER（-9）が返る
function Invoke-Hello($client, [int]$timeoutMs = 3000) {
    $s = $client.GetStream()
    $s.ReadTimeout = $timeoutMs
    $s.WriteTimeout = $timeoutMs
    $req = New-Object byte[] 12
    [BitConverter]::GetBytes([uint32]1).CopyTo($req, 0)
    [BitConverter]::GetBytes([uint32]4).CopyTo($req, 4)
    [BitConverter]::GetBytes([uint32]$script:ProtoVersion).CopyTo($req, 8)
    try { $s.Write($req, 0, 12); $h = Read-Exact $s 8 }
    catch { return @{ Ok = $false; Why = $(if (Test-Closed $_.Exception) { 'closed' } else { 'timeout' }) } }
    $res = [BitConverter]::ToInt32($h, 0)
    $n = [BitConverter]::ToUInt32($h, 4)
    if ($res -ne 0) { return @{ Ok = $false; Why = 'result'; Result = $res } }
    if ($n -lt 276 -or $n -gt 16MB) { return @{ Ok = $false; Why = 'short' } }
    try { $b = Read-Exact $s ([int]$n) } catch { return @{ Ok = $false; Why = 'closed' } }
    # VkPhysicalDeviceProperties（64 ビットで 824 バイト）の deviceName は 20 バイト目から 256 バイト
    $end = 20
    while ($end -lt 276 -and $b[$end] -ne 0) { $end++ }
    $name = [Text.Encoding]::UTF8.GetString($b, 20, $end - 20)
    # その後ろに VkPhysicalDeviceFeatures（220）と VkPhysicalDeviceMemoryProperties。heap の数は 1044+260、heap[0] は 1044+264
    $mb = 0
    if ($n -ge 1316) {
        $nh = [BitConverter]::ToUInt32($b, 1304)
        for ($i = 0; $i -lt [Math]::Min($nh, 16); $i++) {
            $size = [BitConverter]::ToUInt64($b, 1308 + 16 * $i)
            $flags = [BitConverter]::ToUInt32($b, 1316 + 16 * $i)
            if ($flags -band 1) { $mb += [int64]($size / 1MB) }
        }
    }
    return @{ Ok = $true; Name = $name; MemMB = $mb }
}

# 受け手が答えるかを 1 回だけ見る（つないで、あいさつして、切る）
function Test-Receiver([int64]$dev, [int]$timeoutMs = 3000) {
    $r = Connect-Phone $dev $script:RecvPort
    if (-not $r.Ok) { return $r }
    try { return (Invoke-Hello $r.Client $timeoutMs) } finally { $r.Client.Close() }
}

# ---- USB の太さ（iPhone の 50058 番） ----
# 頼み：向き（u32。0 は iPhone→PC・1 は PC→iPhone）と量（u64）。PC→iPhone のときは最後に受け手が受けた量（u64）を返す
function Send-BwHead($s, [int]$dir, [int64]$n) {
    $h = New-Object byte[] 12
    [BitConverter]::GetBytes([uint32]$dir).CopyTo($h, 0)
    [BitConverter]::GetBytes([uint64]$n).CopyTo($h, 4)
    $s.Write($h, 0, 12)
}
function Invoke-BwTransfer($s, [int]$dir, [int64]$n, [byte[]]$buf) {
    Send-BwHead $s $dir $n
    if ($dir -eq 0) {
        $left = $n
        while ($left -gt 0) {
            $k = $s.Read($buf, 0, [int][Math]::Min([int64]$buf.Length, $left))
            if ($k -le 0) { throw 'closed' }
            $left -= $k
        }
    } else {
        $left = $n
        while ($left -gt 0) {
            $k = [int][Math]::Min([int64]$buf.Length, $left)
            $s.Write($buf, 0, $k)
            $left -= $k
        }
        $back = Read-Exact $s 8
        if ([BitConverter]::ToUInt64($back, 0) -ne [uint64]$n) { throw 'count mismatch' }
    }
}
function Measure-UsbSpeed([int64]$dev, [int]$mb = 32) {
    $r = Connect-Phone $dev $script:BwPort
    if (-not $r.Ok) { return $null }
    $c = $r.Client
    try {
        $c.ReceiveBufferSize = 4MB
        $c.SendBufferSize = 4MB
        $s = $c.GetStream()
        $s.ReadTimeout = 20000
        $s.WriteTimeout = 20000
        $buf = New-Object byte[] 1MB
        # 往復：1 バイトを 20 回
        $t = New-Object Collections.Generic.List[double]
        for ($i = 0; $i -lt 20; $i++) {
            $sw = [Diagnostics.Stopwatch]::StartNew()
            Invoke-BwTransfer $s 0 1 $buf
            $t.Add($sw.Elapsed.TotalMilliseconds)
        }
        $t.Sort()
        $rtt = $t[[int]($t.Count / 2)]
        Invoke-BwTransfer $s 0 (2MB) $buf
        Invoke-BwTransfer $s 1 (2MB) $buf
        $n = [int64]$mb * 1MB
        $sw = [Diagnostics.Stopwatch]::StartNew(); Invoke-BwTransfer $s 0 $n $buf; $down = $mb / $sw.Elapsed.TotalSeconds
        $sw = [Diagnostics.Stopwatch]::StartNew(); Invoke-BwTransfer $s 1 $n $buf; $up = $mb / $sw.Elapsed.TotalSeconds
        return @{ Down = $down; Up = $up; RttMs = $rtt }
    } catch {
        return @{ Error = $_.Exception.Message }
    } finally { $c.Close() }
}

# ---- Windows が USB の iPhone を見ているか（Apple の会社番号 05AC・iPhone と iPad の型は 12xx） ----
function Get-UsbApplePhones {
    try {
        return @(Get-CimInstance Win32_PnPEntity -Filter "PNPDeviceID LIKE 'USB\\VID_05AC&PID_12%'" -ErrorAction Stop |
            Select-Object Name, Status, ConfigManagerErrorCode, PNPDeviceID)
    } catch { return @() }
}

# ---- ドライバに渡す環境変数（キットの pocketgpu.cmd と同じ） ----
function Get-PgpuEnv {
    $e = [ordered]@{}
    $e['VK_ADD_DRIVER_FILES'] = (Join-Path $script:KitDir 'iphone_icd.json') + ';' + (Join-Path $script:KitDir 'x86\iphone_icd.json')
    $e['IPHONE_VK'] = $(if ($env:IPHONE_VK) { $env:IPHONE_VK } else { 'usb' })
    $only = $(if ($env:IPHONE_VK_ONLY) { $env:IPHONE_VK_ONLY } else { '1' })
    if ($only -eq '1') { $e['VK_LOADER_DRIVERS_SELECT'] = '*iphone_icd*' }
    # VK_ADD_DRIVER_FILES と VK_LOADER_DRIVERS_SELECT は Vulkan ローダー 1.3.234 から。古いローダー（10/13 にテスターの 1.3.204）でも効くように、iPhone だけを見せるときは VK_ICD_FILENAMES も付ける
    if ($only -eq '1') { $e['VK_ICD_FILENAMES'] = $e['VK_ADD_DRIVER_FILES'] }
    $e['DISABLE_TWITCH_VULKAN_OVERLAY'] = '1'
    $e['DISABLE_VULKAN_OBS_CAPTURE'] = '1'
    $e['DISABLE_VULKAN_OW_OBS_CAPTURE'] = '1'
    return $e
}

# vklist.exe を、ドライバの環境変数を付けて走らせ、出た GPU の行を返す。窓は出さない
function Invoke-VkList {
    $exe = $(if ($env:PGPU_VKLIST) { $env:PGPU_VKLIST } else { Join-Path $script:KitDir 'vklist.exe' })
    if (-not (Test-Path $exe)) { return @{ Ok = $false; Why = 'missing'; Path = $exe } }
    $psi = New-Object Diagnostics.ProcessStartInfo
    if ($exe -like '*.cmd') { $psi.FileName = $env:ComSpec; $psi.Arguments = '/c "' + $exe + '"' } else { $psi.FileName = $exe }
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $envs = Get-PgpuEnv
    foreach ($k in $envs.Keys) { $psi.EnvironmentVariables[$k] = $envs[$k] }
    # 確かめるときだけ、ほかの層（Steam や OBS の重ね絵など）を読ませない。古いローダーは知らない名前を無視する
    $psi.EnvironmentVariables['VK_LOADER_LAYERS_DISABLE'] = '~implicit~'
    $p = [Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync()
    $er = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(30000)) { try { $p.Kill() } catch { }; return @{ Ok = $false; Why = 'timeout' } }
    $out = $o.Result + $er.Result
    $gpus = @($out -split "`r?`n" | Where-Object { $_ -match '^GPU\d+: ' })
    # PC の GPU（Intel・NVIDIA・AMD・Microsoft・Mesa の CPU）しか見えないときは、iPhone の GPU が見えていない
    $gpus = @($gpus | Where-Object { $_ -notmatch '\(vendor 0x(8086|10de|1002|1414|10005|10004)\b' })
    return @{ Ok = ($gpus.Count -gt 0); Gpus = $gpus; Output = $out; Exit = $p.ExitCode }
}

# ---- exe の中を読む（PE の見出しから、32/64 ビット・取り込む DLL・manifest の管理者の印） ----
function Get-PeInfo([string]$path) {
    $fs = [IO.File]::OpenRead($path)
    try {
        $br = New-Object IO.BinaryReader($fs)
        if ($br.ReadUInt16() -ne 0x5A4D) { return $null }
        $fs.Position = 0x3C
        $pe = $br.ReadInt32()
        $fs.Position = $pe
        if ($br.ReadUInt32() -ne 0x4550) { return $null }
        $machine = $br.ReadUInt16()
        $nsec = $br.ReadUInt16()
        $fs.Position = $pe + 20
        $optSize = $br.ReadUInt16()
        $opt = $pe + 24
        $fs.Position = $opt
        $magic = $br.ReadUInt16()
        $dd = $opt + $(if ($magic -eq 0x20b) { 112 } else { 96 })
        $fs.Position = $dd + 8; $impRva = $br.ReadUInt32()
        $fs.Position = $dd + 16; $rsrcRva = $br.ReadUInt32(); $rsrcSize = $br.ReadUInt32()
        $secs = @()
        $fs.Position = $opt + $optSize
        for ($i = 0; $i -lt $nsec; $i++) {
            [void]$br.ReadBytes(8)
            $vs = $br.ReadUInt32(); $va = $br.ReadUInt32(); $rs = $br.ReadUInt32(); $rp = $br.ReadUInt32()
            [void]$br.ReadBytes(16)
            $secs += , @([int64]$va, [int64][Math]::Max($vs, $rs), [int64]$rp)
        }
        $toOff = { param($rva) foreach ($s in $secs) { if ($rva -ge $s[0] -and $rva -lt $s[0] + $s[1]) { return $rva - $s[0] + $s[2] } }; return -1 }
        $imports = @()
        $o = & $toOff $impRva
        if ($impRva -and $o -ge 0) {
            for ($i = 0; $i -lt 512; $i++) {
                $fs.Position = $o + 20 * $i
                $d = $br.ReadBytes(20)
                if ([BitConverter]::ToUInt32($d, 12) -eq 0) { break }
                $no = & $toOff ([BitConverter]::ToUInt32($d, 12))
                if ($no -lt 0) { continue }
                $fs.Position = $no
                $nb = $br.ReadBytes(64)
                $end = [Array]::IndexOf($nb, [byte]0)
                if ($end -lt 0) { $end = 64 }
                $imports += [Text.Encoding]::ASCII.GetString($nb, 0, $end).ToLowerInvariant()
            }
        }
        $level = ''
        $o = & $toOff $rsrcRva
        if ($rsrcRva -and $o -ge 0) {
            $fs.Position = $o
            $raw = $br.ReadBytes([int][Math]::Min([int64]$rsrcSize, 32MB))
            $txt = [Text.Encoding]::GetEncoding(28591).GetString($raw) + [Text.Encoding]::Unicode.GetString($raw)
            if ($txt -match 'requireAdministrator') { $level = 'requireAdministrator' }
            elseif ($txt -match 'highestAvailable') { $level = 'highestAvailable' }
        }
        return @{ Machine = $machine; Imports = $imports; Level = $level }
    } catch { return $null } finally { $fs.Close() }
}

# 互換性の設定の「管理者としてこのプログラムを実行する」
function Test-CompatRunAsAdmin([string]$exe) {
    foreach ($k in 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers', 'HKLM:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers') {
        try { $v = (Get-ItemProperty -LiteralPath $k -ErrorAction Stop).$exe } catch { $v = $null }
        if ($v -and $v -match 'RUNASADMIN') { return $true }
    }
    return $false
}
