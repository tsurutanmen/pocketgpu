# PocketGPU 動作報告の作成（試す人が自分の PC で実行）
#   PowerShell で実行:  powershell -ExecutionPolicy Bypass -File report.ps1
# 1 つのテキストファイル pocketgpu-report.txt を作ります。
# 入れる物: Windows の版 / CPU / GPU の一覧 / 配った印 / Apple Mobile Device Service / iTunes / iPhone が USB で見えるか
# 入れない物: ユーザー名・PC 名・IP アドレス・パスのユーザー名の部分（最後にまとめて伏せます）
# 管理者の権限は要りません。

$ErrorActionPreference = 'SilentlyContinue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $here 'pocketgpu-report.txt'
$L = New-Object System.Collections.Generic.List[string]
function Add($s) { $L.Add($s) }

Add('PocketGPU 動作報告')
Add('作成: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm'))
Add('')

# --- Windows の版 ---
Add('[Windows]')
$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
if ($cv) {
    $name = $cv.ProductName
    if ([int]$cv.CurrentBuild -ge 22000) { $name = $name -replace 'Windows 10', 'Windows 11' }
    $build = '' + $cv.CurrentBuild
    if ($cv.UBR) { $build = $build + '.' + $cv.UBR }
    Add('  名前: ' + $name)
    Add('  表示バージョン: ' + $cv.DisplayVersion)
    Add('  ビルド: ' + $build)
    $arch = $env:PROCESSOR_ARCHITECTURE
    Add('  アーキテクチャ: ' + $arch)
} else {
    Add('  取得できませんでした')
}
Add('')

# --- CPU ---
Add('[CPU]')
$cpu = Get-CimInstance Win32_Processor
if ($cpu) {
    foreach ($c in $cpu) {
        Add('  ' + $c.Name.Trim() + '  （コア ' + $c.NumberOfCores + ' / スレッド ' + $c.NumberOfLogicalProcessors + '）')
    }
} else {
    Add('  取得できませんでした')
}
Add('')

# --- GPU の一覧 ---
Add('[GPU]')
$gpu = Get-CimInstance Win32_VideoController
if ($gpu) {
    foreach ($g in $gpu) {
        Add('  ' + $g.Name + '  ドライバ ' + $g.DriverVersion)
    }
} else {
    Add('  取得できませんでした')
}
Add('')

# --- 配った印（iphone_icd.dll の中の PGKIT-MARK: に続く 32 文字）---
Add('[配った印]')
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
        Add('  印が見つかりませんでした（iphone_icd.dll の中に PGKIT-MARK: がない）')
    }
} else {
    Add('  iphone_icd.dll がこのフォルダにありません。report.ps1 をキットのフォルダに置いて実行してください')
}
Add('')

# --- Apple Mobile Device Service ---
Add('[Apple Mobile Device Service]')
$svc = Get-Service -Name 'Apple Mobile Device Service' -ErrorAction SilentlyContinue
if (-not $svc) { $svc = Get-Service -DisplayName 'Apple Mobile Device Service' -ErrorAction SilentlyContinue }
if ($svc) {
    Add('  状態: ' + $svc.Status + ' / スタートアップ: ' + $svc.StartType)
} else {
    Add('  見つかりません（iTunes か Apple Mobile Device Support が入っていない可能性）')
}
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
        Add('  ' + $app.DisplayName + '  バージョン ' + $app.DisplayVersion)
        $itunesFound = $true
    }
}
if (-not $itunesFound) {
    $exe = Join-Path $env:ProgramFiles 'iTunes\iTunes.exe'
    if (Test-Path $exe) {
        $ver = (Get-Item $exe).VersionInfo.ProductVersion
        Add('  iTunes.exe あり  バージョン ' + $ver)
        $itunesFound = $true
    }
}
if (-not $itunesFound) { Add('  見つかりません') }
Add('')

# --- iPhone が USB で見えるか ---
Add('[iPhone の USB 接続]')
$pnp = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'USB\*' } | Where-Object {
    $_.FriendlyName -like '*iPhone*' -or $_.FriendlyName -like '*iPad*' -or $_.FriendlyName -like '*Apple Mobile Device*'
}
if ($pnp) {
    foreach ($d in $pnp) {
        Add('  ' + $d.FriendlyName + '  （' + $d.Status + '）')
    }
} else {
    Add('  Apple の携帯端末が見つかりません。iPhone を USB で差して、画面のロックを外してから、もう一度実行してください')
}
Add('')

# --- 個人情報を伏せる（念のため最後にまとめて）---
$body = ($L -join "`r`n")
$u = $env:USERNAME
$pc = $env:COMPUTERNAME
if ($u)  { $body = $body -replace [regex]::Escape($u),  '(伏せ)' }
if ($pc) { $body = $body -replace [regex]::Escape($pc), '(伏せ)' }
# IP アドレスはどこからも集めていないので、伏せる処理は入れない（バージョン番号を誤って消すため）

# BOM 付き UTF-8 で書き出す
$enc = New-Object System.Text.UTF8Encoding($true)
[System.IO.File]::WriteAllText($out, $body + "`r`n", $enc)

Write-Host ''
Write-Host '報告ファイルを作りました: このフォルダの pocketgpu-report.txt'
Write-Host 'この中身を Discord かメールで送ってください。中にユーザー名・PC 名・IP は入っていません。'
Write-Host ''
Write-Host '----- 中身 -----'
Write-Host $body
