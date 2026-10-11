# iPhone の GPU で起動：1 つのゲームやアプリを、iPhone の GPU が見える形で起動する。Run-on-iPhone-GPU.cmd から呼ぶ。
#   Run-on-iPhone-GPU.cmd "C:\Games\game.exe" [ゲームに渡す引数]   .exe か、そのショートカット（.lnk）を上に落としてもよい
#   Run-on-iPhone-GPU.cmd                                       何も無ければ .exe を選ぶ窓を出す
# 起動する前に見ること：管理者で動く設定のアプリか・iPhone のアプリが答えるか・DirectX 11 なら DXVK が横にあるか
# 環境変数はこの起動にだけ付ける。レジストリには書かない。DXVK を置くのは、聞いて「はい」のときだけ
# 試験：PGPU_DRYRUN=1 で、最後に起動せず、何を起動するかだけ出す（DXVK を置くのは本当に行う。試験は試験用のフォルダで）
. (Join-Path $PSScriptRoot 'pgpu_common.ps1')

$DryRun = $env:PGPU_DRYRUN -eq '1'
$Warned = $false

function Done([int]$code) {
    if (($script:Warned -or $code -ne 0) -and -not $script:NoPrompt -and $env:PGPU_NOPAUSE -ne '1') {
        [void](Read-Host (M 'Enter キーを押すと閉じます' 'Press Enter to close'))
    } elseif ($code -eq 0 -and -not $script:NoPrompt -and $env:PGPU_NOPAUSE -ne '1') {
        Start-Sleep -Seconds 2
    }
    exit $code
}
function Warn([string]$t) { Say-Warn $t; $script:Warned = $true }

