# PocketGPU を整える（管理者の部分）：tools\setup.ps1 が、管理者権限が要る物をまとめて 1 回の UAC で渡す小さな部分。
# 自分で開く物ではない。setup.ps1 が -Verb RunAs で起動する。Windows PowerShell 5.1 だけで動く。
# 日本語を含むので UTF-8（BOM 付き）で保存する。BOM が無いと 5.1 は Shift_JIS として読んで壊れる。
# 画面の文は仮。M '日本語' 'English' の組で並べてあるので、言い回しはここで直す。
#
# 受け取るのは、決まった名前の一覧（-Actions）だけ。コマンドは、すべてこのファイルの中に固定してある。名前が違う物は何もしない。
#   apple-itunes     winget install Apple.iTunes（Apple Mobile Device Support と USB ドライバを含む）
#   vulkan-runtime   winget install KhronosGroup.VulkanRT（LunarG の Vulkan Runtime）
#   vcredist-x64     winget install Microsoft.VCRedist.2015+.x64
#   vcredist-x86     winget install Microsoft.VCRedist.2015+.x86
#   apple-service    Apple Mobile Device Service を開始する
# 終わりのコード：0 すべて成功・1 失敗した物がある・3 winget がない
# レジストリ（HKCU・HKLM）には自分では書かない。仮想カメラの登録は、管理者ではなく、ふつうの権限の setup.ps1 がやる（HKCU が別のユーザーになるため）。
#
# 試す人は使わない、試験用の差し替え（pgpu_common.ps1 の物に加えて）：
#   PGPU_TEST_SERVICE_START=パス   サービスを開始する代わりに、この .cmd を動かす
param(
    [string]$Actions = '',
    [string]$Winget = '',
    [switch]$Pause
)
. (Join-Path $PSScriptRoot 'pgpu_common.ps1')

$Table = @{
    'apple-itunes'   = @{ Id = 'Apple.iTunes'; Source = 'winget'; Title = 'iTunes (Apple Mobile Device Support)' }
    'vulkan-runtime' = @{ Id = 'KhronosGroup.VulkanRT'; Source = 'winget'; Title = 'Vulkan Runtime (LunarG)' }
    'vcredist-x64'   = @{ Id = 'Microsoft.VCRedist.2015+.x64'; Source = 'winget'; Title = 'Visual C++ Redistributable x64' }
    'vcredist-x86'   = @{ Id = 'Microsoft.VCRedist.2015+.x86'; Source = 'winget'; Title = 'Visual C++ Redistributable x86' }
}

function Close-Here([int]$code) {
    if ($Pause -and -not $script:NoPrompt) { [void](Read-Host (M 'Enter キーを押すと、このウィンドウを閉じます' 'Press Enter to close this window')) }
    exit $code
}

Out-Line (M 'PocketGPU セットアップ（管理者）' 'PocketGPU Setup (administrator)')
Out-Line (M 'このウィンドウで、管理者権限が必要な物をまとめて入れます。' 'This window installs, in one go, what needs administrator rights.')

$list = @($Actions -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($list.Count -eq 0) { Say-NG (M '入れる物が渡されませんでした。' 'Nothing was passed to install.'); Close-Here 1 }

$wg = ''
if ($Winget -and (Test-Path -LiteralPath $Winget -PathType Leaf)) { $wg = $Winget }
else { $c = Get-Command winget.exe -ErrorAction SilentlyContinue | Select-Object -First 1; if ($c) { $wg = $c.Source } }
$needWinget = @($list | Where-Object { $Table.ContainsKey($_) }).Count -gt 0
if ($needWinget -and -not $wg) {
    Say-NG (M 'この管理者ウィンドウから winget が見つかりません。' 'winget was not found from this administrator window.')
    Say-Info (M '元のウィンドウに戻って、手順を見てください。' 'Go back to the first window and follow its steps.')
    Close-Here 3
}

$failed = 0
$n = 0
foreach ($a in $list) {
    $n++
    Out-Line ''
    if ($a -eq 'apple-service') {
        Out-Line ('[{0}/{1}] Apple Mobile Device Service' -f $n, $list.Count)
        try {
            if ($env:PGPU_TEST_SERVICE_START) {
                & $env:ComSpec /c ('"' + $env:PGPU_TEST_SERVICE_START + '"') | Out-Null
            } else {
                $s = Get-Service -Name 'Apple Mobile Device Service' -ErrorAction Stop
                if ($s.Status -eq 'Running') { Restart-Service -Name 'Apple Mobile Device Service' -ErrorAction Stop } else { Start-Service -Name 'Apple Mobile Device Service' -ErrorAction Stop }
            }
            Say-OK (M 'Apple のサービスを開始しました。' 'The Apple service was started.')
        } catch {
            $failed++
            Say-NG ((M '開始できませんでした：' 'Could not start it: ') + $_.Exception.Message)
        }
        continue
    }
    if (-not $Table.ContainsKey($a)) { $failed++; Out-Line ('[{0}/{1}] {2}' -f $n, $list.Count, $a); Say-NG (M '知らない名前なので、何もしません。' 'Unknown name. Nothing was done.'); continue }
    $t = $Table[$a]
    Out-Line ('[{0}/{1}] {2}' -f $n, $list.Count, $t.Title)
    $wargs = @('install', '--id', $t.Id, '--source', $t.Source, '--accept-package-agreements', '--accept-source-agreements', '--silent')
    Say-Info ('winget ' + ($wargs -join ' '))
    $code = -1
    try { & $wg @wargs; $code = $LASTEXITCODE } catch { Say-Info $_.Exception.Message }
    # 0 成功・0x8A150061 すでに入っている・0x8A15002B 更新なし
    if ($code -eq 0 -or $code -eq -1978335135 -or $code -eq -1978335189) { Say-OK ((M '入りました。' 'Installed.') + ' (' + $t.Id + ')') }
    else { $failed++; Say-NG ((M '入りませんでした。コード {0}' 'Not installed. Code {0}') -f $code) }
}

Out-Line ''
if ($failed -eq 0) { Say-OK (M 'すべて終わりました。元のウィンドウに戻ってください。' 'All done. Go back to the first window.') }
else { Say-NG ((M '{0} 個が失敗しました。元のウィンドウに戻ってください。' '{0} failed. Go back to the first window.') -f $failed) }
Close-Here $(if ($failed -eq 0) { 0 } else { 1 })
