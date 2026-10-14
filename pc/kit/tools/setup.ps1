# PocketGPU を整える：この PC に要る物を見て、足りない物は y/N で聞いてから入れ、iPhone のアプリの入れ方を案内し、最後に iPhone の GPU が見えるところまでチェックする。
# Setup-PocketGPU.cmd から呼ぶ。Windows PowerShell 5.1 だけで動く（Python は要らない）。
# 日本語を含むので UTF-8（BOM 付き）で保存する。BOM が無いと 5.1 は Shift_JIS として読んで壊れる。
# 画面の文は仮。M '日本語' 'English' の組で並べてあるので、言い回しはここで直す。
#
# 流れ：1. この PC をチェック（何も変えない）→ 2. 足りない物を入れる → 3. iPhone のアプリ → 4. 最後のチェック（Start-PocketGPU.cmd と同じ）→ まとめ
# 決まり：
#   - 入れる前に、必ず「y/N」で聞く。y と答えた物だけを入れる。入れるのは winget でだけ（ダウンロードを自分では書かない）
#   - 管理者権限が要る物（iTunes・Vulkan Runtime・Visual C++・Apple のサービスの開始）は、全部の問いのあとで、tools\setup-admin.ps1 に
#     まとめて渡す。UAC の確認は 1 回だけ。管理者が要らない物（Apple Devices・仮想カメラ・キットの更新）は、このウィンドウ（ふつうの権限）で
#   - 管理者のウィンドウに渡すのは、決まった名前の一覧だけ。コマンドは setup-admin.ps1 の中に固定してある
#   - 仮想カメラの登録（HKCU）は、必ずふつうの権限で。管理者で動いているときは何もしないで止まる（HKCU が別のユーザーになるため）
#   - レジストリ（HKLM）・サービスの設定・ドライバ・ネットワークは変えない。AltServer と Apple ID は自動にしない（案内だけ）
#   - 結果は pocketgpu-setup-log.txt と pocketgpu-setup-summary.txt（Discord に貼れる短い文）に書く。ユーザー名・PC 名・フォルダの場所は載せない
#
#   Setup-PocketGPU.cmd               順にチェックして、足りない物は聞いてから入れる
#   Setup-PocketGPU.cmd -CheckOnly    チェックだけ。何も変えない（何を入れるかだけ出す）
#   Setup-PocketGPU.cmd -Yes          聞かれたことすべてに「はい」と答える（UAC の確認と Apple ID は、それでも本人が操作する）
#   終わりのコード：0 iPhone の GPU が見えた（-CheckOnly は足りない物なし）・1 まだ足りない物がある・2 始められない（管理者で動いている・古い Windows）
#
# 試す人は使わない、試験用の差し替え（pgpu_common.ps1 の物に加えて）：
#   PGPU_WINGET=パス               winget の代わり（パスが無ければ「winget がない」。本物を探しにはいかない）
#   PGPU_TEST_STATE_DIR=フォルダ    偽の winget が「入れた」印（<ID>.installed）を置く所。印があれば、入ったことにする
#   PGPU_TEST_ELEVATE_LOG=パス      UAC の代わりに、管理者の部分を管理者なしで動かし、頼んだ名前の一覧をこのファイルに 1 行書く
#   PGPU_TEST_APPLE_DRIVERS=usb,eth|none  Apple の USB ドライバ・Ethernet ドライバが入っていることにする
#   PGPU_TEST_CAMERA_REG=パス       仮想カメラの登録を、レジストリの代わりにこのファイル（camera_setup が書く）で見る
#   PGPU_CAMERA_DIR=フォルダ・PGPU_MICCAM_BIN=フォルダ   Start-Mic-Camera.cmd と同じ（仮想カメラを写す先・道具の置き場）
#   PGPU_TEST_ALTSERVER=パス|none   AltServer.exe の場所（none は入っていない）
#   PGPU_TEST_LAUNCH_LOG=パス       ページ・フォルダ・アプリを開く代わりに、開くはずだった物をこのファイルに書く
#   PGPU_TEST_FINAL_ANSWERS=y,n     最後のチェック（start.ps1）の y/N に、これを答える（無ければ、残りの答え）
#   PGPU_TEST_SERVICE_START=パス    Apple のサービスを開始する代わりに、この .cmd を動かす（setup-admin.ps1 と同じ）
param(
    [switch]$CheckOnly,
    [switch]$Yes
)
. (Join-Path $PSScriptRoot 'pgpu_common.ps1')

$script:SetupVersion = '1'
$script:LoaderMin = [Version]'1.3.234'
$script:Total = 4
$script:CheckOnly = [bool]$CheckOnly
$script:YesAll = [bool]$Yes
$script:Rows = New-Object System.Collections.Generic.List[object]
$script:Ctx = @{}
$script:SelfHash = ''
try { $script:SelfHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash } catch { }

# ---- 小さな道具 ----
# 表に載せる行。state：ok 問題なし・done 入れた・failed 入れられなかった・todo 足りない（まだ）・skipped しなかった・note 気をつけたい
# 同じ物の行（例：Apple の部品の「足りない」→「入れた」）は、後から来た行で置き換える。-CheckOnly では「しなかった」は載せない（聞いていないため）
function Get-RowKey([string]$title) {
    if ($title -match '^(Apple Devices|Apple software|iTunes)') { return 'apple' }
    if ($title -match '^(Apple service|Apple Mobile Device Service)') { return 'applesvc' }
    if ($title -match '^Vulkan') { return 'vulkan' }
    if ($title -match '^Visual C\+\+') { return 'vc' }
    if ($title -match '^Virtual camera') { return 'camera' }
    if ($title -match '^PocketGPU update') { return 'update' }
    if ($title -eq 'iPhone app') { return 'iphoneapp' }
    return $title
}
function Add-Row([string]$state, [string]$title, [string]$note = '') {
    if ($state -eq 'skipped' -and $script:CheckOnly) { return }
    $key = Get-RowKey $title
    for ($i = $script:Rows.Count - 1; $i -ge 0; $i--) { if ((Get-RowKey $script:Rows[$i].Title) -eq $key) { $script:Rows.RemoveAt($i) } }
    $script:Rows.Add([pscustomobject]@{ State = $state; Title = $title; Note = $note })
}
function Confirm-Step([string]$q) {
    if ($script:CheckOnly) { return $false }
    if ($script:YesAll) { Out-Line ('      ' + $q + ' -> ' + (M 'はい' 'yes')); return $true }
    return (Ask-YesNo $q)
}
function Test-StateMarker([string]$id) {
    return [bool]($env:PGPU_TEST_STATE_DIR -and (Test-Path -LiteralPath (Join-Path $env:PGPU_TEST_STATE_DIR ($id + '.installed'))))
}
# 外のプログラムを窓を出さずに動かし、出力と終わりのコードを返す（.cmd は cmd.exe 経由）
function Invoke-Capture([string]$exe, [string]$arguments, [int]$timeoutMs = 20000, $envs = $null, [switch]$Oem) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    if ($exe -like '*.cmd') { $psi.FileName = $env:ComSpec; $psi.Arguments = '/c ""' + $exe + '" ' + $arguments + '"' }
    else { $psi.FileName = $exe; $psi.Arguments = $arguments }
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $enc = $(if ($Oem) { [Text.Encoding]::GetEncoding([Globalization.CultureInfo]::CurrentCulture.TextInfo.OEMCodePage) } else { [Text.Encoding]::UTF8 })
    $psi.StandardOutputEncoding = $enc
    $psi.StandardErrorEncoding = $enc
    if ($envs) { foreach ($k in $envs.Keys) { $psi.EnvironmentVariables[[string]$k] = [string]$envs[$k] } }
    try { $p = [Diagnostics.Process]::Start($psi) } catch { return @{ Ok = $false; Exit = -1; Out = ''; Err = $_.Exception.Message; TimedOut = $false } }
    $o = $p.StandardOutput.ReadToEndAsync()
    $er = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($timeoutMs)) { try { $p.Kill() } catch { }; return @{ Ok = $false; Exit = -1; Out = ''; Err = 'timeout'; TimedOut = $true } }
    [void]$p.WaitForExit()
    return @{ Ok = ($p.ExitCode -eq 0); Exit = $p.ExitCode; Out = $o.Result; Err = $er.Result; TimedOut = $false }
}
# ページ・フォルダ・アプリを開く。試験では開かずに記録だけする
function Invoke-Launch([string]$kind, [string]$target) {
    if ($env:PGPU_TEST_LAUNCH_LOG) { Add-Content -LiteralPath $env:PGPU_TEST_LAUNCH_LOG -Value ($kind + ' ' + $target); return $true }
    try {
        if ($kind -eq 'folder') { Start-Process -FilePath 'explorer.exe' -ArgumentList ('/select,"' + $target + '"') }
        else { Start-Process -FilePath $target }
        return $true
    } catch { Say-Warn ((M '開けませんでした：' 'Could not open it: ') + $_.Exception.Message); return $false }
}
function Offer-Page([string]$url, [string]$question) {
    Say-Info ((M '開くページ：' 'Page: ') + $url)
    if (Confirm-Step $question) { [void](Invoke-Launch 'url' $url) }
}

