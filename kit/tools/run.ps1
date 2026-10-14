# iPhone の GPU で起動：1 つのゲームやアプリを、iPhone の GPU が見える形で起動する。Run-on-iPhone-GPU.cmd から呼ぶ。
#   Run-on-iPhone-GPU.cmd "C:\Games\game.exe" [ゲームに渡す引数]   .exe か、そのショートカット（.lnk）を上に落としてもよい
#   Run-on-iPhone-GPU.cmd                                       何も無ければ .exe を選ぶ窓を出す
# 起動する前に見ること：管理者で動く設定のアプリか・iPhone のアプリが答えるか・DirectX 9・10・11 なら DXVK が横にあるか
# 環境変数はこの起動にだけ付ける。レジストリには書かない。DXVK を置くのは、聞いて「はい」のときだけ
# 試験：PGPU_DRYRUN=1 で、最後に起動せず、何を起動するかだけ出す（DXVK を置くのは本当に行う。試験は試験用のフォルダで）
# Android（10/14）：PGPU_ANDROID=1 のときは、Android のスマートフォンの GPU で起動する。Start-Android-GPU.cmd が付けて呼ぶ。
#   iPhone のアプリ（usbmuxd）に聞く代わりに、adb forward の口（IPHONE_VK=127.0.0.1:口。無ければ PGPU_PORT か android\port.txt の gpu= か 50300）の受け手にあいさつする。
#   DirectX の見分けと DXVK の置き方は iPhone と同じ。DXVK にはスマートフォンの GPU を選ばせる（DXVK_FILTER_DEVICE_NAME=PocketGPU。GPU の名前は「PocketGPU phone (…)」）
. (Join-Path $PSScriptRoot 'pgpu_common.ps1')

$DryRun = $env:PGPU_DRYRUN -eq '1'
$Warned = $false
$Android = $env:PGPU_ANDROID -eq '1'
if ($Android) {
    # 口：環境変数 PGPU_PORT > android\port.txt に覚えた口（Windows が 50300 を予約しているとき、android\start_android.cmd が別の口を選んで覚える）> 50300
    $gpuPort = '50300'
    if ($env:PGPU_PORT) { $gpuPort = $env:PGPU_PORT }
    else {
        $pf = Join-Path $script:KitDir 'android\port.txt'
        if (Test-Path -LiteralPath $pf) { foreach ($pl in [IO.File]::ReadAllLines($pf)) { if ($pl -match '^\s*gpu\s*=\s*(\d{1,5})\s*$') { $gpuPort = $Matches[1] } } }
    }
    if (-not $env:IPHONE_VK) { $env:IPHONE_VK = '127.0.0.1:' + $gpuPort }
    # 画面の文の「どの GPU か」と、Steam の起動オプションに書く入口
    $GJa = 'Android のスマートフォン'; $GEn = 'Android phone'; $Launcher = 'pocketgpu_android.cmd'
} else {
    $GJa = 'iPhone'; $GEn = 'iPhone'; $Launcher = 'pocketgpu.cmd'
}

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
# ファイル名を「a と b」「a、b と c」（英語は「a and b」「a, b and c」）の形に並べる
function Join-Names([string[]]$n, [bool]$ja) {
    $n = @($n)
    if ($n.Count -le 1) { return ($n -join '') }
    if ($ja) { return (($n[0..($n.Count - 2)] -join '、') + ' と ' + $n[-1]) }
    return (($n[0..($n.Count - 2)] -join ', ') + ' and ' + $n[-1])
}

