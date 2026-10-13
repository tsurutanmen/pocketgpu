# PocketGPU 動作報告の作成。試す人が自分の PC で実行します
#   PowerShell で実行:  powershell -ExecutionPolicy Bypass -File report.ps1
# 1 つのテキストファイル pocketgpu-report.txt を作成します。
# 含める内容: Windows のバージョン / CPU / GPU の一覧 / Apple Mobile Device Service / Apple Devices / Apple のサービスのポート / iTunes / iPhone が USB で認識されているか
# 含めない内容: ユーザー名・PC 名・IP アドレス・パスのユーザー名の部分。最後にまとめて伏せます
#   キットに入っている人ごとの識別用の印は、付けたときだけ載せます：  report.cmd -WithKitMark  （GitHub の Issue など、公開の場所には貼らないでください）
# 管理者権限は必要ありません。

param([switch]$WithKitMark)
$ErrorActionPreference = 'SilentlyContinue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $here 'pocketgpu-report.txt'
$L = New-Object System.Collections.Generic.List[string]
function Add($s) { $L.Add($s) }

Add('PocketGPU report / 動作報告')
Add('Created / 作成: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm'))
Add('')

# --- Windows の版 ---
Add('[Windows]')
$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
if ($cv) {
    $name = $cv.ProductName
    if ([int]$cv.CurrentBuild -ge 22000) { $name = $name -replace 'Windows 10', 'Windows 11' }
    $build = '' + $cv.CurrentBuild
    if ($cv.UBR) { $build = $build + '.' + $cv.UBR }
    Add('  Name / 名前: ' + $name)
    Add('  Version / 表示バージョン: ' + $cv.DisplayVersion)
    Add('  Build / ビルド: ' + $build)
    $arch = $env:PROCESSOR_ARCHITECTURE
    Add('  Architecture / アーキテクチャ: ' + $arch)
} else {
    Add('  Not available / 取得できませんでした')
}
Add('')

# --- CPU ---
Add('[CPU]')
$cpu = Get-CimInstance Win32_Processor
if ($cpu) {
    foreach ($c in $cpu) {
        Add('  ' + $c.Name.Trim() + '  (cores ' + $c.NumberOfCores + ', threads ' + $c.NumberOfLogicalProcessors + ')')
    }
} else {
    Add('  Not available / 取得できませんでした')
}
Add('')

# --- GPU の一覧 ---
Add('[GPU]')
$gpu = Get-CimInstance Win32_VideoController
if ($gpu) {
    foreach ($g in $gpu) {
        Add('  ' + $g.Name + '  driver ' + $g.DriverVersion)
    }
} else {
    Add('  Not available / 取得できませんでした')
}
Add('')

# --- 配った印（iphone_icd.dll の中の PGKIT-MARK: に続く 32 文字）。-WithKitMark のときだけ ---
if ($WithKitMark) {
    Add('[Kit mark / 識別用の印]')
    $dll = Join-Path $here 'iphone_icd.dll'
    if (Test-Path $dll) {
        $bytes = [System.IO.File]::ReadAllBytes($dll)
        $txt = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)
        $tag = 'PGKIT-MARK:'
        $i = $txt.IndexOf($tag)
        if ($i -ge 0 -and ($i + $tag.Length + 32) -le $txt.Length) {
            $mark = $txt.Substring($i + $tag.Length, 32)
            Add('  ' + $mark)
        } else {
            Add('  Mark not found in iphone_icd.dll / 識別用の印が見つかりませんでした')
        }
    } else {
        Add('  iphone_icd.dll is not in this folder. Run this from the kit folder. / キットのフォルダで実行してください')
    }
    Add('')
}

# --- Apple Mobile Device Service ---
Add('[Apple Mobile Device Service]')
$svc = Get-Service -Name 'Apple Mobile Device Service' -ErrorAction SilentlyContinue
if (-not $svc) { $svc = Get-Service -DisplayName 'Apple Mobile Device Service' -ErrorAction SilentlyContinue }
if ($svc) {
    Add('  Status / 状態: ' + $svc.Status + ', startup / スタートアップ: ' + $svc.StartType)
} else {
    Add('  Not found. Normal with Apple Devices from the Microsoft Store. / 見つかりません。Microsoft Store の Apple Devices を使っている場合は、正常です')
}
Add('')

# --- Microsoft Store の Apple Devices と、Apple のサービスのポート 127.0.0.1:27015 ---
Add('[Apple Devices]')
$ad = Get-AppxPackage -Name 'AppleInc.AppleDevices' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($ad) { Add('  version ' + $ad.Version) } else { Add('  Not found / 見つかりません') }
Add('')
Add('[Apple service port 27015 / Apple のサービスのポート]')
$tc = New-Object System.Net.Sockets.TcpClient
try { $tc.Connect('127.0.0.1', 27015); Add('  Listening / 待ち受けています') } catch { Add('  Not listening / 待ち受けていません') } finally { $tc.Close() }
Add('')

# --- iTunes が入っているか ---
Add('[iTunes]')
$itunesFound = $false
$keys = @(
    'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*'
)
foreach ($k in $keys) {
    $apps = Get-ItemProperty $k -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -like 'iTunes*' }
    foreach ($app in $apps) {
        Add('  ' + $app.DisplayName + '  version ' + $app.DisplayVersion)
        $itunesFound = $true
    }
}
if (-not $itunesFound) {
    $exe = Join-Path $env:ProgramFiles 'iTunes\iTunes.exe'
    if (Test-Path $exe) {
        $ver = (Get-Item $exe).VersionInfo.ProductVersion
        Add('  iTunes.exe found, version ' + $ver)
        $itunesFound = $true
    }
}
if (-not $itunesFound) { Add('  Not found / 見つかりません') }
Add('')

# --- iPhone が USB で見えるか ---
Add('[iPhone over USB / iPhone の USB 接続]')
$pnp = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'USB\*' } | Where-Object {
    $_.FriendlyName -like '*iPhone*' -or $_.FriendlyName -like '*iPad*' -or $_.FriendlyName -like '*Apple Mobile Device*'
}
if ($pnp) {
    foreach ($d in $pnp) {
        Add('  ' + $d.FriendlyName + '  (' + $d.Status + ')')
    }
} else {
    Add('  No Apple device found. Plug in the iPhone, unlock it, and run this again. / iPhone を USB ケーブルで接続してロックを解除してから、もう一度実行してください')
}
Add('')

# --- 個人情報を伏せる（念のため最後にまとめて）---
$body = ($L -join "`r`n")
$u = $env:USERNAME
$pc = $env:COMPUTERNAME
if ($u)  { $body = $body -replace [regex]::Escape($u),  '(hidden)' }
if ($pc) { $body = $body -replace [regex]::Escape($pc), '(hidden)' }
# IP アドレスはどこからも集めていないので、伏せる処理は入れない（バージョン番号を誤って消すため）

# BOM 付き UTF-8 で書き出す
$enc = New-Object System.Text.UTF8Encoding($true)
[System.IO.File]::WriteAllText($out, $body + "`r`n", $enc)

Write-Host ''
Write-Host 'Created pocketgpu-report.txt in this folder. / このフォルダに pocketgpu-report.txt を作成しました'
Write-Host 'Please send its contents on Discord. It does not include your user name, PC name, or IP address. / 内容を Discord で送ってください。ユーザー名・PC 名・IP アドレスは含まれていません'
Write-Host ''
Write-Host '----- report / 内容 -----'
Write-Host $body