function Finish([int]$code) {
    # start.ps1 のログは、ユーザー名を「(user)」に直してあるので、そちらの形の場所も伏せる
    $masked = $script:KitDir
    if ($env:USERNAME) { $masked = $masked -replace [regex]::Escape($env:USERNAME), '(user)' }
    if ($env:COMPUTERNAME) { $masked = $masked -replace [regex]::Escape($env:COMPUTERNAME), '(pc)' }
    for ($i = 0; $i -lt $script:Log.Count; $i++) { $script:Log[$i] = $script:Log[$i].Replace($script:KitDir, '<kit>').Replace($masked, '<kit>') }
    $p = Save-Log 'pocketgpu-setup-log.txt'
    Out-Line ''
    Out-Line ((M 'ログ：' 'Log: ') + (Split-Path -Leaf $p))
    if (-not $script:NoPrompt -and $env:PGPU_NOPAUSE -ne '1') { [void](Read-Host (M 'Enter キーを押すと閉じます' 'Press Enter to close')) }
    exit $code
}

# ---- 見る：winget ----
function Get-WingetInfo {
    $p = $env:PGPU_WINGET
    if (-not $p) { $c = Get-Command winget.exe -ErrorAction SilentlyContinue | Select-Object -First 1; if ($c) { $p = $c.Source } }
    if (-not $p -or -not (Test-Path -LiteralPath $p -PathType Leaf)) { return @{ Ok = $false; Path = ''; Version = '' } }
    $r = Invoke-Capture $p '--version' 30000
    $v = ''
    if ($r.Out -match '(\d+\.\d+(\.\d+)*)') { $v = $Matches[1] }
    return @{ Ok = ($r.Exit -eq 0 -and [bool]$v); Path = $p; Version = $v }
}

# ---- 見る：Apple ----
function Get-AppleState {
    $sw = Get-AppleSoftware
    if ($env:PGPU_TEST_STATE_DIR) {
        if (Test-StateMarker '9NP83LWLPZ9K') { $sw.AppleDevices = [pscustomobject]@{ PackageFamilyName = 'AppleInc.AppleDevices_nzyj5cx40ttqa'; Aumid = 'AppleInc.AppleDevices_nzyj5cx40ttqa!App' } }
        if (Test-StateMarker 'Apple.iTunes') { $sw.Service = [pscustomobject]@{ Status = 'Running' } }
    }
    $kind = ''
    if ($sw.AppleDevices) { $kind = 'Apple Devices' } elseif ($sw.Service) { $kind = 'iTunes' } elseif ($sw.StoreITunes) { $kind = 'iTunes (Store)' }
    return @{ Sw = $sw; Kind = $kind; Installed = [bool]$kind; MuxUp = (Test-MuxListening) }
}
# Apple の USB ドライバ（usbaapl64.inf）と Ethernet ドライバ（netaapl64.inf）が、ドライバストアにあるか。読むだけ（pnputil /enum-drivers）。分からなければ $null
function Get-AppleDrivers {
    if ($env:PGPU_TEST_APPLE_DRIVERS) { $t = $env:PGPU_TEST_APPLE_DRIVERS; return @{ Usb = [bool]($t -match 'usb'); Eth = [bool]($t -match 'eth') } }
    $r = Invoke-Capture (Join-Path $env:WINDIR 'System32\pnputil.exe') '/enum-drivers' 40000 $null -Oem
    if ($r.TimedOut -or -not $r.Out) { return @{ Usb = $null; Eth = $null } }
    return @{ Usb = [bool]($r.Out -match '(?i)usbaapl(64)?\.inf'); Eth = [bool]($r.Out -match '(?i)netaapl(64)?\.inf') }
}

# ---- 見る：Vulkan のローダー ----
function Get-LoaderState {
    $v64 = Get-VulkanLoaderVersion
    $v32 = $null
    if ($env:PGPU_TEST_VK_LOADER) { $v32 = $v64 }
    else {
        $d = Join-Path $env:WINDIR 'SysWOW64\vulkan-1.dll'
        if (Test-Path -LiteralPath $d) { $vi = (Get-Item -LiteralPath $d).VersionInfo; $v32 = New-Object Version($vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart, $vi.FilePrivatePart) }
    }
    if (Test-StateMarker 'KhronosGroup.VulkanRT') { $v64 = [Version]'1.4.363.0'; $v32 = $v64 }
    return @{ V64 = $v64; V32 = $v32 }
}

# ---- 見る：Visual C++ の再頒布（キットの exe・dll が取り込む DLL を読んで、要るかを決める）----
# Visual Studio 2015〜の部品（vcruntime140.dll・msvcp140.dll など）を取り込んでいて、その DLL が Windows にも exe の横にも無いときだけ「要る」。
# UCRT（api-ms-win-crt-*）は Windows 10 以降に入っているので、要らない。管理された exe（mscoree.dll）は .NET Framework が要る（Windows 10 以降に入っている）
function Get-VcState {
    $kit = $script:KitDir
    $targets = New-Object System.Collections.Generic.List[string]
    foreach ($glob in '*.exe', '*.dll') { foreach ($f in @(Get-ChildItem -LiteralPath $kit -Filter $glob -File -ErrorAction SilentlyContinue)) { $targets.Add($f.FullName) } }
    foreach ($sub in 'x86', 'extras', 'camera') {
        $d = Join-Path $kit $sub
        if (Test-Path -LiteralPath $d) { foreach ($f in @(Get-ChildItem -LiteralPath $d -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in '.exe', '.dll' })) { $targets.Add($f.FullName) } }
    }
    $vcPattern = '^(vcruntime|msvcp|vcomp|concrt|vccorlib|mfc)1[4-9]\d(_\w+)?\.dll$'
    $s = @{ Scanned = 0; UsesUcrt = $false; Managed = $false; NeedX64 = $false; NeedX86 = $false; Missing = @() }
    $miss = New-Object System.Collections.Generic.List[string]
    foreach ($t in $targets) {
        $pe = Get-PeInfo $t
        if (-not $pe) { continue }
        $s.Scanned++
        foreach ($imp in $pe.Imports) {
            if ($imp -like 'api-ms-win-crt-*' -or $imp -eq 'ucrtbase.dll') { $s.UsesUcrt = $true }
            if ($imp -eq 'mscoree.dll') { $s.Managed = $true }
            if ($imp -notmatch $vcPattern) { continue }
            $sys = $(if ($pe.Machine -eq 0x14c) { Join-Path $env:WINDIR 'SysWOW64' } else { Join-Path $env:WINDIR 'System32' })
            if ((Test-Path -LiteralPath (Join-Path $sys $imp)) -or (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $t) $imp))) { continue }
            if ($pe.Machine -eq 0x14c) { $s.NeedX86 = $true } else { $s.NeedX64 = $true }
            $miss.Add($imp + ' <- ' + (Split-Path -Leaf $t))
        }
    }
    if (Test-StateMarker 'Microsoft.VCRedist.2015+.x64') { $s.NeedX64 = $false }
    if (Test-StateMarker 'Microsoft.VCRedist.2015+.x86') { $s.NeedX86 = $false }
    $s.Missing = @($miss)
    return $s
}