function Get-FileHashText([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash }

# ---- 何を起動するか ----
$target = $null
$rest = @()
$lnkArgs = ''
if ($args.Count -gt 0) { $target = [string]$args[0]; if ($args.Count -gt 1) { $rest = @($args[1..($args.Count - 1)]) } }
if (-not $target) {
    if ($script:NoPrompt) { Say-NG (M '起動する .exe が指定されていません。' 'No .exe was given.'); Done 1 }
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object Windows.Forms.OpenFileDialog
    $dlg.Title = M 'iPhone の GPU で起動するアプリを選択してください' 'Choose the program to run on the iPhone GPU'
    $dlg.Filter = (M 'アプリ' 'Programs') + '|*.exe;*.lnk;*.bat;*.cmd'
    if ($dlg.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { exit 1 }
    $target = $dlg.FileName
}
$workDir = $null
if ($target -like '*.url') {
    Say-NG (M 'インターネットショートカットの .url は使用できません。' 'Internet shortcuts .url cannot be used.')
    Say-Info (M 'Steam のゲームは、ゲームのプロパティの「起動オプション」に次の 1 行を入力してください。' 'For a Steam game, put this line in the game''s Properties > Launch Options:')
    Say-Info ('"' + (Join-Path $script:KitDir 'pocketgpu.cmd') + '" %command%')
    Done 1
}
if ($target -like '*.lnk') {
    $lnk = (New-Object -ComObject WScript.Shell).CreateShortcut($target)
    if (-not $lnk.TargetPath -or -not (Test-Path -LiteralPath $lnk.TargetPath)) {
        Say-NG (M 'このショートカットのリンク先が分かりません。.exe を直接選択してください。' 'Cannot find where this shortcut points. Choose the .exe itself.')
        Done 1
    }
    if ($lnk.Arguments) { $lnkArgs = $lnk.Arguments }
    if ($lnk.WorkingDirectory) { $workDir = $lnk.WorkingDirectory }
    $target = $lnk.TargetPath
}
if (-not (Test-Path -LiteralPath $target -PathType Leaf)) { Say-NG ((M '見つかりません：' 'Not found: ') + $target); Done 1 }
$target = (Resolve-Path -LiteralPath $target).Path
if (-not $workDir) { $workDir = Split-Path -Parent $target }
$exeDir = Split-Path -Parent $target
Out-Line ((M 'iPhone の GPU で起動します：' 'Running on the iPhone GPU: ') + $target)

# ---- 1. 管理者 ----
if (Test-Elevated) {
    Warn (M 'このウィンドウは管理者権限で実行されています。このままでは iPhone の GPU を認識できません。「管理者として実行」を使わずに、ダブルクリックし直してください。' 'This window runs as administrator, so the iPhone GPU cannot be seen. Start it again with a normal double-click.')
}
$pe = $null
if ($target -like '*.exe') { $pe = Get-PeInfo $target }
$adminWhy = ''
if ($pe -and $pe.Level -eq 'requireAdministrator') { $adminWhy = M 'このアプリは、常に管理者権限で起動する設定になっています。' 'This program always starts as administrator.' }
elseif (Test-CompatRunAsAdmin $target) { $adminWhy = M 'このアプリには「管理者として実行」の設定があります。' 'This program has the "Run as administrator" setting.' }
if ($adminWhy) {
    Warn $adminWhy
    Say-Info (M 'Windows は管理者権限で実行されるアプリに iPhone の GPU の設定を渡さないため、普通の GPU で動作します。' 'Windows does not pass the iPhone GPU settings to programs that run as administrator, so it will use the normal GPU.')
    if (-not (Ask-YesNo (M 'それでも起動しますか？' 'Start it anyway?'))) { Done 1 }
} elseif ($pe -and $pe.Level -eq 'highestAvailable') {
    Warn (M 'このアプリは、管理者アカウントでは管理者権限で起動することがあります。その場合は iPhone の GPU を認識できません。' 'This program may start as administrator on an administrator account. Then it cannot see the iPhone GPU.')
}

# ---- 2. iPhone の様子（数秒で見る） ----
$problem = ''
if (-not (Test-MuxListening)) { $problem = M 'Apple のサービスが実行されていません。' 'The Apple service is not running.' }
else {
    $usb = @(Get-MuxDevices | Where-Object { $_.Type -eq 'USB' })
    if ($usb.Count -eq 0) { $problem = M 'iPhone が USB で認識されていません。' 'The iPhone is not seen over USB.' }
    else {
        $h = Test-Receiver $usb[0].Id 2500
        if ($h.Ok) { Say-OK ((M 'iPhone のアプリが応答しました：' 'The iPhone app answered: ') + $h.Name) }
        elseif ($h.Why -eq 'timeout') { $problem = M '他のアプリが iPhone の GPU を使用しています。そのアプリを終了してから起動してください。' 'Another program is using the iPhone GPU. Close it first.' }
        elseif ($h.Why -eq 'result' -and $h.Result -eq -9) { $problem = M 'iPhone のアプリとこのキットのバージョンが一致しません。' 'The iPhone app does not match this kit.' }
        else { $problem = M 'iPhone のアプリが応答しません。前面に表示して、画面をオンにしておいてください。' 'The iPhone app does not answer. Keep it in front with the screen on.' }
    }
}
if ($problem) {
    Warn $problem
    Say-Info (M '詳しくは Start-PocketGPU.cmd でチェックできます。' 'Start-PocketGPU.cmd checks this step by step.')
    if (-not (Ask-YesNo (M 'このまま起動しますか？' 'Start it anyway?'))) { Done 1 }
}

# ---- 3. DirectX と Steam ----
if ($pe) {
    $dxTarget = $target
    $dxPe = $pe
    # Unreal のゲームは、外の .exe が中の *-Shipping.exe を起動する。DXVK は中の方の横に置く
    $ship = @(Get-ChildItem -Path (Join-Path $exeDir '*\Binaries\Win64\*-Shipping.exe') -ErrorAction SilentlyContinue)
    if ($ship.Count -eq 1 -and $pe.Imports -notcontains 'd3d11.dll') {
        $dxTarget = $ship[0].FullName
        $dxPe = Get-PeInfo $dxTarget
        Say-Info ((M 'Unreal のゲームのようです。描画を行うのはこちらです：' 'This looks like an Unreal game. The drawing is done by: ') + $dxTarget)
    }
    $imp = @($dxPe.Imports)
    if ($imp -contains 'd3d12.dll') {
        Warn (M 'DirectX 12 のゲームのようです。このキットは、まだ DirectX 12 を iPhone の GPU で描画できません。' 'This looks like a DirectX 12 game. This kit cannot draw DirectX 12 on the iPhone GPU yet.')
    } elseif ($imp -contains 'd3d9.dll') {
        Warn (M 'DirectX 9 のゲームのようです。このキットは、まだ DirectX 9 を iPhone の GPU で描画できません。' 'This looks like a DirectX 9 game. This kit cannot draw DirectX 9 on the iPhone GPU yet.')
    } elseif (($imp -contains 'd3d11.dll' -or $imp -contains 'dxgi.dll') -and $imp -notcontains 'vulkan-1.dll') {
        $arch = $(if ($dxPe.Machine -eq 0x8664) { 'x64' } elseif ($dxPe.Machine -eq 0x14c) { 'x32' } else { '' })
        $dir = Split-Path -Parent $dxTarget
        $here = Join-Path $dir 'd3d11.dll'
        $kitDll = $(if ($arch) { Join-Path $script:KitDir ('dxvk\' + $arch + '\d3d11.dll') } else { '' })
        if (-not $arch) {
            Warn (M 'このゲームのアーキテクチャは DXVK に対応していません。' 'This game''s CPU type does not match DXVK.')
        } elseif (Test-Path -LiteralPath $here) {
            if ((Test-Path $kitDll) -and (Get-FileHashText $here) -eq (Get-FileHashText $kitDll)) { Say-OK (M 'DXVK がゲームと同じフォルダに配置されています。' 'DXVK is next to the game.') }
            else { Warn (M 'ゲームと同じフォルダに別の d3d11.dll があります。上書きしないため、DirectX 11 は普通の GPU で描画されます。' 'There is another d3d11.dll next to the game. It is not replaced, so DirectX 11 will use the normal GPU.') }
        } else {
            Say-Info (M 'DirectX 11 のゲームのようです。iPhone の GPU で描画するには、DXVK の 2 つのファイルをゲームと同じフォルダに配置します。' 'This looks like a DirectX 11 game. To draw it on the iPhone GPU, two DXVK files go next to the game.')
            Say-Info ((M '配置先：' 'Where: ') + $dir)
            Say-Info (M '元に戻すときは、そのフォルダの d3d11.dll と dxgi.dll を削除してください。' 'To undo, delete d3d11.dll and dxgi.dll there.')
            if (Ask-YesNo (M 'DXVK を配置しますか？' 'Put DXVK there?')) {
                $dxgiHere = Join-Path $dir 'dxgi.dll'
                if (Test-Path -LiteralPath $dxgiHere) {
                    Warn (M 'ゲームと同じフォルダに dxgi.dll があります。上書きしないため、DXVK は配置しませんでした。' 'There is a dxgi.dll next to the game. Nothing was copied, so nothing is overwritten.')
                } else {
                    try {
                        Copy-Item -LiteralPath $kitDll -Destination $dir
                        Copy-Item -LiteralPath (Join-Path $script:KitDir ('dxvk\' + $arch + '\dxgi.dll')) -Destination $dir
                        Say-OK (M '配置しました。' 'Done.')
                    } catch {
                        Warn ((M '配置できませんでした。管理者権限がないと書き込めない場所の可能性があります：' 'Could not copy. The folder may need administrator rights: ') + $_.Exception.Message)
                    }
                }
            }
        }
    }
    # Steam のゲームは、Steam から起動し直すことがある。そのときはこの設定が渡らない
    if ((Test-Path (Join-Path $exeDir 'steam_api64.dll')) -or (Test-Path (Join-Path $exeDir 'steam_api.dll'))) {
        Say-Info (M 'Steam のゲームのようです。Steam から再起動されると、iPhone の GPU は使用されません。その場合は、Steam の「起動オプション」に次の 1 行を入力してください。' 'This looks like a Steam game. If Steam restarts it, the iPhone GPU is not used. Then put this line in Steam''s Launch Options:')
        Say-Info ('"' + (Join-Path $script:KitDir 'pocketgpu.cmd') + '" %command%')
    }
}

# ---- 4. 起動 ----
$envs = Get-PgpuEnv
foreach ($k in $envs.Keys) { [Environment]::SetEnvironmentVariable($k, $envs[$k], 'Process') }
# 引数を 1 行に。.lnk の引数はそのまま、落とした・渡した引数は空白があれば " で囲む
$argLine = (@($lnkArgs) + @($rest | ForEach-Object { if ($_ -match '\s' -and $_ -notmatch '^".*"$') { '"' + $_ + '"' } else { $_ } }) | Where-Object { $_ }) -join ' '
if ($DryRun) {
    foreach ($k in $envs.Keys) { Out-Line ('DRYRUN set ' + $k + '=' + $envs[$k]) }
    Out-Line ('DRYRUN start "' + $target + '" ' + $argLine + '  [in ' + $workDir + ']')
    Done 0
}
try {
    if ($argLine) { Start-Process -FilePath $target -ArgumentList $argLine -WorkingDirectory $workDir }
    else { Start-Process -FilePath $target -WorkingDirectory $workDir }
} catch {
    Say-NG ((M '起動できませんでした：' 'Could not start it: ') + $_.Exception.Message)
    Done 1
}
Say-OK (M '起動しました。iPhone のアプリは前面に表示したままにしてください。' 'Started. Keep the iPhone app in front.')
Done 0
