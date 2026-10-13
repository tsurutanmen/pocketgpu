# キットの .ps1 と .cmd の書き方をそろえる。Windows PowerShell 5.1 で動く。
#   .ps1：UTF-8（BOM 付き）・改行は CRLF。BOM が無いと 5.1 は日本語を Shift_JIS として読んで壊れる
#   .cmd：ASCII だけ・改行は CRLF。日本語の Windows の cmd は Shift_JIS で読むので、UTF-8 の日本語が化ける
#   -Check を付けると、直さずに、そろっていない物を一覧にして、あれば終わりのコード 1 を返す
#   -Root を付けると、そのフォルダの下を対象にする（無ければ、このファイルの 1 つ上 = リポジトリ）
param(
    [string]$Root = '',
    [switch]$Check
)
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$bad = @()
$files = Get-ChildItem -LiteralPath $Root -Recurse -File |
    Where-Object { $_.Extension -in '.ps1', '.cmd' -and $_.FullName -notmatch '\\\.git\\' }
foreach ($f in $files) {
    $bytes = [IO.File]::ReadAllBytes($f.FullName)
    $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    $text = [Text.Encoding]::UTF8.GetString($bytes, $(if ($hasBom) { 3 } else { 0 }), $bytes.Length - $(if ($hasBom) { 3 } else { 0 }))
    $isPs1 = $f.Extension -ieq '.ps1'
    if (-not $isPs1 -and $text -match '[^\x00-\x7F]') { $bad += ($f.FullName + ' : .cmd に ASCII ではない文字がある'); continue }
    $norm = ($text -replace "`r`n", "`n" -replace "`r", "`n") -replace "`n", "`r`n"
    $want = New-Object Text.UTF8Encoding($isPs1)
    $out = $want.GetPreamble() + $want.GetBytes($norm)
    $same = ($out.Length -eq $bytes.Length)
    if ($same) { for ($i = 0; $i -lt $out.Length; $i++) { if ($out[$i] -ne $bytes[$i]) { $same = $false; break } } }
    if ($same) { continue }
    if ($Check) { $bad += ($f.FullName + ' : BOM・改行がそろっていない') }
    else { [IO.File]::WriteAllBytes($f.FullName, $out); Write-Host ('直した：' + $f.FullName) }
}
if ($bad.Count -gt 0) { $bad | ForEach-Object { Write-Host $_ }; exit 1 }
exit 0