# ---- 見る：仮想カメラ（Start-Mic-Camera.cmd と同じ。HKCU） ----
$script:CamClsid = '{82069F97-3CA9-4626-AD74-D2AA50BBD618}'
$script:CatClsid = '{860BB310-5D01-11D0-BD3B-00A0C911CE86}'
function Find-CamTool([string]$name) {
    $dirs = @()
    if ($env:PGPU_MICCAM_BIN) { $dirs += $env:PGPU_MICCAM_BIN }
    $dirs += (Join-Path $script:KitDir 'extras'), (Join-Path $script:KitDir 'camera'), $script:KitDir
    foreach ($d in $dirs) { $p = Join-Path $d $name; if (Test-Path -LiteralPath $p) { return $p } }
    return $null
}
function Get-CamReg {
    if ($env:PGPU_TEST_CAMERA_REG) {
        $m = $env:PGPU_TEST_CAMERA_REG
        if (-not (Test-Path -LiteralPath $m)) { return @{ Registered = $false; Path = $null; FileOk = $false } }
        $p = ([IO.File]::ReadAllText($m)).Trim()
        return @{ Registered = $true; Path = $p; FileOk = [bool]($p -and (Test-Path -LiteralPath $p)) }
    }
    $path = $null
    try { $path = (Get-ItemProperty -LiteralPath ('HKCU:\Software\Classes\CLSID\' + $script:CamClsid + '\InprocServer32') -ErrorAction Stop).'(default)' } catch { }
    $inst = Test-Path -LiteralPath ('HKCU:\Software\Classes\CLSID\' + $script:CatClsid + '\Instance\' + $script:CamClsid)
    return @{ Registered = [bool]($path -and $inst); Path = $path; FileOk = [bool]($path -and (Test-Path -LiteralPath $path)) }
}
function Test-CamInKit { return [bool]((Find-CamTool 'camera_setup.exe') -and (Find-CamTool 'dshow_cam64.dll') -and (Find-CamTool 'dshow_cam32.dll')) }
# 動かない場所（%LOCALAPPDATA%\PocketGPU\camera）に写してから、いまのユーザー（HKCU）に登録する。Start-Mic-Camera.cmd と同じ
function Install-Camera {
    $src = @()
    foreach ($n in 'camera_setup.exe', 'dshow_cam64.dll', 'dshow_cam32.dll') {
        $s = Find-CamTool $n
        if (-not $s) { return $false }
        $src += $s
    }
    $dir = $(if ($env:PGPU_CAMERA_DIR) { $env:PGPU_CAMERA_DIR } else { Join-Path $env:LOCALAPPDATA 'PocketGPU\camera' })
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($s in $src) {
        $d = Join-Path $dir (Split-Path -Leaf $s)
        if ((Test-Path -LiteralPath $d) -and (Get-FileHash -LiteralPath $d).Hash -eq (Get-FileHash -LiteralPath $s).Hash) { continue }
        try { Copy-Item -LiteralPath $s -Destination $d -Force }
        catch { Say-Warn (M 'カメラのファイルを更新できませんでした。カメラを使っているアプリを終了してから、もう一度試してください。' 'Could not update the camera files. Quit every app that uses the camera, then try again.'); return $false }
    }
    & (Join-Path $dir 'camera_setup.exe') install | Out-Null
    return ($LASTEXITCODE -eq 0)
}

# ---- 見る：DXVK・キットの主なファイル ----
function Get-MissingKitFiles([string[]]$rel) {
    return @($rel | Where-Object { -not (Test-Path -LiteralPath (Join-Path $script:KitDir $_) -PathType Leaf) })
}
$script:DxvkFiles = @(foreach ($a in 'x64', 'x32') { foreach ($n in 'd3d9.dll', 'd3d10core.dll', 'd3d11.dll', 'dxgi.dll') { 'dxvk\' + $a + '\' + $n } })
$script:CoreFiles = @('iphone_icd.dll', 'iphone_icd.json', 'x86\iphone_icd.dll', 'x86\iphone_icd.json', 'vklist.exe')

# ---- 見る：キットの更新（update.ps1 -Check。0 最新・10 新しい版あり・2 情報なし・3 合言葉が使えない・1 失敗）----
function Get-UpdateState {
    $upd = Join-Path $script:KitDir 'tools\update.ps1'
    if (-not (Test-Path -LiteralPath $upd)) { return @{ Code = -2 } }
    if (-not (Test-Path -LiteralPath (Join-Path $script:KitDir 'tools\update.ini'))) { return @{ Code = 2 } }
    $r = Invoke-Capture (Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe') ('-NoProfile -ExecutionPolicy Bypass -File "' + $upd + '" -Check -KitDir "' + $script:KitDir + '"') 120000 @{ PGPU_NOPAUSE = '1' }
    return @{ Code = $(if ($r.TimedOut) { 1 } else { $r.Exit }) }
}
function Invoke-KitUpdate {
    $upd = Join-Path $script:KitDir 'tools\update.ps1'
    $args2 = '-NoProfile -ExecutionPolicy Bypass -File "' + $upd + '" -Yes -KitDir "' + $script:KitDir + '"'
    $saved = $env:PGPU_NOPAUSE
    $env:PGPU_NOPAUSE = '1'
    try {
        $p = Start-Process -FilePath (Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe') -ArgumentList $args2 -NoNewWindow -Wait -PassThru
        return $p.ExitCode
    } finally { $env:PGPU_NOPAUSE = $saved }
}

# ---- 見る：iPhone のアプリ・AltServer・IPA ----
# State：ok 応答した・mismatch 版が合わない・busy 他のプログラムが GPU を使っている・noanswer 答えない（アプリが開いていないか、入っていない）・nodevice iPhone が USB で見えない・nomux Apple のサービスがない
# 入っているか・版は、Apple のサービス越しには読めない。応答したら入っていて、キットと同じ約束の版（proto）で動いている
function Get-IphoneAppState {
    $devs = Get-MuxDevices
    if ($null -eq $devs) { return @{ State = 'nomux' } }
    $usb = @($devs | Where-Object { $_.Type -eq 'USB' })
    if ($usb.Count -eq 0) { return @{ State = 'nodevice' } }
    $h = Test-Receiver $usb[0].Id 3000
    if ($h.Ok) { return @{ State = 'ok'; Name = $h.Name; MemMB = $h.MemMB } }
    if ($h.Why -eq 'timeout') { return @{ State = 'busy' } }
    if ($h.Why -eq 'result' -and $h.Result -eq -9) { return @{ State = 'mismatch' } }
    return @{ State = 'noanswer' }
}
function Get-AltServerInfo {
    if ($env:PGPU_TEST_ALTSERVER) {
        if ($env:PGPU_TEST_ALTSERVER -eq 'none') { return @{ Path = $null; Running = $false } }
        return @{ Path = $env:PGPU_TEST_ALTSERVER; Running = $false }
    }
    $path = $null
    foreach ($c in (Join-Path ${env:ProgramFiles(x86)} 'AltServer\AltServer.exe'), (Join-Path $env:ProgramFiles 'AltServer\AltServer.exe')) {
        if ($c -and (Test-Path -LiteralPath $c)) { $path = $c; break }
    }
    return @{ Path = $path; Running = [bool](Get-Process -Name AltServer -ErrorAction SilentlyContinue) }
}
function Get-KitIpa([string]$prefix) {
    $best = $null
    $bv = $null
    foreach ($f in @(Get-ChildItem -LiteralPath $script:KitDir -Filter ($prefix + '-*.ipa') -File -ErrorAction SilentlyContinue)) {
        if ($f.Name -notmatch ('^' + [regex]::Escape($prefix) + '-(\d+)\.(\d+)\.(\d+)\.ipa$')) { continue }
        $v = New-Object Version([int]$Matches[1], [int]$Matches[2], [int]$Matches[3])
        if ($null -eq $bv -or $v -gt $bv) { $bv = $v; $best = $f.FullName }
    }
    return $best
}

# ---- 入れる：winget（管理者が要らない物）と、管理者の部分 ----
function Invoke-WingetInstall([string]$id, [string]$source) {
    $a = @('install', '--id', $id, '--source', $source, '--accept-package-agreements', '--accept-source-agreements', '--silent')
    $script:Log.Add('      winget ' + ($a -join ' '))
    $code = -1
    # winget の表示（進み具合）は画面に出す。関数の戻り値には混ぜない（Out-Host）
    try { & $script:Wg.Path @a | Out-Host; $code = $LASTEXITCODE } catch { $script:Log.Add('      ' + $_.Exception.Message) }
    $script:Log.Add('      -> exit ' + $code)
    # 0 成功・0x8A150061 すでに入っている・0x8A15002B 更新なし
    return ($code -eq 0 -or $code -eq -1978335135 -or $code -eq -1978335189)
}
# 管理者権限が要る物を、1 回の UAC でまとめて渡す。終わりのコード：0 すべて成功・1 失敗あり・3 winget がない・-1 UAC を断った（起動できなかった）
function Invoke-AdminBatch([string[]]$actions) {
    $helper = Join-Path $PSScriptRoot 'setup-admin.ps1'
    if (-not (Test-Path -LiteralPath $helper)) { return -2 }
    $argLine = '-NoProfile -ExecutionPolicy Bypass -File "' + $helper + '" -Actions ' + ($actions -join ',') + ' -Winget "' + $script:Wg.Path + '"'
    if (-not $script:NoPrompt) { $argLine += ' -Pause' }
    $psExe = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if ($env:PGPU_TEST_ELEVATE_LOG) {
        Add-Content -LiteralPath $env:PGPU_TEST_ELEVATE_LOG -Value ('UAC ' + ($actions -join ','))
        $p = Start-Process -FilePath $psExe -ArgumentList $argLine -WindowStyle Hidden -Wait -PassThru
        return $p.ExitCode
    }
    try { $p = Start-Process -FilePath $psExe -ArgumentList $argLine -Verb RunAs -Wait -PassThru }
    catch { return -1 }
    return $p.ExitCode
}

# ---- まとめ ----
function Get-PcInfo {
    $os = [Environment]::OSVersion.Version
    $win = $(if ($os.Build -ge 22000) { 'Windows 11' } else { 'Windows 10' })
    $gpus = ''
    try { $gpus = (@(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object { $_.Name.Trim() }) -join ', ') } catch { }
    $lv = (Get-LoaderState).V64
    return ('{0} (build {1}) 64-bit / GPU: {2} / Vulkan loader: {3} / winget: {4} / Apple: {5}' -f $win, $os.Build, $(if ($gpus) { $gpus } else { '?' }), $(if ($lv) { $lv } else { '-' }), $(if ($script:Wg.Ok) { $script:Wg.Version } else { '-' }), $(if ($script:Ctx.AppleKind) { $script:Ctx.AppleKind } else { '-' }))
}

# ==================================================================== ここから
Out-Line (M 'PocketGPU をセットアップします' 'Set up PocketGPU')
Out-Line (M 'この PC に必要な物をチェックして、足りない物は y/N で聞いてからインストールします。' 'This checks what this PC needs and installs what is missing, asking y/N first.')
Out-Line (M '管理者権限が必要な物は、まとめて UAC の確認を 1 回だけ行います。' 'Things that need administrator rights are asked about once, together (one UAC prompt).')
if ($script:CheckOnly) { Out-Line (M '-CheckOnly：チェックだけを行います。何も変更しません。' '-CheckOnly: only checks. Nothing is changed.') 'Yellow' }

# ==== 1. この PC をチェック ====
Say-Step 1 $script:Total (M 'この PC をチェック' 'Check this PC')
$os = [Environment]::OSVersion.Version
if (-not [Environment]::Is64BitOperatingSystem -or $os.Major -lt 10) {
    Say-NG (M '64 ビット版の Windows 10 または 11 が必要です。' 'A 64-bit Windows 10 or 11 is needed.')
    Finish 2
}
$winName = $(if ($os.Build -ge 22000) { 'Windows 11' } else { 'Windows 10' })
Say-OK ((M 'Windows：{0}・ビルド {1}・64 ビット' 'Windows: {0}, build {1}, 64-bit') -f $winName, $os.Build)
if (Test-Elevated) {
    Say-NG (M 'このウィンドウは管理者権限で実行されています。' 'This window is running as administrator.')
    Say-Info (M '仮想カメラの登録と iPhone の GPU のチェックは、ふつうの権限が必要です。このウィンドウを閉じて、「管理者として実行」を使わずに Setup-PocketGPU.cmd をダブルクリックし直してください。' 'The virtual camera and the iPhone GPU check need normal rights. Close this window and double-click Setup-PocketGPU.cmd again, without "Run as administrator".')
    Say-Info (M '管理者権限が必要な物は、必要になったときに自動で UAC の確認が出ます。' 'When something needs administrator rights, the UAC prompt appears by itself.')
    Add-Row 'failed' 'This PC' 'running as administrator'
    Finish 2
}
if (Test-UacOff) {
    Say-Warn (M 'ユーザーアカウント制御が無効です。すべてのアプリが管理者権限で実行されるため、iPhone の GPU を認識できません。' 'User Account Control is off, so every program runs as administrator. The iPhone GPU cannot be seen this way.')
    Add-Row 'note' 'UAC is off'
}
if (-not (Test-AnsiPath $script:KitDir)) {
    Say-Warn (M 'このフォルダの場所に、Windows の言語設定で表示できない文字が含まれています。フォルダごと C:\PocketGPU に移動してください。' 'The path of this folder has characters that the Windows language setting cannot show. Move the whole folder to C:\PocketGPU.')
    Add-Row 'note' 'Kit folder path has characters outside the ANSI code page'
}

# -- winget --
$script:Wg = Get-WingetInfo
if ($script:Wg.Ok) { Say-OK ('winget ' + $script:Wg.Version) }
else { Say-NG (M 'winget がありません。インストールは自動でできません。' 'winget is not available. Installing cannot be done automatically.'); Add-Row 'todo' 'winget' 'not available' }

# -- Apple の部品 --
$ap = Get-AppleState
$script:Ctx.AppleKind = $ap.Kind
if ($ap.Installed) {
    Say-OK ((M 'Apple：{0} がインストールされています。' 'Apple: {0} is installed.') -f $ap.Kind)
    Add-Row 'ok' 'Apple software' $ap.Kind
} else {
    Say-NG (M 'Apple：Apple Devices も iTunes もインストールされていません。' 'Apple: neither Apple Devices nor iTunes is installed.')
    Add-Row 'todo' 'Apple software' 'Apple Devices / iTunes missing'
}
if ($ap.MuxUp) { Say-OK ((M 'Apple のサービス：実行中（ポート {0}）' 'Apple service: running (port {0})') -f $script:MuxPort); Add-Row 'ok' ('Apple service (port ' + $script:MuxPort + ')') }
elseif ($ap.Installed) { Say-Warn (M 'Apple のサービス：実行されていません（手順 4 で起動を案内します）' 'Apple service: not running (step 4 helps to start it)'); Add-Row 'todo' 'Apple service' 'not running' }
else { Say-NG (M 'Apple のサービス：実行されていません。' 'Apple service: not running.') }
$drv = Get-AppleDrivers
if ($null -eq $drv.Usb) { Say-Info (M 'Apple の USB ドライバ：チェックできませんでした。' 'Apple USB driver: could not check.') }
elseif ($drv.Usb) { Say-OK (M 'Apple の USB ドライバ：あります。' 'Apple USB driver: present.'); Add-Row 'ok' 'Apple USB driver' }
elseif ($ap.Installed) { Say-Warn (M 'Apple の USB ドライバ：見つかりません。iPhone を初めて接続したとき、または Apple Devices を起動したときに入ります。' 'Apple USB driver: not found. It is added when the iPhone is first connected or Apple Devices is opened.'); Add-Row 'note' 'Apple USB driver' 'not found yet' }
else { Say-NG (M 'Apple の USB ドライバ：ありません。Apple Devices または iTunes に含まれます。' 'Apple USB driver: missing. It comes with Apple Devices or iTunes.') }
if ($null -ne $drv.Eth) {
    if ($drv.Eth) { Say-OK (M 'Apple Mobile Device Ethernet：あります。' 'Apple Mobile Device Ethernet: present.') }
    else { Say-Info (M 'Apple Mobile Device Ethernet：ありません。USB テザリング用なので、PocketGPU には不要です。' 'Apple Mobile Device Ethernet: not found. It is for USB tethering and PocketGPU does not need it.') }
}
$pnp = @(Get-UsbApplePhones)
if ($pnp.Count -gt 0) {
    if (@($pnp | Where-Object { $_.ConfigManagerErrorCode -ne 0 }).Count -gt 0) { Say-Warn (M 'Windows は iPhone を認識していますが、Apple の USB ドライバが動作していません。' 'Windows sees the iPhone, but the Apple USB driver is not working.'); Add-Row 'note' 'iPhone USB driver' 'not working' }
    else { Say-OK (M 'iPhone：Windows が USB で認識しています。' 'iPhone: Windows sees it over USB.') }
} else { Say-Info (M 'iPhone：いま USB で見えません（手順 3 と 4 で接続します）。' 'iPhone: not seen over USB now (steps 3 and 4 use it).') }

# -- Vulkan のローダー --
$lo = Get-LoaderState
$vkMissing = $false
$vkOld = $false
if (-not $lo.V64) { Say-NG (M 'Vulkan ローダー（vulkan-1.dll）：ありません。' 'Vulkan loader (vulkan-1.dll): missing.'); $vkMissing = $true; Add-Row 'todo' 'Vulkan loader' 'missing' }
elseif ($lo.V64 -lt $script:LoaderMin) {
    Say-Warn ((M 'Vulkan ローダー：{0}（{1} より古い）' 'Vulkan loader: {0} (older than {1})') -f $lo.V64, $script:LoaderMin)
    Say-Info (M 'キットの入口が VK_ICD_FILENAMES でドライバを渡すので、古いままでも動きます。' 'The kit entries pass the driver with VK_ICD_FILENAMES, so an old loader still works.')
    $vkOld = $true
    Add-Row 'note' 'Vulkan loader' ('old (' + $lo.V64 + '); the kit works with it')
} else { Say-OK ((M 'Vulkan ローダー：{0}' 'Vulkan loader: {0}') -f $lo.V64); Add-Row 'ok' ('Vulkan loader ' + $lo.V64) }
if ($lo.V64 -and -not $lo.V32) { Say-Info (M '32 ビット版の vulkan-1.dll がありません。32 ビットのゲームには Vulkan Runtime が必要です。' 'The 32-bit vulkan-1.dll is missing. 32-bit games need the Vulkan Runtime.') }

# -- Visual C++ の再頒布 --
$vc = Get-VcState
if ($vc.NeedX64 -or $vc.NeedX86) {
    Say-NG ((M 'Visual C++ の再頒布：必要です（{0}）' 'Visual C++ Redistributable: needed ({0})') -f ($vc.Missing -join ', '))
    Add-Row 'todo' 'Visual C++ Redistributable' ($vc.Missing -join ', ')
} else {
    $why = $(if ($vc.UsesUcrt) { M 'Windows 標準の UCRT だけを使います。' 'Only the Windows built-in UCRT is used.' } else { '' })
    Say-OK ((M 'Visual C++ の再頒布：不要（{0} 個のファイルを確認）{1}' 'Visual C++ Redistributable: not needed ({0} files checked). {1}') -f $vc.Scanned, $why)
    Add-Row 'ok' 'Visual C++ Redistributable' 'not needed'
}
if ($vc.Managed -and -not (Test-Path -LiteralPath (Join-Path $env:WINDIR 'System32\mscoree.dll'))) { Say-Warn (M '.NET Framework が見つかりません。PocketGPU.exe に必要です。Windows Update を実行してください。' '.NET Framework was not found. PocketGPU.exe needs it. Run Windows Update.'); Add-Row 'note' '.NET Framework' 'not found' }

# -- 仮想カメラ --
$cam = Get-CamReg
$camTodo = $false
if (-not (Test-CamInKit)) { Say-Info (M '仮想カメラ：このキットには含まれていません。' 'Virtual camera: not in this kit.') }
elseif ($cam.Registered -and $cam.FileOk) { Say-OK (M '仮想カメラ：登録されています。' 'Virtual camera: registered.'); Add-Row 'ok' 'Virtual camera' }
else {
    Say-Warn (M '仮想カメラ：まだ登録されていません。' 'Virtual camera: not registered yet.')
    $camTodo = $true
    Add-Row 'todo' 'Virtual camera' 'not registered'
}

# -- DXVK・キットのファイル --
$core = Get-MissingKitFiles $script:CoreFiles
$dx = Get-MissingKitFiles $script:DxvkFiles
if ($core.Count -gt 0) { Say-NG ((M 'キットのファイルが足りません：{0}' 'Kit files are missing: {0}') -f ($core -join ', ')); Add-Row 'todo' 'Kit files' ($core -join ', ') }
else { Say-OK (M 'キットの主なファイル：そろっています。' 'Main kit files: all present.'); Add-Row 'ok' 'Kit files' }
if ($dx.Count -gt 0) { Say-Warn ((M 'DXVK：{0} 個のファイルがありません。' 'DXVK: {0} files are missing.') -f $dx.Count); Add-Row 'todo' 'DXVK' ('' + $dx.Count + ' files missing') }
else { Say-OK (M 'DXVK：そろっています（x64・x32）。' 'DXVK: all present (x64 and x32).'); Add-Row 'ok' 'DXVK' }

# -- キットの更新 --
$upd = Get-UpdateState
$updAvail = $false
switch ($upd.Code) {
    0 { Say-OK (M 'PocketGPU：最新です。' 'PocketGPU: up to date.'); Add-Row 'ok' 'PocketGPU update' 'up to date' }
    10 { Say-Warn (M 'PocketGPU：新しいバージョンがあります。' 'PocketGPU: a new version is available.'); $updAvail = $true; Add-Row 'todo' 'PocketGPU update' 'new version available' }
    2 { Say-Info (M 'PocketGPU：更新の情報がありません（tools\update.ini がありません）。' 'PocketGPU: no update information (tools\update.ini is missing).') }
    -2 { Say-Info (M 'PocketGPU：更新の道具（tools\update.ps1）がありません。' 'PocketGPU: the updater (tools\update.ps1) is missing.') }
    default { Say-Info ((M 'PocketGPU：更新をチェックできませんでした（コード {0}）。インターネットに接続しているか確認してください。' 'PocketGPU: could not check for updates (code {0}). Check the internet connection.') -f $upd.Code) }
}

# -- iPhone のアプリ --
$app = Get-IphoneAppState
switch ($app.State) {
    'ok' { Say-OK ((M 'iPhone のアプリ：応答しました（{0}）。キットと同じ版の約束で動いています。' 'iPhone app: answered ({0}). It speaks the same protocol version as this kit.') -f $app.Name); Add-Row 'ok' 'iPhone app' $app.Name }
    'mismatch' { Say-NG (M 'iPhone のアプリ：版がこのキットと一致しません。' 'iPhone app: its version does not match this kit.'); Add-Row 'todo' 'iPhone app' 'version does not match' }
    'busy' { Say-Warn (M 'iPhone のアプリ：起動していますが、他のアプリが GPU を使っています。' 'iPhone app: open, but another program is using the GPU.'); Add-Row 'note' 'iPhone app' 'GPU in use by another program' }
    'noanswer' { Say-Warn (M 'iPhone のアプリ：応答しません（開いていないか、まだインストールしていません）。' 'iPhone app: no answer (not open, or not installed yet).'); Add-Row 'todo' 'iPhone app' 'no answer' }
    default { Say-Info (M 'iPhone のアプリ：iPhone が USB で見えないため、チェックできません。' 'iPhone app: cannot check because the iPhone is not seen over USB.') }
}
$alt = Get-AltServerInfo
if ($alt.Path) { Say-OK (M 'AltServer：インストールされています。' 'AltServer: installed.') } else { Say-Info (M 'AltServer：インストールされていません。' 'AltServer: not installed.') }

# ==== 2. 足りない物をインストール ====
Say-Step 2 $script:Total (M '足りない物をインストール' 'Install what is missing')
$userSteps = New-Object System.Collections.Generic.List[hashtable]
$adminSteps = New-Object System.Collections.Generic.List[hashtable]
$needAppleInstall = -not $ap.Installed
$needVkInstall = $vkMissing
$needVc = $vc.NeedX64 -or $vc.NeedX86
$needSvc = [bool]($ap.Sw.Service -and $ap.Sw.Service.Status -ne 'Running' -and -not $ap.MuxUp -and -not $ap.Sw.AppleDevices)
$wantAnything = $needAppleInstall -or $needVkInstall -or $needVc -or $needSvc -or $camTodo -or $updAvail -or $vkOld
$wingetWanted = $needAppleInstall -or $needVkInstall -or $needVc

if (-not $wantAnything -and $dx.Count -eq 0 -and $core.Count -eq 0) {
    Say-OK (M '足りない物はありません。' 'Nothing is missing.')
} else {
    # winget が無いときは、案内だけ
    if ($wingetWanted -and -not $script:Wg.Ok) {
        Say-Info (M 'winget は Microsoft Store の「アプリ インストーラー」に含まれます。更新するか、インストールしてから、もう一度 Setup-PocketGPU.cmd を起動してください。' 'winget comes with "App Installer" in the Microsoft Store. Update or install it, then open Setup-PocketGPU.cmd again.')
        if ($os.Build -lt 17763) { Say-Warn (M 'Windows 10 のバージョン 1809 以降が必要です。Windows Update を実行してください。' 'Windows 10 version 1809 or later is needed. Run Windows Update.') }
        Offer-Page 'ms-windows-store://pdp/?ProductId=9NBLGGH4NNS1' (M '「アプリ インストーラー」のページを開きますか？' 'Open the App Installer page in the Microsoft Store?')
    }
    # Apple の部品
    if ($needAppleInstall) {
        if ($script:Wg.Ok) {
            Say-Info (M 'Apple Devices（Microsoft Store）を winget でインストールします。管理者権限は不要です。' 'Apple Devices (Microsoft Store) is installed with winget. No administrator rights are needed.')
            if (Confirm-Step (M 'Apple Devices をインストールしますか？' 'Install Apple Devices?')) { $userSteps.Add(@{ Key = 'apple-devices'; Title = 'Apple Devices' }) }
            else { Add-Row 'skipped' 'Apple Devices' 'not installed (answered no)' }
        } else {
            Offer-Page 'ms-windows-store://pdp/?ProductId=9NP83LWLPZ9K' (M 'Microsoft Store で Apple Devices のページを開きますか？' 'Open the Apple Devices page in the Microsoft Store?')
            Say-Info (M 'Store が使えないときは、https://www.apple.com/itunes/ から iTunes を入れても使えます。' 'If the Store is not available, iTunes from https://www.apple.com/itunes/ also works.')
        }
    }
    # Vulkan のローダー
    if ($needVkInstall) {
        if ($script:Wg.Ok) {
            if (Confirm-Step (M 'LunarG の Vulkan Runtime をインストールしますか？（管理者権限が必要）' 'Install the LunarG Vulkan Runtime? (needs administrator rights)')) { $adminSteps.Add(@{ Key = 'vulkan-runtime'; Title = 'Vulkan Runtime' }) }
            else { Add-Row 'skipped' 'Vulkan Runtime' 'not installed (answered no)' }
        } else { Offer-Page 'https://vulkan.lunarg.com/sdk/home' (M 'Vulkan Runtime のダウンロードページを開きますか？' 'Open the Vulkan Runtime download page?') }
    } elseif ($vkOld -and $script:Wg.Ok) {
        Say-Info (M 'ローダーが古くても動くので、ふつうは不要です。混在モード（IPHONE_VK_ONLY=0）を使うときだけ必要です。' 'An old loader works, so this is usually not needed. Only mixed mode (IPHONE_VK_ONLY=0) needs a newer one.')
        if (Confirm-Step (M 'それでも新しい Vulkan Runtime をインストールしますか？（管理者権限が必要）' 'Install the newer Vulkan Runtime anyway? (needs administrator rights)')) { $adminSteps.Add(@{ Key = 'vulkan-runtime'; Title = 'Vulkan Runtime (newer)' }) }
    }
    # Visual C++ の再頒布
    if ($needVc) {
        if ($script:Wg.Ok) {
            if (Confirm-Step (M 'Visual C++ 再頒布可能パッケージをインストールしますか？（管理者権限が必要）' 'Install the Visual C++ Redistributable? (needs administrator rights)')) {
                if ($vc.NeedX64) { $adminSteps.Add(@{ Key = 'vcredist-x64'; Title = 'Visual C++ Redistributable x64' }) }
                if ($vc.NeedX86) { $adminSteps.Add(@{ Key = 'vcredist-x86'; Title = 'Visual C++ Redistributable x86' }) }
            } else { Add-Row 'skipped' 'Visual C++ Redistributable' 'not installed (answered no)' }
        } else { Say-Info 'https://aka.ms/vs/17/release/vc_redist.x64.exe' }
    }
    # Apple のサービス（iTunes の Windows サービスが止まっているとき）
    if ($needSvc) {
        if (Confirm-Step (M 'Apple のサービス（Apple Mobile Device Service）を開始しますか？（管理者権限が必要）' 'Start the Apple service (Apple Mobile Device Service)? (needs administrator rights)')) { $adminSteps.Add(@{ Key = 'apple-service'; Title = 'Apple Mobile Device Service' }) }
        else { Add-Row 'skipped' 'Apple service start' 'answered no' }
    }
    # 仮想カメラ
    if ($camTodo) {
        Say-Info (M 'このユーザーにだけ登録します。管理者権限は不要です。' 'It is added for this user only. No administrator rights are needed.')
        if (Confirm-Step (M '仮想カメラを登録しますか？' 'Register the virtual camera?')) { $userSteps.Add(@{ Key = 'camera'; Title = 'Virtual camera' }) }
        else { Add-Row 'skipped' 'Virtual camera' 'not registered (answered no)' }
    }
    # キットの更新
    if ($updAvail) {
        if (Confirm-Step (M 'PocketGPU を更新しますか？（変更されたファイルだけダウンロードします）' 'Update PocketGPU? (only the changed files are downloaded)')) { $userSteps.Add(@{ Key = 'update'; Title = 'PocketGPU update' }) }
        else { Add-Row 'skipped' 'PocketGPU update' 'answered no' }
    }
    if (($dx.Count -gt 0 -or $core.Count -gt 0) -and -not $updAvail) {
        Say-Info (M 'キットのファイルが足りません。Update-PocketGPU.cmd で取り直すか、zip を展開し直してください。' 'Kit files are missing. Get them again with Update-PocketGPU.cmd, or extract the zip again.')
    }
}

if ($script:CheckOnly) {
    Out-Line ''
    Say-Info (M '-CheckOnly のため、ここで終わります。' 'Stopping here because of -CheckOnly.')
    $todo = @($script:Rows | Where-Object { $_.State -eq 'todo' })
    if ($todo.Count -eq 0) { Say-OK (M '足りない物はありません。' 'Nothing is missing.') }
    foreach ($r in $todo) { Say-Info ('- ' + $r.Title + $(if ($r.Note) { ' (' + $r.Note + ')' } else { '' })) }
    Finish $(if ($todo.Count -eq 0) { 0 } else { 1 })
}

# ---- 実行：管理者が要らない物（ふつうの権限）----
foreach ($st in @($userSteps | Where-Object { $_.Key -eq 'update' })) {
    Out-Line ''
    Out-Line (M 'PocketGPU を更新しています。' 'Updating PocketGPU.')
    $code = Invoke-KitUpdate
    $script:Log.Add('      update.ps1 exit ' + $code)
    if ($code -eq 0) {
        Say-OK (M 'PocketGPU を更新しました。' 'PocketGPU was updated.'); Add-Row 'done' 'PocketGPU update'
        $newHash = ''
        try { $newHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash } catch { }
        if ($newHash -and $script:SelfHash -and $newHash -ne $script:SelfHash) { Say-Info (M 'セットアップ自身も新しくなりました。今回はこのまま続けます。終わったあと、もう一度 Setup-PocketGPU.cmd を起動してください。' 'Setup itself was updated. It continues as is this time. When it ends, open Setup-PocketGPU.cmd again.') }
    } else { Say-NG ((M '更新できませんでした（コード {0}）。' 'The update failed (code {0}).') -f $code); Add-Row 'failed' 'PocketGPU update' ('exit ' + $code) }
    $dx = Get-MissingKitFiles $script:DxvkFiles
}
foreach ($st in @($userSteps | Where-Object { $_.Key -eq 'apple-devices' })) {
    Out-Line ''
    Out-Line (M 'Apple Devices をインストールしています。' 'Installing Apple Devices.')
    $ok = Invoke-WingetInstall '9NP83LWLPZ9K' 'msstore'
    $now = Get-AppleState
    if ($ok -and $now.Installed) {
        Say-OK (M 'Apple Devices をインストールしました。' 'Apple Devices was installed.'); Add-Row 'done' 'Apple Devices' 'winget 9NP83LWLPZ9K'
        $script:Ctx.AppleKind = $now.Kind
        Say-Info (M 'iPhone を接続して、Apple Devices を一度開いてください（使用許諾に同意すると、USB ドライバが入ります）。iPhone の画面で「信頼」をタップしてください。' 'Plug in the iPhone and open Apple Devices once (accept its terms; the USB driver is added). Tap Trust on the iPhone.')
    } else {
        if ($ok) { Say-NG (M 'winget は成功と答えましたが、Apple Devices が見つかりません。' 'winget said it worked, but Apple Devices was not found.') }
        else { Say-NG (M 'Apple Devices をインストールできませんでした。Microsoft Store が使えない PC では、この方法は使えません。' 'Apple Devices could not be installed. This does not work on a PC without the Microsoft Store.') }
        Add-Row 'failed' 'Apple Devices' 'winget 9NP83LWLPZ9K'
        if (Confirm-Step (M '代わりに iTunes をインストールしますか？（管理者権限が必要）' 'Install iTunes instead? (needs administrator rights)')) { $adminSteps.Add(@{ Key = 'apple-itunes'; Title = 'iTunes' }) }
    }
}
foreach ($st in @($userSteps | Where-Object { $_.Key -eq 'camera' })) {
    Out-Line ''
    Out-Line (M '仮想カメラを登録しています。' 'Registering the virtual camera.')
    $ok = Install-Camera
    $reg = Get-CamReg
    if ($ok -and $reg.Registered -and $reg.FileOk) { Say-OK (M '「PocketGPU iPhone Camera」をカメラの一覧に追加しました。カメラを使うアプリを開いている場合は、開き直してください。' 'Added "PocketGPU iPhone Camera" to the camera list. If an app that uses the camera is open, restart it.'); Add-Row 'done' 'Virtual camera' }
    else { Say-NG (M '仮想カメラを登録できませんでした。' 'Could not register the virtual camera.'); Add-Row 'failed' 'Virtual camera' }
}

# ---- 実行：管理者が要る物（UAC は 1 回）----
if ($adminSteps.Count -gt 0) {
    Out-Line ''
    Out-Line (M '管理者権限が必要な物をインストールします。UAC の確認は 1 回です。' 'Installing what needs administrator rights. The UAC prompt appears once.')
    foreach ($s in $adminSteps) { Say-Info ('- ' + $s.Title) }
    $code = Invoke-AdminBatch @($adminSteps | ForEach-Object { $_.Key })
    $script:Log.Add('      setup-admin.ps1 exit ' + $code)
    if ($code -eq -1) { Say-NG (M 'UAC の確認が断られたため、管理者権限が必要な物は入りませんでした。' 'The UAC prompt was declined, so the items that need administrator rights were not installed.') }
    elseif ($code -eq 3) { Say-NG (M '管理者ウィンドウから winget が見つかりませんでした。' 'winget was not found from the administrator window.') }
    elseif ($code -ne 0) { Say-Warn (M '管理者ウィンドウで失敗した物があります。下で結果を確認します。' 'Something failed in the administrator window. The results are checked below.') }
    # 効果は、画面の言葉ではなく、実際の状態で確かめる
    $ap2 = Get-AppleState
    $lo2 = Get-LoaderState
    $vc2 = Get-VcState
    foreach ($s in $adminSteps) {
        $ok = $false
        switch ($s.Key) {
            'apple-itunes' { $ok = [bool]($ap2.Sw.Service -or $ap2.Sw.StoreITunes) }
            'vulkan-runtime' { $ok = [bool]($lo2.V64 -and $lo2.V64 -ge $script:LoaderMin) }
            'vcredist-x64' { $ok = -not $vc2.NeedX64 }
            'vcredist-x86' { $ok = -not $vc2.NeedX86 }
            'apple-service' { for ($w = 0; $w -lt 10 -and -not (Test-MuxListening); $w++) { Start-Sleep -Seconds 1 }; $ok = Test-MuxListening }
        }
        if ($ok) { Say-OK ((M '{0}：インストールされました。' '{0}: done.') -f $s.Title); Add-Row 'done' $s.Title }
        else { Say-NG ((M '{0}：確認できませんでした。' '{0}: could not be confirmed.') -f $s.Title); Add-Row 'failed' $s.Title $(if ($code -eq -1) { 'UAC declined' } else { 'not detected after install' }) }
    }
    $script:Ctx.AppleKind = $ap2.Kind
    if ($adminSteps | Where-Object { $_.Key -eq 'apple-itunes' }) {
        if ($ap2.Installed) { Say-Info (M 'iPhone を接続して、ロックを解除し、「このコンピュータを信頼しますか？」で「信頼」をタップしてください。' 'Plug in the iPhone, unlock it, and tap Trust when it asks "Trust This Computer?".') }
        Say-Info (M 'Windows が再起動を求めたときは、再起動してから、Setup-PocketGPU.cmd をもう一度起動してください。' 'If Windows asks to restart, restart it and open Setup-PocketGPU.cmd again.')
    }
}

# ==== 3. iPhone のアプリ ====
Say-Step 3 $script:Total (M 'iPhone のアプリ' 'iPhone app')
$ipa = Get-KitIpa 'RpcServer'
$guided = $false
while ($true) {
    $app = Get-IphoneAppState
    if ($app.State -eq 'ok') {
        Say-OK ((M 'iPhone のアプリが応答しました：{0}' 'The iPhone app answered: {0}') -f $app.Name)
        Add-Row 'ok' 'iPhone app' $app.Name
        break
    }
    if ($app.State -eq 'busy') {
        Say-Warn (M 'iPhone のアプリは起動していますが、他のアプリが GPU を使っています。そのアプリを終了してください。' 'The iPhone app is open, but another program is using the GPU. Close that program.')
        break
    }
    if ($app.State -eq 'mismatch') { Say-NG (M 'iPhone のアプリがこのキットと一致しません。この .ipa で入れ直してください。' 'The iPhone app does not match this kit. Install it again from the .ipa below.') }
    elseif ($app.State -eq 'noanswer') { Say-Warn (M 'iPhone のアプリが応答しません。まだインストールしていない場合は、下の手順で入れてください。' 'The iPhone app does not answer. If it is not installed yet, install it with the steps below.') }
    else { Say-Info (M 'iPhone が USB で見えません。接続してロックを解除してください。アプリをまだ入れていない場合は、下の手順で入れてください。' 'The iPhone is not seen over USB. Plug it in and unlock it. If the app is not installed yet, use the steps below.') }
    if (-not $guided) {
        $guided = $true
        $alt = Get-AltServerInfo
        if ($ipa) { Say-Info ((M '入れるファイル：{0}' 'File to install: {0}') -f (Split-Path -Leaf $ipa)); Say-Info ((M '場所：{0}' 'Location: {0}') -f $ipa) }
        else { Say-Warn (M 'このフォルダに RpcServer-*.ipa がありません。Update-PocketGPU.cmd で取得できます。' 'There is no RpcServer-*.ipa in this folder. Update-PocketGPU.cmd can get it.'); Add-Row 'todo' 'iPhone app file' 'no .ipa in the kit' }
        $oldIpa = Get-KitIpa 'PocketCam'
        if ($oldIpa) { Say-Info ((M 'iOS 15・16 の iPhone では、{0} を使います（カメラとマイクだけ）。' 'On iOS 15 or 16, use {0} (camera and microphone only).') -f (Split-Path -Leaf $oldIpa)) }
        if ($alt.Path) {
            Say-Info (M '手順（AltServer）' 'Steps (AltServer)')
            Say-Info (M '1. iPhone を USB で接続して、ロックを解除し、「信頼」をタップします。' '1. Plug in the iPhone, unlock it, and tap Trust.')
            Say-Info (M '2. タスクトレイの AltServer のアイコンをクリック → 「Sideload .ipa...」を選び、上の .ipa を選びます。' '2. Click the AltServer icon in the tray, choose "Sideload .ipa...", and pick the .ipa above.')
            Say-Info (M '3. 自分の Apple ID とパスワードを、AltServer の画面に入力します（このセットアップは入力しません）。' '3. Type your own Apple ID and password in the AltServer window (this setup never does).')
            Say-Info (M '4. iPhone の「設定 → 一般 → VPN とデバイス管理」で、自分の Apple ID を「信頼」します。iOS 16 以降は、開発者モードもオンにします。' '4. On the iPhone, open Settings > General > VPN & Device Management and trust your Apple ID. On iOS 16 or later, also turn on Developer Mode.')
            Say-Info (M '5. PocketGPU のアプリを開き、画面をオンにして前面に表示します。' '5. Open the PocketGPU app and keep it in front with the screen on.')
            Say-Info (M '無料の Apple ID では、7 日で起動できなくなります。その場合は、もう一度入れます。' 'With a free Apple ID the app stops opening after 7 days. Install it again then.')
            if (-not $alt.Running -and (Confirm-Step (M 'AltServer を起動しますか？' 'Start AltServer?'))) { [void](Invoke-Launch 'exe' $alt.Path) }
            if ($ipa -and (Confirm-Step (M '.ipa のフォルダを開きますか？' 'Show the .ipa in Explorer?'))) { [void](Invoke-Launch 'folder' $ipa) }
        } else {
            Say-Info (M '手順（AltServer が入っていません。このセットアップは AltServer を自動ではインストールしません）' 'Steps (AltServer is not installed; this setup does not install it for you)')
            Say-Info (M '1. AltServer は、Microsoft Store 版ではない iTunes と iCloud（apple.com 版）が必要です。' '1. AltServer needs iTunes and iCloud from apple.com, not the Microsoft Store versions.')
            Say-Info (M '2. 自分でインストールするコマンド（管理者権限が必要）：' '2. Commands to run yourself (administrator rights are needed):')
            Say-Info '     winget install --id RileyTestut.AltServer'
            Say-Info '     winget install --id Apple.iCloud'
            Say-Info (M '   別の方法：Sideloadly  winget install --id iOSGods.Sideloadly' '   Or Sideloadly:  winget install --id iOSGods.Sideloadly')
            Say-Info (M '3. 入れたら、Setup-PocketGPU.cmd をもう一度起動してください。AltServer を見つけると、.ipa を入れる手順を表示します。' '3. Then open Setup-PocketGPU.cmd again. When it finds AltServer, it shows how to install the .ipa.')
            Say-Info (M 'Apple ID は、AltServer や Sideloadly の画面に、自分で入力してください。' 'Type your Apple ID yourself in the AltServer or Sideloadly window.')
            Offer-Page 'https://altstore.io' (M 'AltStore のページを開きますか？' 'Open the AltStore page?')
        }
    }
    if (-not (Ask-Retry)) { break }
}
if ($app.State -ne 'ok' -and $app.State -ne 'busy') {
    $appNote = @{ noanswer = 'no answer (not open, or not installed)'; mismatch = 'version does not match the kit'; nodevice = 'iPhone not seen over USB'; nomux = 'Apple service not running' }[$app.State]
    Add-Row 'todo' 'iPhone app' $appNote
}

# ==== 4. 最後のチェック ====
Say-Step 4 $script:Total (M '最後のチェック' 'Final check')
Out-Line (M 'Start-PocketGPU.cmd と同じチェックを行います。iPhone の GPU が見えるところまで進みます。' 'This runs the same check as Start-PocketGPU.cmd, up to the point where the iPhone GPU is seen.')
$childEnv = @{ PGPU_NOPAUSE = '1' }
if ($env:PGPU_TEST_FINAL_ANSWERS) { $childEnv.PGPU_ANSWERS = $env:PGPU_TEST_FINAL_ANSWERS }
elseif ($script:NoPrompt) { $childEnv.PGPU_ANSWERS = (@($script:Answers.ToArray()) -join ',') }
# 試験：偽の winget が入れた印を、最後のチェックにも伝える
if ($env:PGPU_TEST_STATE_DIR) {
    if (Test-StateMarker '9NP83LWLPZ9K') { $childEnv.PGPU_TEST_APPLE_SW = 'devices' }
    elseif (Test-StateMarker 'Apple.iTunes') { $childEnv.PGPU_TEST_APPLE_SW = 'service' }
    if (Test-StateMarker 'KhronosGroup.VulkanRT') { $childEnv.PGPU_TEST_VK_LOADER = '1.4.363.0' }
}
$saved = @{}
foreach ($k in $childEnv.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $childEnv[$k]) }
$startLog = Join-Path $script:KitDir 'pocketgpu-start-log.txt'
if (Test-Path -LiteralPath $startLog) { Remove-Item -LiteralPath $startLog -ErrorAction SilentlyContinue }
$finalCode = 99
$startPs1 = Join-Path $PSScriptRoot 'start.ps1'
if (Test-Path -LiteralPath $startPs1) {
    try {
        & (Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass -File $startPs1
        $finalCode = $LASTEXITCODE
    } finally { foreach ($k in $childEnv.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) } }
} else {
    foreach ($k in $childEnv.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    Say-NG (M 'tools\start.ps1 がありません。キットを展開し直してください。' 'tools\start.ps1 is missing. Extract the kit again.')
}
# 結果は、終わりのコードと、start.ps1 のログ（[n/7] の見出しと OK の行）から読む
$stopStep = 0
$gpuNames = @()
if (Test-Path -LiteralPath $startLog) {
    $cur = 0
    foreach ($l in [IO.File]::ReadAllLines($startLog)) {
        $script:Log.Add('  | ' + $l)
        if ($l -match '^\[(\d+)/(\d+)\]') { $cur = [int]$Matches[1]; $stopStep = $cur }
        elseif ($cur -eq 7 -and $l -match '^  OK  (.+)$') { $gpuNames += ($Matches[1] -replace '^[^：:]*[：:]\s*', '') }
    }
}
Out-Line ''
if ($finalCode -eq 0 -and $gpuNames.Count -gt 0) {
    Say-OK ((M 'iPhone の GPU が見えました：{0}' 'The iPhone GPU is seen: {0}') -f ($gpuNames -join ', '))
    Add-Row 'ok' 'iPhone GPU' ($gpuNames -join ', ')
    # ここまで通ったので、Apple の部品とサービスは使えている。「足りない」のまま残った行は「問題なし」にする（「入れた」の行はそのまま）
    foreach ($t in @(@('apple', 'Apple software'), @('applesvc', 'Apple service'))) {
        $row = @($script:Rows | Where-Object { (Get-RowKey $_.Title) -eq $t[0] })
        if ($row.Count -gt 0 -and $row[0].State -eq 'todo') { Add-Row 'ok' $t[1] }
    }
} elseif ($finalCode -eq 0) {
    Say-Warn (M 'チェックは最後まで進みましたが、注意があります。上の表示を見てください。' 'The check reached the end, but there are notes. See the lines above.')
    Add-Row 'note' 'Final check' 'finished with notes'
} else {
    Say-NG ((M '最後のチェックが手順 {0} で止まりました。' 'The final check stopped at step {0}.') -f $(if ($stopStep -gt 0) { $stopStep } else { '?' }))
    Say-Info (M '直せる問題は、Fix-PocketGPU.cmd で自動的に直せることがあります。' 'Fix-PocketGPU.cmd can fix some problems automatically.')
    Add-Row 'todo' 'iPhone GPU' ('final check stopped at step ' + $(if ($stopStep -gt 0) { $stopStep } else { '?' }) + ' of 7')
}
Out-Line (M 'Android のスマートフォンは Start-Android-GPU.cmd を使います。' 'For an Android phone, use Start-Android-GPU.cmd.')

# ==== まとめ ====
$names = @{ ok = (M '問題なし' 'OK'); done = (M '入れた' 'installed'); failed = (M '入れられなかった' 'failed'); todo = (M '足りない' 'missing'); skipped = (M 'しなかった' 'skipped'); note = (M '気をつけたい' 'note') }
$cnt = @{ ok = 0; done = 0; failed = 0; todo = 0; skipped = 0; note = 0 }
foreach ($r in $script:Rows) { $cnt[$r.State]++ }
$sum = New-Object System.Collections.Generic.List[string]
$sum.Add('PocketGPU Setup ' + (Get-Date -Format 'yyyy-MM-dd') + ' (setup v' + $script:SetupVersion + ')')
$sum.Add('PC: ' + (Get-PcInfo))
$sum.Add(('Kit: app {0} / proto {1}' -f $(if ($script:AppVersion) { $script:AppVersion } else { '?' }), $script:ProtoVersion))
$sum.Add(((M '結果：入れた {0} / 入れられなかった {1} / 足りない {2} / しなかった {3} / 気をつけたい {4} / 問題なし {5}' 'Result: installed {0} / failed {1} / missing {2} / skipped {3} / note {4} / OK {5}') -f $cnt.done, $cnt.failed, $cnt.todo, $cnt.skipped, $cnt.note, $cnt.ok))
foreach ($r in $script:Rows) {
    if ($r.State -eq 'ok') { continue }
    $sum.Add(('[{0}] {1}{2}' -f $names[$r.State], $r.Title, $(if ($r.Note) { ' - ' + $r.Note } else { '' })))
}
$oks = @($script:Rows | Where-Object { $_.State -eq 'ok' } | ForEach-Object { $_.Title })
if ($oks.Count -gt 0) { $sum.Add((M '問題なし：' 'OK: ') + ($oks -join ', ')) }
$sum.Add($(if ($finalCode -eq 0 -and $gpuNames.Count -gt 0) { 'iPhone GPU: seen' } else { 'iPhone GPU: not seen yet' }))
$summaryText = ($sum -join "`r`n")
if ($env:USERNAME) { $summaryText = $summaryText -replace [regex]::Escape($env:USERNAME), '(user)' }
if ($env:COMPUTERNAME) { $summaryText = $summaryText -replace [regex]::Escape($env:COMPUTERNAME), '(pc)' }
$summaryText = $summaryText.Replace($script:KitDir, '<kit>')
Out-Line ''
Out-Line (M '==== まとめ（Discord に貼れます）====' '==== Summary (you can paste this on Discord) ====') 'Cyan'
foreach ($l in ($summaryText -split "`r?`n")) { Out-Line $l }
Out-Line (M '=======================================' '==============================================') 'Cyan'
Out-Line (M 'この文には、ユーザー名・PC 名・フォルダの場所は入っていません。' 'This text has no user name, PC name, or folder path.')
try {
    [IO.File]::WriteAllText((Join-Path $script:KitDir 'pocketgpu-setup-summary.txt'), $summaryText + "`r`n", (New-Object Text.UTF8Encoding($true)))
    Out-Line ((M 'ファイルにも書きました：' 'Also saved to: ') + 'pocketgpu-setup-summary.txt')
} catch { }
Finish $(if ($finalCode -eq 0 -and $cnt.failed -eq 0) { 0 } else { 1 })