# ---- 何を起動するか ----
$target = $null
$rest = @()
$lnkArgs = ''
if ($args.Count -gt 0) { $target = [string]$args[0]; if ($args.Count -gt 1) { $rest = @($args[1..($args.Count - 1)]) } }
if (-not $target) {
    if ($script:NoPrompt) { Say-NG (M '起動する .exe が指定されていません。' 'No .exe was given.'); Done 1 }
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object Windows.Forms.OpenFileDialog
    $dlg.Title = M ($GJa + ' の GPU で起動するアプリを選択してください') ('Choose the program to run on the ' + $GEn + ' GPU')
    $dlg.Filter = (M 'アプリ' 'Programs') + '|*.exe;*.lnk;*.bat;*.cmd'
    if ($dlg.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { exit 1 }
    $target = $dlg.FileName
}
$workDir = $null
if ($target -like '*.url') {
    Say-NG (M 'インターネットショートカットの .url は使用できません。' 'Internet shortcuts .url cannot be used.')
    Say-Info (M 'Steam のゲームは、ゲームのプロパティの「起動オプション」に次の 1 行を入力してください。' 'For a Steam game, put this line in the game''s Properties > Launch Options:')
    Say-Info ('"' + (Join-Path $script:KitDir $Launcher) + '" %command%')
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
Out-Line ((M ($GJa + ' の GPU で起動します：') ('Running on the ' + $GEn + ' GPU: ')) + $target)

# ---- 1. 管理者 ----
if (Test-Elevated) {
    Warn (M ('このウィンドウは管理者権限で実行されています。このままでは ' + $GJa + ' の GPU を認識できません。「管理者として実行」を使わずに、ダブルクリックし直してください。') ('This window runs as administrator, so the ' + $GEn + ' GPU cannot be seen. Start it again with a normal double-click.'))
}
$pe = $null
if ($target -like '*.exe') { $pe = Get-PeInfo $target }
$adminWhy = ''
if ($pe -and $pe.Level -eq 'requireAdministrator') { $adminWhy = M 'このアプリは、常に管理者権限で起動する設定になっています。' 'This program always starts as administrator.' }
elseif (Test-CompatRunAsAdmin $target) { $adminWhy = M 'このアプリには「管理者として実行」の設定があります。' 'This program has the "Run as administrator" setting.' }
if ($adminWhy) {
    Warn $adminWhy
    Say-Info (M ('Windows は管理者権限で実行されるアプリに ' + $GJa + ' の GPU の設定を渡さないため、普通の GPU で動作します。') ('Windows does not pass the ' + $GEn + ' GPU settings to programs that run as administrator, so it will use the normal GPU.'))
    if (-not (Ask-YesNo (M 'それでも起動しますか？' 'Start it anyway?'))) { Done 1 }
} elseif ($pe -and $pe.Level -eq 'highestAvailable') {
    Warn (M ('このアプリは、管理者アカウントでは管理者権限で起動することがあります。その場合は ' + $GJa + ' の GPU を認識できません。') ('This program may start as administrator on an administrator account. Then it cannot see the ' + $GEn + ' GPU.'))
}

# ---- 2. iPhone・Android の様子（数秒で見る） ----
$problem = ''
if ($Android) {
    # adb forward の PC の口（127.0.0.1:口）につないで、受け手にあいさつする。受け手は一度に 1 人しか相手をしない
    $hp = $env:IPHONE_VK
    $ahost = '127.0.0.1'; $aport = 50300
    if ($hp -match '^(.+):(\d+)$') { $ahost = $Matches[1]; $aport = [int]$Matches[2] }
    $c = Connect-Tcp $ahost $aport 1500
    if (-not $c) { $problem = (M 'PC の口 {0} に、スマートフォンの受け手がつながっていません。Start-Android-GPU.cmd か android\start_android.cmd を先に実行してください。' 'Port {0} on this PC is not connected to the receiver on the phone. Run Start-Android-GPU.cmd or android\start_android.cmd first.') -f $aport }
    else {
        try { $h = Invoke-Hello $c 2500 } finally { $c.Close() }
        if ($h.Ok) { Say-OK ((M 'スマートフォンの受け手が応答しました：' 'The receiver on the phone answered: ') + $h.Name) }
        elseif ($h.Why -eq 'timeout') { $problem = M '他のアプリがスマートフォンの GPU を使用しています。そのアプリを終了してから起動してください。' 'Another program is using the phone GPU. Close it first.' }
        elseif ($h.Why -eq 'result' -and $h.Result -eq -9) { $problem = M 'スマートフォンの受け手とこのキットのバージョンが一致しません。android\start_android.cmd を実行し直してください。' 'The receiver on the phone does not match this kit. Run android\start_android.cmd again.' }
        else { $problem = M 'スマートフォンの受け手が応答しません。スマートフォンのロックを解除して、画面をオンにしておいてください。' 'The receiver on the phone does not answer. Unlock the phone and keep the screen on.' }
    }
}
elseif (-not (Test-MuxListening)) { $problem = M 'Apple のサービスが実行されていません。' 'The Apple service is not running.' }
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
    if ($Android) { Say-Info (M 'Windows からスマートフォンの GPU が見えるかは、check_android.cmd でチェックできます。' 'check_android.cmd checks whether Windows sees the phone GPU.') }
    else { Say-Info (M '詳しくは Start-PocketGPU.cmd でチェックできます。' 'Start-PocketGPU.cmd checks this step by step.') }
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
    # Unity のゲームは .exe が UnityPlayer.dll を読むだけで、d3d11.dll は後から読む（取り込みの表に出ない）。Windows の Unity の既定は DirectX 11 なので、DirectX 11 のゲームとして扱う
    $isUnity = ($dxTarget -eq $target) -and ($imp -contains 'unityplayer.dll')
    if ($isUnity) { Say-Info (M 'Unity のゲームのようです。DirectX 11 のゲームとして扱います。' 'This looks like a Unity game. It is treated as a DirectX 11 game.') }
    # API ごとに必要な DXVK のファイル：DirectX 9 は d3d9.dll、DirectX 10 は d3d10core.dll・d3d11.dll・dxgi.dll、DirectX 11 は d3d11.dll・dxgi.dll
    # DXVK 1.10 に d3d10.dll と d3d10_1.dll は無い（DirectX 10 は d3d10core が d3d11 と dxgi を呼ぶ）。Windows の d3d10.dll が、横の d3d10core.dll を読む
    $viaVulkan = $imp -notcontains 'vulkan-1.dll'
    $wantDx9  = ($imp -contains 'd3d9.dll') -and $viaVulkan
    $wantDx10 = (($imp -contains 'd3d10.dll') -or ($imp -contains 'd3d10_1.dll') -or ($imp -contains 'd3d10core.dll')) -and $viaVulkan
    $wantDx11 = ((($imp -contains 'd3d11.dll') -or ($imp -contains 'dxgi.dll')) -and $viaVulkan) -or $isUnity
    if ($imp -contains 'd3d12.dll') {
        Warn (M ('DirectX 12 のゲームのようです。このキットは、まだ DirectX 12 を ' + $GJa + ' の GPU で描画できません。') ('This looks like a DirectX 12 game. This kit cannot draw DirectX 12 on the ' + $GEn + ' GPU yet.'))
    } elseif ($wantDx9 -or $wantDx10 -or $wantDx11) {
        $arch = $(if ($dxPe.Machine -eq 0x8664) { 'x64' } elseif ($dxPe.Machine -eq 0x14c) { 'x32' } else { '' })
        $dir = Split-Path -Parent $dxTarget
        if (-not $arch) {
            Warn (M 'このゲームのアーキテクチャは DXVK に対応していません。' 'This game''s CPU type does not match DXVK.')
        } else {
            # DirectX 9 と、DirectX 10・11 は別々に扱う（DirectX 10 と 11 は d3d11.dll と dxgi.dll を共有する）
            $groups = @()
            if ($wantDx9) { $groups += , @{ Ver = @('9'); Files = @('d3d9.dll') } }
            if ($wantDx10) { $groups += , @{ Ver = @('10') + $(if (($imp -contains 'd3d11.dll') -or $isUnity) { @('11') } else { @() }); Files = @('d3d10core.dll', 'd3d11.dll', 'dxgi.dll') } }
            elseif ($wantDx11) { $groups += , @{ Ver = @('11'); Files = @('d3d11.dll', 'dxgi.dll') } }
            foreach ($g in $groups) {
                $verJa = 'DirectX ' + ($g.Ver -join '・'); $verEn = 'DirectX ' + ($g.Ver -join '/')
                $dxvkSrc = Join-Path $script:KitDir ('dxvk\' + $arch)
                $absent = @(); $other = @(); $same = 0; $noKit = @()
                foreach ($n in $g.Files) {
                    $kitF = Join-Path $dxvkSrc $n
                    $hereF = Join-Path $dir $n
                    if (-not (Test-Path -LiteralPath $kitF)) { $noKit += $n }
                    if (Test-Path -LiteralPath $hereF) {
                        if ((Test-Path -LiteralPath $kitF) -and (Get-FileHashText $hereF) -eq (Get-FileHashText $kitF)) { $same++ } else { $other += $n }
                    } else { $absent += $n }
                }
                if ($other.Count -gt 0) {
                    Warn ((M ('ゲームと同じフォルダに別の ' + (Join-Names $other $true) + ' があります。上書きしないため、' + $verJa + ' は普通の GPU で描画されます。') ('There is another ' + (Join-Names $other $false) + ' next to the game. It is not replaced, so ' + $verEn + ' will use the normal GPU.')))
                } elseif ($absent.Count -eq 0) {
                    Say-OK (M ('DXVK（' + $verJa + '）がゲームと同じフォルダに配置されています。') ('DXVK (' + $verEn + ') is next to the game.'))
                } elseif ($noKit.Count -gt 0) {
                    Warn (M ('このキットには DXVK の ' + (Join-Names $noKit $true) + ' がありません。Update-PocketGPU.cmd でキットを更新してください。') ('This kit has no DXVK ' + (Join-Names $noKit $false) + '. Run Update-PocketGPU.cmd to update the kit.'))
                } else {
                    $cnt = $absent.Count
                    $cntEn = @('', 'one', 'two', 'three', 'four')[$cnt]
                    Say-Info (M ($verJa + ' のゲームのようです。' + $GJa + ' の GPU で描画するには、DXVK の ' + $cnt + ' つのファイルをゲームと同じフォルダに配置します。') ('This looks like a ' + $verEn.Replace('/', ' / ') + ' game. To draw it on the ' + $GEn + ' GPU, ' + $cntEn + ' DXVK file' + $(if ($cnt -gt 1) { 's' } else { '' }) + ' go next to the game.'))
                    Say-Info ((M '配置先：' 'Where: ') + $dir)
                    Say-Info (M ('元に戻すときは、そのフォルダの ' + (Join-Names $absent $true) + ' を削除してください。') ('To undo, delete ' + (Join-Names $absent $false) + ' there.'))
                    if (Ask-YesNo (M 'DXVK を配置しますか？' 'Put DXVK there?')) {
                        try {
                            foreach ($n in $absent) { Copy-Item -LiteralPath (Join-Path $dxvkSrc $n) -Destination $dir }
                            Say-OK (M '配置しました。' 'Done.')
                        } catch {
                            Warn ((M '配置できませんでした。管理者権限がないと書き込めない場所の可能性があります：' 'Could not copy. The folder may need administrator rights: ') + $_.Exception.Message)
                        }
                    }
                }
            }
        }
    }
    # Steam のゲームは、Steam から起動し直すことがある。そのときはこの設定が渡らない
    if ((Test-Path (Join-Path $exeDir 'steam_api64.dll')) -or (Test-Path (Join-Path $exeDir 'steam_api.dll'))) {
        Say-Info (M ('Steam のゲームのようです。Steam から再起動されると、' + $GJa + ' の GPU は使用されません。その場合は、Steam の「起動オプション」に次の 1 行を入力してください。') ('This looks like a Steam game. If Steam restarts it, the ' + $GEn + ' GPU is not used. Then put this line in Steam''s Launch Options:'))
        Say-Info ('"' + (Join-Path $script:KitDir $Launcher) + '" %command%')
    }
}

# ---- 4. 起動 ----
$envs = Get-PgpuEnv
# Android：DXVK にスマートフォンの GPU を選ばせる。名前を IPHONE_VK_NAME で変えたときは付けない（名前に PocketGPU が入らないかもしれない）
if ($Android -and -not $env:DXVK_FILTER_DEVICE_NAME -and -not $env:IPHONE_VK_NAME) { $envs['DXVK_FILTER_DEVICE_NAME'] = 'PocketGPU' }
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
if ($Android) { Say-OK (M '起動しました。スマートフォンのロックを解除して、画面をオンにしたままにしてください。' 'Started. Keep the phone unlocked with the screen on.') }
else { Say-OK (M '起動しました。iPhone のアプリは前面に表示したままにしてください。' 'Started. Keep the iPhone app in front.') }
Done 0
