# Fix-PocketGPU の試験。PC の中だけで、わざと壊したキットを作って、直せることを確かめる。Windows PowerShell 5.1 で動く。
#   powershell -ExecutionPolicy Bypass -File tests\run-tests.ps1 [-Work フォルダ] [-Scenario s1,s2] [-Python python]
# 必要な物：Python 3（偽の usbmuxd と偽の更新サーバーを動かす）・.NET Framework の C# コンパイラ（偽の adb.exe を組む。Windows に入っている）
# 本物の iPhone・Android・Apple のサービス・GPU・インターネットは使わない。窓は出さない。
#
# 1 つの場面ごとに 3 通りで動かす：
#   fix       直すことに「y」と答える。直ったこと（effect）を、画面の文ではなく、ファイル・プロセス・口の本当の状態で確かめる
#   decline   直すことに「n」と答える。何も変わらないこと（効果が出ないこと、消えていないこと）を確かめる
#   sabotage  直す動作だけを何もしない物に差し替えた fix.ps1 で、fix と同じ答えで動かす。効果の確認が失敗することを確かめる（直す部分が本当に効いている証拠）
# 守ること：止めるのは、この試験が自分で起こしたプロセスだけ（PID を指定）。試験用の口・フォルダ以外には触らない。
param(
    [string]$Work = '',
    [string]$Python = 'python',
    [string[]]$Scenario = @(),
    [switch]$KeepWork
)
$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }
$script:Repo = Split-Path -Parent $PSScriptRoot
$Scenario = @($Scenario | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not $Work) { $Work = Join-Path ([IO.Path]::GetTempPath()) 'pgpu-setup-tests' }
$Work = [IO.Path]::GetFullPath($Work)
$script:PS = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$script:Spawned = New-Object System.Collections.Generic.List[int]
$script:Checks = New-Object System.Collections.Generic.List[object]
$script:Now = ''

# ---- 後始末（自分で起こした物だけ）----
function Stop-Spawned {
    foreach ($id in @($script:Spawned)) { try { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue } catch { } }
    $script:Spawned.Clear()
    # 偽の adb が起こした「口を開けて待つ」プロセスと、この作業場所の中の exe（試験用の写し）。この試験の作業場所の中の物だけ
    $prefix = $Work.TrimEnd([char]92) + [string][char]92
    if ($prefix.Length -lt 8) { return }
    foreach ($p in @(Get-Process -ErrorAction SilentlyContinue)) {
        $path = $null
        try { $path = $p.Path } catch { }
        if ($path -and $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { try { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } catch { } }
    }
}
function Start-Hidden([string]$exe, [string]$arguments) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
    $script:Spawned.Add($p.Id)
    return $p
}
# 試験用の口は、OS が自動で割り当てる範囲（49152 以上）と、よく使われる 27xxx を避けた 38000 台から選ぶ。使われていないことを確かめてから返す
$script:PortNext = 38100 + (Get-Random -Minimum 0 -Maximum 800)
function Get-FreePort {
    for ($tries = 0; $tries -lt 400; $tries++) {
        $script:PortNext++
        if ($script:PortNext -gt 48000) { $script:PortNext = 38100 }
        $l = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, $script:PortNext)
        try { $l.Start(); $l.Stop(); return $script:PortNext } catch { }
    }
    throw 'no free test port'
}
function Test-Port([int]$port) {
    $c = New-Object Net.Sockets.TcpClient
    try { $ar = $c.BeginConnect('127.0.0.1', $port, $null, $null); if ($ar.AsyncWaitHandle.WaitOne(800)) { $c.EndConnect($ar); return $true } else { return $false } } catch { return $false } finally { $c.Close() }
}
function Wait-Until([scriptblock]$cond, [int]$sec = 10) {
    $until = (Get-Date).AddSeconds($sec)
    while ((Get-Date) -lt $until) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 200 }
    return [bool](& $cond)
}
function Test-Alive([int]$id) { return [bool](Get-Process -Id $id -ErrorAction SilentlyContinue) }

# ---- 検査の記録 ----
function Check([string]$name, [bool]$ok, [string]$kind = 'effect') {
    $script:Checks.Add([pscustomobject]@{ Scn = $script:Now; Name = $name; Ok = $ok; Kind = $kind })
}

# ---- 偽の物を作る ----
function New-FakeAdb {
    $dir = Join-Path $Work 'fakeadb'
    $exe = Join-Path $dir 'adb.exe'
    if (Test-Path $exe) { return $exe }
    [void](New-Item -ItemType Directory -Path $dir -Force)
    $src = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'fakes\FakeAdb.cs'))
    Add-Type -TypeDefinition $src -OutputAssembly $exe -OutputType ConsoleApplication -ReferencedAssemblies 'System.dll'
    return $exe
}
function Initialize-Pristine {
    $dst = Join-Path $Work 'pristine'
    if (Test-Path $dst) { return $dst }
    [void](New-Item -ItemType Directory -Path $dst -Force)
    Copy-Item -Path (Join-Path $script:Repo 'kit\*') -Destination $dst -Recurse -Force
    & $script:PS -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Normalize-Files.ps1') -Root $dst | Out-Null
    # 中核の代わりのダミー：本物の PE（System32 の version.dll・where.exe）の写し。名前だけ iphone_icd.dll
    Copy-Item (Join-Path $env:WINDIR 'System32\version.dll') (Join-Path $dst 'iphone_icd.dll')
    [void](New-Item -ItemType Directory -Path (Join-Path $dst 'x86') -Force)
    Copy-Item (Join-Path $env:WINDIR 'SysWOW64\version.dll') (Join-Path $dst 'x86\iphone_icd.dll')
    Copy-Item (Join-Path $env:WINDIR 'System32\where.exe') (Join-Path $dst 'vklist.exe')
    $json = "{`r`n    `"file_format_version`": `"1.0.1`",`r`n    `"ICD`": {`r`n        `"library_path`": `".\\iphone_icd.dll`",`r`n        `"api_version`": `"1.2.0`"`r`n    }`r`n}`r`n"
    [IO.File]::WriteAllText((Join-Path $dst 'iphone_icd.json'), $json, [Text.Encoding]::ASCII)
    [IO.File]::WriteAllText((Join-Path $dst 'x86\iphone_icd.json'), $json, [Text.Encoding]::ASCII)
    [IO.File]::WriteAllText((Join-Path $dst 'tools\kit.ini'), "proto=3`r`napp=0.7.58`r`n", [Text.Encoding]::ASCII)
    $rnd = New-Object byte[] 4096
    (New-Object Random 7).NextBytes($rnd)
    [IO.File]::WriteAllBytes((Join-Path $dst 'android\server_aarch64'), $rnd)
    return $dst
}
function New-Kit([string]$name) {
    $dir = Join-Path $Work $name
    if (Test-Path $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    [void](New-Item -ItemType Directory -Path $dir -Force)
    $kit = Join-Path $dir 'kit'
    Copy-Item -Path (Initialize-Pristine) -Destination $kit -Recurse -Force
    return $kit
}
function Get-Sha([string]$p) { return (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash }
function Get-TreeHash([string]$root) {
    $skip = '\\(update-backup|fix-backup)\\|pocketgpu-(fix|update|start)-(log|summary)\.txt$'
    $lines = foreach ($f in (Get-ChildItem -LiteralPath $root -Recurse -File | Where-Object { $_.FullName -notmatch $skip } | Sort-Object FullName)) {
        $f.FullName.Substring($root.Length) + ' ' + (Get-Sha $f.FullName)
    }
    return (($lines -join "`n"))
}
# 古い入口：VK_ICD_FILENAMES を渡す行を抜いた物（10/13 の cbdd483 より前の pocketgpu.cmd など）
function Make-StaleLauncher([string]$path) {
    $t = [IO.File]::ReadAllText($path)
    $t2 = ($t -split "`r?`n" | Where-Object { $_ -notmatch 'VK_ICD_FILENAMES' }) -join "`r`n"
    [IO.File]::WriteAllText($path, $t2, [Text.Encoding]::ASCII)
    if ($t2 -match 'VK_ICD_FILENAMES') { throw 'stale launcher still has VK_ICD_FILENAMES' }
}
# 直す動作だけを何もしない物に差し替えた fix.ps1（対照）。読むだけの動作は本物のまま
function Make-Sabotaged([string]$kit) {
    $p = Join-Path $kit 'tools\fix.ps1'
    $t = [IO.File]::ReadAllText($p, [Text.Encoding]::UTF8)
    $t = $t.Replace('function Invoke-Native(', 'function Invoke-NativeReal(')
    $marker = '# 最初に、直すキットのフォルダを決める'
    if (-not $t.Contains($marker)) { throw 'marker not found' }
    $over = @'
function Invoke-Native([string]$exe, [string]$arguments, [int]$timeoutMs = 20000, $envs = $null, [switch]$Oem) {
    if ($arguments -match 'start_android') { return @{ Ok = $true; Exit = 0; Out = ''; Err = ''; TimedOut = $false } }
    if ($Oem) { return (Invoke-NativeReal $exe $arguments $timeoutMs $envs -Oem) }
    return (Invoke-NativeReal $exe $arguments $timeoutMs $envs)
}
function Replace-Launcher([string]$name) { return @{ Ok = $true; Backup = '' } }
function Set-RememberedPort([string]$key, [int]$port, [string]$androidDir) { }
function Stop-ByPid([int]$procId) { return $true }
function Invoke-KitUpdate { return @{ Ran = $true; Exit = 0 } }
function Invoke-Launch([string]$kind, [string]$target) { return $true }
function Invoke-Adb([string]$adb, [string]$arguments, [int]$ms = 20000) {
    if ($arguments -match 'forward --remove|forward tcp') { return @{ Ok = $true; Exit = 0; Out = ''; Err = ''; TimedOut = $false } }
    return (Invoke-Native $adb $arguments $ms)
}

'@
    $t = $t.Replace($marker, $over + $marker)
    [IO.File]::WriteAllText($p, $t, (New-Object Text.UTF8Encoding($true)))
}

# ---- fix.ps1 を動かす ----
function Invoke-Fix($kit, [string[]]$fixArgs, [hashtable]$envs, [string]$answers, [string]$script = '') {
    $scr = $(if ($script) { $script } else { Join-Path $kit 'tools\fix.ps1' })
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $script:PS
    $psi.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $scr + '" ' + ($fixArgs -join ' ')
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    # 画面の日本語を調べる場面では、受け取りを 1 バイト 1 文字にして、あとで正しく読み直す（子の出力の文字コードが UTF-8 か OEM かに左右されないため）
    if ($script:RawOut) { $psi.StandardOutputEncoding = [Text.Encoding]::GetEncoding(28591) }
    $psi.WorkingDirectory = $Work
    foreach ($k in @($psi.EnvironmentVariables.Keys)) {
        if ($k -like 'PGPU_*' -or $k -like 'IPHONE_VK*' -or $k -in 'ADB', 'ANDROID_SERIAL', 'FAKE_ADB_STATE') { $psi.EnvironmentVariables.Remove($k) }
    }
    $base = @{ PGPU_NOPROMPT = '1'; PGPU_NOPAUSE = '1'; PGPU_TEST_ELEVATED = '0'; PGPU_LANG = 'ja'; PGPU_TEST_VK_LOADER = '1.3.280'; PGPU_TEST_APPLE_SW = 'devices'
               PGPU_TEST_LAUNCH_LOG = (Join-Path (Split-Path -Parent $kit) 'launch.log'); PGPU_TEST_HOLDER_ROOT = $kit; PGPU_VKLIST = (Join-Path $PSScriptRoot 'fakes\fake_vklist.cmd')
               PGPU_TEST_SERVICE_START = (Join-Path $PSScriptRoot 'fakes\noop.cmd'); IPHONE_VK_MUX = (Get-FreePort); PGPU_PORT = (Get-FreePort)
               ADB = (New-FakeAdb); FAKE_ADB_STATE = (Join-Path (Split-Path -Parent $kit) 'adb-state-default.txt') }
    foreach ($k in $base.Keys) { $psi.EnvironmentVariables[$k] = $base[$k] }
    foreach ($k in $envs.Keys) { if ($null -eq $envs[$k]) { [void]$psi.EnvironmentVariables.Remove($k) } else { $psi.EnvironmentVariables[$k] = [string]$envs[$k] } }
    $psi.EnvironmentVariables['PGPU_ANSWERS'] = $answers
    # 止めてよいのは、この試験が起こした物だけ（偽の adb が起こした「口を開けて待つ」物は、fix.ps1 が止める相手ではない）
    $psi.EnvironmentVariables['PGPU_TEST_STOP_ALLOW'] = $(if ($script:Spawned.Count -gt 0) { @($script:Spawned) -join ',' } else { '0' })
    $p = [Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync()
    $e = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(240000)) { try { $p.Kill() } catch { }; return @{ Exit = -99; Out = $o.Result; Err = 'TIMEOUT'; Log = ''; Summary = '' } }
    [void]$p.WaitForExit()
    try { [IO.File]::WriteAllText((Join-Path (Split-Path -Parent $kit) ('stdout-' + (Get-Date -Format 'HHmmss-fff') + '.txt')), $o.Result + "`r`n--- stderr ---`r`n" + $e.Result) } catch { }
    $log = Join-Path $kit 'pocketgpu-fix-log.txt'
    $sum = Join-Path $kit 'pocketgpu-fix-summary.txt'
    return @{ Exit = $p.ExitCode; Out = $o.Result; Err = $e.Result
             Log = $(if (Test-Path $log) { [IO.File]::ReadAllText($log) } else { '' }); Summary = $(if (Test-Path $sum) { [IO.File]::ReadAllText($sum) } else { '' })
             Launch = '' }
}
function Get-Launch($kit) {
    $f = Join-Path (Split-Path -Parent $kit) 'launch.log'
    if (Test-Path $f) { return [IO.File]::ReadAllText($f) } else { return '' }
}

# ---- 偽の usbmuxd・更新サーバー ----
function Start-FakeMux([int]$port, [string[]]$extra = @()) {
    $a = '"' + (Join-Path $PSScriptRoot 'fakes\fake_mux.py') + '" --port ' + $port + ' ' + ($extra -join ' ')
    $p = Start-Hidden $Python $a
    Start-Sleep -Milliseconds 700
    if (-not (Test-Alive $p.Id)) { throw ('fake mux exited (port ' + $port + ' may be in use)') }
    return $p
}
function Start-FakeUpdate([int]$port, [string]$root, [string]$token) {
    $a = '"' + (Join-Path $PSScriptRoot 'fakes\fake_update_server.py') + '" --port ' + $port + ' --root "' + $root + '" --token ' + $token
    $p = Start-Hidden $Python $a
    if (-not (Wait-Until { Test-Port $port } 15)) { throw 'update server did not start' }
    return $p
}
function Start-Blocker([int]$port) {
    $code = "import socket,time; s=socket.socket(); s.bind(('127.0.0.1',$port)); s.listen(5); time.sleep(900)"
    $p = Start-Hidden $Python ('-c "' + $code + '"')
    if (-not (Wait-Until { Test-Port $port } 10)) { throw 'blocker did not start' }
    return $p
}
function Start-Holder([string]$dll) {
    $f = Join-Path $Work ('holder-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.ps1')
    $code = "Add-Type -MemberDefinition '[DllImport(`"kernel32.dll`")] public static extern IntPtr LoadLibrary(string p);' -Name L -Namespace W`r`n[void][W.L]::LoadLibrary('$dll')`r`nStart-Sleep 900`r`n"
    [IO.File]::WriteAllText($f, $code, [Text.Encoding]::ASCII)
    $p = Start-Hidden $script:PS ('-NoProfile -ExecutionPolicy Bypass -File "' + $f + '"')
    $ok = Wait-Until {
        $o = & (Join-Path $env:WINDIR 'System32\tasklist.exe') /m iphone_icd.dll /fo csv /nh
        [bool]($o | Where-Object { $_ -match ('"' + $p.Id + '"') })
    } 25
    if (-not $ok) { throw 'holder did not load the dll' }
    return $p
}
function Get-LauncherEnvValue([string]$kit, [string]$name) {
    $full = Join-Path $kit $name
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $env:ComSpec
    $psi.Arguments = '/c ""' + $full + '" cmd.exe /c set"'
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    foreach ($k in @($psi.EnvironmentVariables.Keys)) { if ($k -like 'VK_*' -or $k -like 'IPHONE_VK*') { $psi.EnvironmentVariables.Remove($k) } }
    $p = [Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync(); $e = $p.StandardError.ReadToEndAsync()
    [void]$p.WaitForExit(15000)
    return ($o.Result -split "`r?`n" | Where-Object { $_ -like 'VK_ICD_FILENAMES=*' } | Select-Object -First 1)
}
function Get-AdbState([string]$statePath) {
    $d = @{}
    if (Test-Path $statePath) { foreach ($l in [IO.File]::ReadAllLines($statePath)) { $i = $l.IndexOf('='); if ($i -gt 0) { $d[$l.Substring(0, $i)] = $l.Substring($i + 1) } } }
    return $d
}
function Set-AdbState([string]$statePath, [hashtable]$d) {
    [IO.File]::WriteAllLines($statePath, [string[]]@($d.Keys | ForEach-Object { $_ + '=' + $d[$_] }))
}
function Test-Hello([int]$port) {
    $c = New-Object Net.Sockets.TcpClient
    try {
        $ar = $c.BeginConnect('127.0.0.1', $port, $null, $null)
        if (-not $ar.AsyncWaitHandle.WaitOne(1500)) { return $false }
        $c.EndConnect($ar)
        $s = $c.GetStream(); $s.ReadTimeout = 2500
        $req = New-Object byte[] 12; [BitConverter]::GetBytes([uint32]1).CopyTo($req, 0); [BitConverter]::GetBytes([uint32]4).CopyTo($req, 4); [BitConverter]::GetBytes([uint32]3).CopyTo($req, 8)
        $s.Write($req, 0, 12)
        $h = New-Object byte[] 8; $got = 0
        while ($got -lt 8) { $k = $s.Read($h, $got, 8 - $got); if ($k -le 0) { return $false }; $got += $k }
        return ([BitConverter]::ToInt32($h, 0) -eq 0)
    } catch { return $false } finally { $c.Close() }
}

# 3 通りの動かし方の共通部分：直す答え・断る答え
function Get-Mode([string]$mode, [string]$fixAnswers) {
    if ($mode -eq 'decline') { return ((($fixAnswers -split ',') | ForEach-Object { 'n' }) -join ',') }
    return $fixAnswers
}
function Use-Mode([string]$mode, $kit) { if ($mode -eq 'sabotage') { Make-Sabotaged $kit } }

# =====================================================================
# 場面
# =====================================================================

# 古いローダー（1.3.204 のふり）+ VK_ICD_FILENAMES を渡さない古い入口 -> 新しい入口に入れ替える
function Scn-Loader([string]$mode) {
    $kit = New-Kit "loader-$mode"
    $mux = Get-FreePort
    $names = 'pocketgpu.cmd', 'pocketgpu_android.cmd', 'check_android.cmd'
    foreach ($n in $names) { Make-StaleLauncher (Join-Path $kit $n) }
    $before = @{}; foreach ($n in $names) { $before[$n] = Get-Sha (Join-Path $kit $n) }
    Use-Mode $mode $kit
    $r = Invoke-Fix $kit @('-Only', 'loader') @{ PGPU_TEST_VK_LOADER = '1.3.204'; IPHONE_VK_MUX = $mux; PGPU_TEST_LAUNCH_LOG = (Join-Path (Split-Path -Parent $kit) 'launch.log') } (Get-Mode $mode 'y,y,y,n')
    $fixedAll = $true
    foreach ($n in $names) {
        $full = Join-Path $kit $n
        $has = $(if ($n -eq 'check_android.cmd') { (Get-Content -Raw $full) -match 'set\s+"?VK_ICD_FILENAMES=' } else { [bool](Get-LauncherEnvValue $kit $n) })
        if (-not $has) { $fixedAll = $false }
    }
    Check 'ローダー 1.3.204 でも、3 つの入口が VK_ICD_FILENAMES を渡す（実際に動かして確認）' $fixedAll
    if ($mode -eq 'fix') {
        $bk = @(Get-ChildItem (Join-Path $kit 'fix-backup') -Recurse -File -ErrorAction SilentlyContinue)
        Check '入れ替える前の 3 つが fix-backup に元の中身のまま残っている' (($bk.Count -eq 3) -and (@($bk | Where-Object { $before[$_.Name] -eq (Get-Sha $_.FullName) }).Count -eq 3)) 'safety'
        Check '入れ替えたあとの中身が、新しい入口（fix-data）と同じ' ((Get-Sha (Join-Path $kit 'pocketgpu.cmd')) -eq (Get-Sha (Join-Path $kit 'tools\fix-data\pocketgpu.cmd'))) 'safety'
        Check '終わりのコードが 0・まとめに「直った」' (($r.Exit -eq 0) -and ($r.Summary -match '\[直った\]')) 'safety'
    } else {
        Check '直さなかった物は、元のまま（SHA-256 が同じ）' (@($names | Where-Object { $before[$_] -ne (Get-Sha (Join-Path $kit $_)) }).Count -eq 0) 'safety'
        Check '終わりのコードが 1（直っていない）' ($r.Exit -eq 1) 'safety'
        Check '「聞かずに入れ替える」ことをしていない（fix-backup が無い）' (-not (Test-Path (Join-Path $kit 'fix-backup'))) 'safety'
    }
    Check 'ブラウザ・ストアを勝手に開いていない（開いたのは y と答えた分だけ）' ((Get-Launch $kit) -notmatch '(?m)^(url|app|service) ') 'safety'
}

# 古いローダーだが入口は新しい -> 何も入れ替えず、注意だけ
function Scn-LoaderOkLaunchers([string]$mode) {
    if ($mode -ne 'fix') { return }
    $kit = New-Kit 'loader-ok'
    $before = Get-TreeHash $kit
    $r = Invoke-Fix $kit @('-Only', 'loader') @{ PGPU_TEST_VK_LOADER = '1.3.204'; IPHONE_VK_MUX = (Get-FreePort) } 'n'
    Check '入口が対応済みなら、古いローダーでも何も変えない' ((Get-TreeHash $kit) -eq $before -and $r.Summary -match '気をつけたい') 'safety'
    Check '終わりのコードが 0' ($r.Exit -eq 0) 'safety'
}

# vulkan-1.dll が無い -> 直せない。GPU ドライバのページを案内（聞いてから開く）
function Scn-LoaderNone([string]$mode) {
    if ($mode -ne 'fix') { return }
    $kit = New-Kit 'loader-none'
    $r = Invoke-Fix $kit @('-Only', 'loader') @{ PGPU_TEST_VK_LOADER = 'none'; IPHONE_VK_MUX = (Get-FreePort) } 'y'
    Check 'vulkan-1.dll が無いときは「直らなかった」と案内し、y と答えたのでドライバのページを開く' (($r.Summary -match '\[直らなかった\]') -and ((Get-Launch $kit) -match 'url (https://|ms-settings:)') -and $r.Exit -eq 1) 'safety'
}

# キットのファイルが欠け・壊れ -> update.ps1 で取り直す
function Scn-Files([string]$mode) {
    $kit = New-Kit "files-$mode"
    $pristine = Initialize-Pristine
    $port = Get-FreePort
    $token = 'TESTTOKENabcdefghijklmnop123'  # pgpu-allow-secret: 試験用の偽の値
    [IO.File]::WriteAllText((Join-Path $kit 'tools\update.ini'), "url=http://127.0.0.1:$port`r`ntoken=$token`r`n", [Text.Encoding]::ASCII)
    [void](Start-FakeUpdate $port $pristine $token)
    Remove-Item (Join-Path $kit 'iphone_icd.dll')
    Remove-Item (Join-Path $kit 'x86\iphone_icd.json')
    $startPs1 = Join-Path $kit 'tools\start.ps1'
    $b = [IO.File]::ReadAllBytes($startPs1); [IO.File]::WriteAllBytes($startPs1, $b[0..([int]($b.Length / 3))])
    [IO.File]::WriteAllBytes((Join-Path $kit 'vklist.exe'), [byte[]]@())
    $damaged = @('iphone_icd.dll', 'x86\iphone_icd.json', 'tools\start.ps1', 'vklist.exe')
    $oldStart = Get-Sha $startPs1
    Use-Mode $mode $kit
    $r = Invoke-Fix $kit @('-Only', 'files') @{ IPHONE_VK_MUX = (Get-FreePort) } (Get-Mode $mode 'y')
    $okAll = $true
    foreach ($d in $damaged) {
        $f = Join-Path $kit $d
        if (-not ((Test-Path $f) -and ((Get-Sha $f) -eq (Get-Sha (Join-Path $pristine $d))))) { $okAll = $false }
    }
    Check '欠けた・壊れた 4 つのファイルが、正しいキットと同じ SHA-256 に戻った' $okAll
    if ($mode -eq 'fix') {
        $bk = @(Get-ChildItem (Join-Path $kit 'update-backup') -Recurse -File -Filter 'start.ps1' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match '\\files\\' })
        Check '壊れていた tools\start.ps1 が update-backup に残っている（捨てていない）' (($bk.Count -ge 1) -and ((Get-Sha $bk[0].FullName) -eq $oldStart)) 'safety'
        Check '終わりのコードが 0' ($r.Exit -eq 0) 'safety'
    } else {
        Check '断ったときは、壊れたまま・update-backup も作らない・終わりのコード 1' ((-not (Test-Path (Join-Path $kit 'update-backup'))) -and $r.Exit -eq 1) 'safety'
    }
    Check '合言葉が、画面・ログ・まとめに出ていない' (-not (($r.Out + $r.Log + $r.Summary) -match $token)) 'safety'
}

# update.ini が無いキットの欠け -> 取り直せない。案内だけ
function Scn-FilesNoIni([string]$mode) {
    if ($mode -ne 'fix') { return }
    $kit = New-Kit 'files-noini'
    Remove-Item (Join-Path $kit 'iphone_icd.dll')
    $r = Invoke-Fix $kit @('-Only', 'files') @{ IPHONE_VK_MUX = (Get-FreePort) } 'y'
    Check 'update.ini が無いときは、勝手に何かを取りに行かず、直らなかったと案内する' ((-not (Test-Path (Join-Path $kit 'iphone_icd.dll'))) -and $r.Log -match 'update.ini' -and $r.Exit -eq 1) 'safety'
}

# Apple のサービスが止まっている（iTunes のサービスのふり）-> 開始する
function Scn-AppleService([string]$mode) {
    $kit = New-Kit "apple-svc-$mode"
    $mux = Get-FreePort
    $flag = Join-Path (Split-Path -Parent $kit) 'service-started.flag'
    $hook = Join-Path (Split-Path -Parent $kit) 'start-service.cmd'
    [IO.File]::WriteAllText($hook, "@echo off`r`necho started> `"$flag`"`r`n", [Text.Encoding]::ASCII)
    [void](Start-FakeMux $mux @('--listen-after', ('"' + $flag + '"')))
    Use-Mode $mode $kit
    $r = Invoke-Fix $kit @('-Only', 'apple') @{ PGPU_TEST_APPLE_SW = 'service'; IPHONE_VK_MUX = $mux; PGPU_TEST_SERVICE_START = $hook } (Get-Mode $mode 'y')
    $up = Wait-Until { Test-Port $mux } 3
    Check 'Apple のサービスが開始され、口 (試験用) に実際につながる' $up
    if ($mode -eq 'fix') { Check 'まとめに「直った」・終わりのコード 0' (($r.Summary -match '\[直った\]') -and $r.Exit -eq 0) 'safety' }
    else { Check '終わりのコード 1・まとめに「直らなかった」' (($r.Summary -match '\[直らなかった\]') -and $r.Exit -eq 1) 'safety' }
}
function Scn-AppleDenied([string]$mode) {
    if ($mode -ne 'fix') { return }
    $kit = New-Kit 'apple-denied'
    $r = Invoke-Fix $kit @('-Only', 'apple') @{ PGPU_TEST_APPLE_SW = 'service'; PGPU_TEST_SERVICE_DENIED = '1'; IPHONE_VK_MUX = (Get-FreePort) } 'y'
    Check '管理者権限が要るときは、その旨と Start-Service の 1 行を出し、直らなかったとする' (($r.Log -match 'Start-Service -Name') -and ($r.Log -match '管理者') -and $r.Exit -eq 1) 'safety'
}
function Scn-AppleDevices([string]$mode) {
    $kit = New-Kit "apple-dev-$mode"
    $mux = Get-FreePort
    $flag = Join-Path (Split-Path -Parent $kit) 'app-started.flag'
    $hook = Join-Path (Split-Path -Parent $kit) 'open-app.cmd'
    [IO.File]::WriteAllText($hook, "@echo off`r`necho started> `"$flag`"`r`n", [Text.Encoding]::ASCII)
    [void](Start-FakeMux $mux @('--listen-after', ('"' + $flag + '"')))
    Use-Mode $mode $kit
    $r = Invoke-Fix $kit @('-Only', 'apple') @{ PGPU_TEST_APPLE_SW = 'devices'; IPHONE_VK_MUX = $mux; PGPU_TEST_SERVICE_START = $hook } (Get-Mode $mode 'y')
    Check 'Apple Devices を起動して、Apple のサービスが始まる（口につながる）' (Wait-Until { Test-Port $mux } 3)
    if ($mode -eq 'fix') { Check '起動した物の名前が Apple Devices' ((Get-Launch $kit) -match 'app AppleInc.AppleDevices_') 'safety' }
}
function Scn-AppleNone([string]$mode) {
    if ($mode -ne 'fix') { return }
    $kit = New-Kit 'apple-none'
    $r = Invoke-Fix $kit @('-Only', 'apple') @{ PGPU_TEST_APPLE_SW = 'none'; IPHONE_VK_MUX = (Get-FreePort) } 'y'
    Check 'Apple のソフトが無いときは、Store のページを（y と答えたので）開く・直らなかったとする' (((Get-Launch $kit) -match 'url ms-windows-store://') -and $r.Exit -eq 1) 'safety'
}
# 口を別のプログラムが使っている -> 止めると、Apple のサービス（偽）が口を取る
function Scn-AppleBlocker([string]$mode) {
    $kit = New-Kit "apple-block-$mode"
    $mux = Get-FreePort
    $blocker = Start-Blocker $mux
    [void](Start-FakeMux $mux @('--wait-port-free'))
    Use-Mode $mode $kit
    $r = Invoke-Fix $kit @('-Only', 'apple') @{ PGPU_TEST_APPLE_SW = 'none'; IPHONE_VK_MUX = $mux } (Get-Mode $mode 'n,y')
    Start-Sleep -Milliseconds 800
    $gone = -not (Test-Alive $blocker.Id)
    Check '口を使っていた別のプログラム（試験用）が止まり、Apple のサービス（偽）が口を使えるようになる' ($gone -and (Wait-Until { Test-Port $mux } 6))
    if ($mode -ne 'fix') { Check '止めなかったとき、別のプログラムは生きたまま' (Test-Alive $blocker.Id) 'safety' }
    Check 'PID が出ている行は、試験で起こしたプログラムの PID だけ' ((@([regex]::Matches($r.Log, 'PID (\d+)') | ForEach-Object { [int]$_.Groups[1].Value } | Where-Object { $_ -ne $blocker.Id }).Count) -eq 0) 'safety'
}

# 同じ道具が二重に動いている -> 古い方を止める
function Scn-ToolsDup([string]$mode) {
    $kit = New-Kit "tools-$mode"
    [void](New-Item -ItemType Directory -Path (Join-Path $kit 'extras') -Force)
    $exe = Join-Path $kit 'extras\webcam_rx.exe'
    Copy-Item (Join-Path $env:WINDIR 'System32\ping.exe') $exe
    $old = Start-Hidden $exe '-t 127.0.0.1'
    Start-Sleep -Milliseconds 1500
    $new = Start-Hidden $exe '-t 127.0.0.1'
    $other = Start-Hidden (Join-Path $env:WINDIR 'System32\ping.exe') '-t 127.0.0.1'
    Start-Sleep -Milliseconds 800
    Use-Mode $mode $kit
    $r = Invoke-Fix $kit @('-Only', 'tools') @{ IPHONE_VK_MUX = (Get-FreePort) } (Get-Mode $mode 'y')
    Start-Sleep -Milliseconds 500
    Check '二重のうち古い方が止まる' (-not (Test-Alive $old.Id))
    Check '新しい方は動いたまま' (Test-Alive $new.Id) 'safety'
    Check 'キットの外の同じ名前でない別のプログラムには触れていない' (Test-Alive $other.Id) 'safety'
    if ($mode -eq 'decline') { Check '断ったので古い方も生きている' (Test-Alive $old.Id) 'safety' }
}

# 受け手を別のプログラムが握っている（iPhone の GPU が busy）-> 止めると応答する
function Scn-IphoneHolder([string]$mode) {
    $kit = New-Kit "iphone-holder-$mode"
    $mux = Get-FreePort
    $holder = Start-Holder (Join-Path $kit 'iphone_icd.dll')
    [void](Start-FakeMux $mux @('--busy-pid', $holder.Id))
    if (-not (Wait-Until { Test-Port $mux } 10)) { throw 'fake mux did not start' }
    Use-Mode $mode $kit
    $r = Invoke-Fix $kit @('-Only', 'apple,iphone') @{ PGPU_TEST_APPLE_SW = 'devices'; IPHONE_VK_MUX = $mux } (Get-Mode $mode 'y')
    Start-Sleep -Milliseconds 500
    Check '握っていたプログラム（試験用）が止まる' (-not (Test-Alive $holder.Id))
    if ($mode -eq 'fix') { Check '握りが外れて、受け手（偽）が応答し、まとめに「直った」' (($r.Summary -match '\[直った\] iPhone') -and $r.Exit -eq 0) 'safety' }
    else { Check '直っていない（終わりのコード 1）' ($r.Exit -eq 1) 'safety' }
    Check '止める相手として出た PID は、試験の握り手だけ' ((@([regex]::Matches($r.Log, 'PID (\d+)') | ForEach-Object { [int]$_.Groups[1].Value } | Where-Object { $_ -ne $holder.Id }).Count) -eq 0) 'safety'
}
function Scn-IphoneOther([string]$mode) {
    if ($mode -ne 'fix') { return }
    $kit = New-Kit 'iphone-other'
    $mux = Get-FreePort
    [void](Start-FakeMux $mux @('--recv', 'version'))
    Wait-Until { Test-Port $mux } 10 | Out-Null
    $r = Invoke-Fix $kit @('-Only', 'apple,iphone') @{ IPHONE_VK_MUX = $mux } 'y'
    Check '版の違い（-9）のときは、キットの ipa の名前を案内して、何も変えない' (($r.Log -match 'RpcServer-0.7.58.ipa') -and $r.Exit -eq 1) 'safety'
    $mux2 = Get-FreePort
    [void](Start-FakeMux $mux2 @('--devices', 'none'))
    Wait-Until { Test-Port $mux2 } 10 | Out-Null
    $r2 = Invoke-Fix $kit @('-Only', 'apple,iphone') @{ IPHONE_VK_MUX = $mux2 } 'y'
    Check 'USB で見えないときは、つなぐ・ロック解除の案内を出して直らなかったとする' (($r2.Log -match '認識されていません') -and $r2.Exit -eq 1) 'safety'
    $mux3 = Get-FreePort
    [void](Start-FakeMux $mux3 @('--recv', 'silent'))
    Wait-Until { Test-Port $mux3 } 10 | Out-Null
    $r3 = Invoke-Fix $kit @('-Only', 'apple,iphone') @{ IPHONE_VK_MUX = $mux3 } 'y'
    Check '握っているプログラムが PC に無いときは、iPhone のアプリを開き直す案内（誰も止めない）' (($r3.Log -match '開き直してください') -and $r3.Exit -eq 1) 'safety'
}

# Android：adb の状態は偽の adb.exe（状態ファイル）。PC の口は偽の adb が開く
#   $more：最初から足しておく adb forward。@{ Port = 口; Target = 'tcp:50060' や 'localabstract:pocketgpu-webcam' } の並び
function New-AndroidKit([string]$name, [hashtable]$state, [bool]$withForward, [object[]]$more = @()) {
    $kit = New-Kit $name
    $adb = New-FakeAdb
    $st = Join-Path (Split-Path -Parent $kit) 'adb-state.txt'
    $port = Get-FreePort
    $cam = Get-FreePort
    $aud = Get-FreePort
    $localHash = (Get-Sha (Join-Path $kit 'android\server_aarch64')).ToLowerInvariant()
    if ($state.hash -eq '@local') { $state.hash = $localHash }
    Set-AdbState $st $state
    $env:FAKE_ADB_STATE = $st
    try {
        if ($withForward) { & $adb forward "tcp:$port" "tcp:$port" | Out-Null }
        foreach ($f in $more) {
            $p = $(if ($f.Port -eq '@cam') { $cam } elseif ($f.Port -eq '@aud') { $aud } elseif ($f.Port -eq '@gpu') { $port } else { $f.Port })
            & $adb forward "tcp:$p" $f.Target | Out-Null
        }
    } finally { Remove-Item Env:\FAKE_ADB_STATE }
    foreach ($id in (Get-FwdPids $st)) { $script:Spawned.Add($id) }
    return @{ Kit = $kit; Adb = $adb; State = $st; Port = $port; CamPort = $cam; AudioPort = $aud; LocalHash = $localHash }
}
function Invoke-AndroidFix($k, [string]$answers, [string[]]$more = @(), [hashtable]$extraEnv = @{}) {
    $e = @{ ADB = $k.Adb; FAKE_ADB_STATE = $k.State; PGPU_PORT = $k.Port; PGPU_CAM_PORT = $k.CamPort; PGPU_AUDIO_PORT = $k.AudioPort; IPHONE_VK_MUX = (Get-FreePort) }
    foreach ($key in $extraEnv.Keys) { $e[$key] = $extraEnv[$key] }
    return (Invoke-Fix $k.Kit (@('-Only', 'android') + $more) $e $answers)
}
# 偽の adb の forward の一覧（状態ファイルから）：口 -> 向け先
function Get-FwdMap([string]$statePath) {
    $m = @{}
    $s = Get-AdbState $statePath
    if ($s.fwds) { foreach ($e in ($s.fwds -split ';')) { $p = $e -split '>'; if ($p.Count -ge 2) { $m[$p[0]] = $p[1] } } }
    return $m
}
function Get-FwdPids([string]$statePath) {
    $l = @()
    $s = Get-AdbState $statePath
    if ($s.fwds) { foreach ($e in ($s.fwds -split ';')) { $p = $e -split '>'; if ($p.Count -ge 3 -and $p[2] -match '^\d+$') { $l += [int]$p[2] } } }
    return $l
}
# アプリの口につないで、偽のアプリの答え（webcam か audio）を読む。答えなければ空
function Test-AppAnswer([int]$port) {
    $c = New-Object Net.Sockets.TcpClient
    try {
        $ar = $c.BeginConnect('127.0.0.1', $port, $null, $null)
        if (-not $ar.AsyncWaitHandle.WaitOne(1500)) { return '' }
        $c.EndConnect($ar)
        $s = $c.GetStream(); $s.ReadTimeout = 1500
        $b = New-Object byte[] 64
        $n = $s.Read($b, 0, 64)
        if ($n -le 0) { return '' }
        $t = [Text.Encoding]::ASCII.GetString($b, 0, $n)
        if ($t -match '^PGFAKE-APP (\w+)') { return $Matches[1] } else { return '' }
    } catch { return '' } finally { $c.Close() }
}
$script:AppNew = @{ Cam = 'localabstract:pocketgpu-webcam'; Aud = 'localabstract:pocketgpu-audio' }
$script:AppOld = @{ Cam = 'tcp:50064'; Aud = 'tcp:50063' }
# アプリの中の GPU の受け手：0.1.4 以上は USB 用の口でも待つ
$script:AppGpuUsb = 'localabstract:pocketgpu-gpu'
# アプリが入っていて、アプリの中の受け手（50060）だけが動く形の端末の状態
function New-AppState([string]$ver) { return @{ serial = 'EMU1'; dstate = 'device'; pids = ''; hash = ''; file = '0'; next = '7001'; app = $ver; apprecv = '1' } }

function Scn-AndroidDupOld([string]$mode) {
    $k = New-AndroidKit "android-dup-$mode" @{ serial = 'EMU1'; dstate = 'device'; pids = '1111 2222'; hash = 'ab' * 32; file = '1'; next = '7001' } $true
    Use-Mode $mode $k.Kit
    $r = Invoke-AndroidFix $k (Get-Mode $mode 'y')
    $s = Get-AdbState $k.State
    $one = (@($s.pids -split ' ' | Where-Object { $_ }).Count -eq 1)
    Check '受け手が二重・古い（SHA 不一致）だったのが、1 つになり、キットの受け手と同じ SHA になる' ($one -and ($s.hash -eq $k.LocalHash))
    Check 'adb forward がつながり、口で受け手が応答する（実際につないで確認）' (Test-Hello $k.Port) 'safety'
    if ($mode -eq 'fix') { Check 'まとめに「直った」・終わりのコード 0' (($r.Summary -match '\[直った\] Android') -and $r.Exit -eq 0) 'safety' }
    else { Check '断ったので二重のまま・終わりのコード 1' (($s.pids -eq '1111 2222') -and $r.Exit -eq 1) 'safety' }
}
function Scn-AndroidStaleFwd([string]$mode) {
    # 端末が無いのに、GPU の口と、アプリのカメラとマイクの口（新旧の両方の形）が残っている
    $k = New-AndroidKit "android-stale-$mode" @{ serial = ''; pids = ''; hash = ''; file = '0'; next = '7001' } $true @(
        @{ Port = '@cam'; Target = $script:AppNew.Cam }, @{ Port = '@aud'; Target = $script:AppOld.Aud })
    Use-Mode $mode $k.Kit
    $r = Invoke-AndroidFix $k (Get-Mode $mode 'y')
    $m = Get-FwdMap $k.State
    Check '端末が無いのに残っていた adb forward（GPU の口・カメラ・マイク）が全部外れ、口も閉じる' (($m.Count -eq 0) -and (Wait-Until { -not (Test-Port $k.Port) -and -not (Test-Port $k.CamPort) -and -not (Test-Port $k.AudioPort) } 4))
    if ($mode -eq 'decline') { Check '断ったので 3 つとも残ったまま' ($m.Count -eq 3) 'safety' }
}
function Scn-AndroidPortConflict([string]$mode) {
    $k = New-AndroidKit "android-port-$mode" @{ serial = 'EMU1'; dstate = 'device'; pids = '1111'; hash = '@local'; file = '1'; next = '7001' } $false
    $blocker = Start-Blocker $k.Port
    Use-Mode $mode $k.Kit
    $r = Invoke-AndroidFix $k (Get-Mode $mode 'y,y')
    Start-Sleep -Milliseconds 500
    Check '口を使っていた adb ではないプログラムが止まり、adb forward がつながって応答する' ((-not (Test-Alive $blocker.Id)) -and (Test-Hello $k.Port))
    if ($mode -eq 'decline') { Check '断ったので別のプログラムは生きている' (Test-Alive $blocker.Id) 'safety' }
}
function Scn-AndroidFwdMissing([string]$mode) {
    $k = New-AndroidKit "android-fwd-$mode" @{ serial = 'EMU1'; dstate = 'device'; pids = '1111'; hash = '@local'; file = '1'; next = '7001' } $false
    Use-Mode $mode $k.Kit
    $r = Invoke-AndroidFix $k (Get-Mode $mode 'y')
    $s = Get-AdbState $k.State
    foreach ($id in (Get-FwdPids $k.State)) { $script:Spawned.Add($id) }
    Check 'adb forward が作り直され、応答する' (Test-Hello $k.Port)
    Check '受け手（スマートフォン側）には触れていない（PID・SHA がそのまま）' (($s.pids -eq '1111') -and ($s.hash -eq $k.LocalHash)) 'safety'
}

# アプリ 0.1.2：カメラとマイクの口が古い向け方（tcp:50064・tcp:50063）のまま -> 新しい向け方（localabstract）につなぎ直す。GPU はアプリの受け手（50060）で動いたまま
function Scn-AndroidAppOldFormOnNew([string]$mode) {
    $k = New-AndroidKit "android-app-oldform-$mode" (New-AppState '0.1.2') $false @(
        @{ Port = '@gpu'; Target = 'tcp:50060' }, @{ Port = '@cam'; Target = $script:AppOld.Cam }, @{ Port = '@aud'; Target = $script:AppOld.Aud })
    Check '準備：アプリ 0.1.2 は古い向け方を断る（偽のアプリが答えない）' ((Test-AppAnswer $k.CamPort) -eq '') 'safety'
    Use-Mode $mode $k.Kit
    $r = Invoke-AndroidFix $k (Get-Mode $mode 'y')
    $m = Get-FwdMap $k.State
    foreach ($id in (Get-FwdPids $k.State)) { $script:Spawned.Add($id) }
    Check 'カメラとマイクの adb forward が新しい向け方（localabstract）になる' (($m[[string]$k.CamPort] -eq $script:AppNew.Cam) -and ($m[[string]$k.AudioPort] -eq $script:AppNew.Aud))
    Check 'PC の口で、アプリがカメラ・マイクとして応答する（実際につないで確認）' (((Test-AppAnswer $k.CamPort) -eq 'webcam') -and ((Test-AppAnswer $k.AudioPort) -eq 'audio'))
    Check 'GPU の口はアプリの受け手（tcp:50060）のまま・応答する（触れていない）' (($m[[string]$k.Port] -eq 'tcp:50060') -and (Test-Hello $k.Port)) 'safety'
    Check 'アプリ（版・受け手）には触れていない' ((Get-AdbState $k.State).app -eq '0.1.2' -and (Get-AdbState $k.State).apprecv -eq '1') 'safety'
    if ($mode -eq 'fix') { Check 'まとめに「直った」・終わりのコード 0' (($r.Summary -match '\[直った\] Android') -and $r.Exit -eq 0) 'safety' }
    else { Check '直していないので、終わりのコード 1・古い向け方のまま' (($r.Exit -eq 1) -and ($m[[string]$k.CamPort] -eq $script:AppOld.Cam)) 'safety' }
}
# アプリ 0.1.1：新しい向け方（localabstract）で作られている -> 古い向け方につなぎ直す
function Scn-AndroidAppNewFormOnOld([string]$mode) {
    $k = New-AndroidKit "android-app-newform-$mode" (New-AppState '0.1.1') $false @(
        @{ Port = '@gpu'; Target = 'tcp:50060' }, @{ Port = '@cam'; Target = $script:AppNew.Cam }, @{ Port = '@aud'; Target = $script:AppNew.Aud })
    Use-Mode $mode $k.Kit
    $r = Invoke-AndroidFix $k (Get-Mode $mode 'y')
    $m = Get-FwdMap $k.State
    foreach ($id in (Get-FwdPids $k.State)) { $script:Spawned.Add($id) }
    Check 'カメラとマイクの adb forward が古い向け方（tcp:50064・tcp:50063）になる' (($m[[string]$k.CamPort] -eq $script:AppOld.Cam) -and ($m[[string]$k.AudioPort] -eq $script:AppOld.Aud))
    Check 'PC の口で、アプリ 0.1.1 がカメラ・マイクとして応答する（実際につないで確認）' (((Test-AppAnswer $k.CamPort) -eq 'webcam') -and ((Test-AppAnswer $k.AudioPort) -eq 'audio'))
    Check 'GPU の口はアプリの受け手（tcp:50060）のまま（触れていない）' ($m[[string]$k.Port] -eq 'tcp:50060') 'safety'
}
# カメラだけつながっていて、マイクの口が無い（欠け）-> 版に合う向け方で足す
function Scn-AndroidAppHalf([string]$mode) {
    $k = New-AndroidKit "android-app-half-$mode" (New-AppState '0.1.2') $false @(
        @{ Port = '@gpu'; Target = 'tcp:50060' }, @{ Port = '@cam'; Target = $script:AppNew.Cam })
    Use-Mode $mode $k.Kit
    $r = Invoke-AndroidFix $k (Get-Mode $mode 'y')
    $m = Get-FwdMap $k.State
    foreach ($id in (Get-FwdPids $k.State)) { $script:Spawned.Add($id) }
    Check '無かったマイクの adb forward が、新しい向け方で足され、応答する' (($m[[string]$k.AudioPort] -eq $script:AppNew.Aud) -and ((Test-AppAnswer $k.AudioPort) -eq 'audio'))
    Check 'つながっていたカメラの口は、そのまま応答する' ((Test-AppAnswer $k.CamPort) -eq 'webcam') 'safety'
}
# adb forward の一覧には残っているが、PC の口が開いていない（adb の再起動のあと）-> つなぎ直す
function Scn-AndroidAppStale([string]$mode) {
    $k = New-AndroidKit "android-app-stale-$mode" (New-AppState '0.1.2') $false @(
        @{ Port = '@gpu'; Target = 'tcp:50060' }, @{ Port = '@cam'; Target = $script:AppNew.Cam }, @{ Port = '@aud'; Target = $script:AppNew.Aud })
    # 口を開けている偽の adb（この試験が起こした物）だけを止める。一覧には残る
    $camPid = [int](((Get-AdbState $k.State).fwds -split ';' | Where-Object { $_ -like ($k.CamPort.ToString() + '>*') }) -split '>')[2]
    Stop-Process -Id $camPid -Force
    Check '準備：カメラの口が閉じ、一覧には残っている' ((Wait-Until { -not (Test-Port $k.CamPort) } 4) -and (Get-FwdMap $k.State).ContainsKey([string]$k.CamPort)) 'safety'
    Use-Mode $mode $k.Kit
    $r = Invoke-AndroidFix $k (Get-Mode $mode 'y')
    foreach ($id in (Get-FwdPids $k.State)) { $script:Spawned.Add($id) }
    Check '閉じていたカメラの口が開き直り、アプリが応答する' ((Test-AppAnswer $k.CamPort) -eq 'webcam')
    Check '閉じていなかったマイクの口は、そのまま応答する' ((Test-AppAnswer $k.AudioPort) -eq 'audio') 'safety'
}
# アプリの口を、adb ではないプログラムが使っている（一覧には残っている）-> 止めてから、つなぎ直す
function Scn-AndroidAppConflict([string]$mode) {
    $k = New-AndroidKit "android-app-conflict-$mode" (New-AppState '0.1.2') $false @(
        @{ Port = '@gpu'; Target = 'tcp:50060' }, @{ Port = '@aud'; Target = $script:AppNew.Aud })
    # カメラの口は、一覧にだけ書いて（口を開ける偽の adb は無し）、別のプログラムに握らせる
    $s = Get-AdbState $k.State
    $s.fwds = $s.fwds + ';' + $k.CamPort + '>' + $script:AppNew.Cam + '>0'
    Set-AdbState $k.State $s
    $blocker = Start-Blocker $k.CamPort
    Use-Mode $mode $k.Kit
    $r = Invoke-AndroidFix $k (Get-Mode $mode 'y,y')
    foreach ($id in (Get-FwdPids $k.State)) { $script:Spawned.Add($id) }
    Check '口を使っていた adb ではないプログラムが止まり、カメラの口がつながって応答する' ((-not (Test-Alive $blocker.Id)) -and ((Test-AppAnswer $k.CamPort) -eq 'webcam'))
    if ($mode -eq 'decline') { Check '断ったので別のプログラムは生きている' (Test-Alive $blocker.Id) 'safety' }
}
# アプリが入っていないのに、カメラとマイクの口が残っている -> 外す。GPU の口（キットの受け手）は残す
function Scn-AndroidAppLeft([string]$mode) {
    $k = New-AndroidKit "android-app-left-$mode" @{ serial = 'EMU1'; dstate = 'device'; pids = '1111'; hash = '@local'; file = '1'; next = '7001' } $true @(
        @{ Port = '@cam'; Target = $script:AppNew.Cam }, @{ Port = '@aud'; Target = $script:AppOld.Aud })
    Use-Mode $mode $k.Kit
    $r = Invoke-AndroidFix $k (Get-Mode $mode 'y')
    $m = Get-FwdMap $k.State
    Check 'アプリが無いのに残っていたカメラとマイクの adb forward が外れ、口も閉じる' (($m.ContainsKey([string]$k.CamPort) -eq $false) -and ($m.ContainsKey([string]$k.AudioPort) -eq $false) -and (Wait-Until { -not (Test-Port $k.CamPort) -and -not (Test-Port $k.AudioPort) } 4))
    Check 'GPU の口（キットの受け手）は残り、応答する' (($m.ContainsKey([string]$k.Port)) -and (Test-Hello $k.Port)) 'safety'
}
# アプリ 0.1.4：端末の 50060 が使えず、アプリの受け手が 50061 で待っている（10/14 テスターの Redmagic 9S Pro の形）。
# GPU の口は古い向け方の tcp:50060 のままで答えない -> USB 用の口（localabstract:pocketgpu-gpu）につなぎ直す。キットの受け手は起こさない
function Scn-AndroidAppGpuTaken([string]$mode) {
    $s = New-AppState '0.1.4'; $s.appport = '50061'
    $k = New-AndroidKit "android-app-gputaken-$mode" $s $false @(
        @{ Port = '@gpu'; Target = 'tcp:50060' }, @{ Port = '@cam'; Target = $script:AppNew.Cam }, @{ Port = '@aud'; Target = $script:AppNew.Aud })
    Check '準備：tcp:50060 では、50061 で待つアプリの受け手に届かない' (-not (Test-Hello $k.Port)) 'safety'
    Use-Mode $mode $k.Kit
    $r = Invoke-AndroidFix $k (Get-Mode $mode 'y')
    $m = Get-FwdMap $k.State
    foreach ($id in (Get-FwdPids $k.State)) { $script:Spawned.Add($id) }
    Check 'GPU の口が localabstract:pocketgpu-gpu になり、アプリの受け手が応答する（実際につないで確認）' (($m[[string]$k.Port] -eq $script:AppGpuUsb) -and (Test-Hello $k.Port))
    $st = Get-AdbState $k.State
    Check 'キットの受け手は起こさず、スマートフォンに置かない' (($st.pids -eq '') -and ($st.file -eq '0')) 'safety'
    Check 'カメラとマイクの口は、そのまま応答する' (((Test-AppAnswer $k.CamPort) -eq 'webcam') -and ((Test-AppAnswer $k.AudioPort) -eq 'audio')) 'safety'
    if ($mode -eq 'fix') { Check 'まとめに「直った」・終わりのコード 0' (($r.Summary -match '\[直った\] Android') -and $r.Exit -eq 0) 'safety' }
    else { Check '直していないので、終わりのコード 1・GPU の口は tcp:50060 のまま' (($r.Exit -eq 1) -and ($m[[string]$k.Port] -eq 'tcp:50060')) 'safety' }
}
# アプリ 0.1.3：GPU の口が USB 用の口（0.1.3 は待たない）に向いている -> tcp:50060 につなぎ直す
function Scn-AndroidAppGpuUsbOnOld([string]$mode) {
    $k = New-AndroidKit "android-app-gpuusb-old-$mode" (New-AppState '0.1.3') $false @(
        @{ Port = '@gpu'; Target = $script:AppGpuUsb }, @{ Port = '@cam'; Target = $script:AppNew.Cam }, @{ Port = '@aud'; Target = $script:AppNew.Aud })
    Check '準備：0.1.3 のアプリは、USB 用の GPU の口では答えない' (-not (Test-Hello $k.Port)) 'safety'
    Use-Mode $mode $k.Kit
    $r = Invoke-AndroidFix $k (Get-Mode $mode 'y')
    $m = Get-FwdMap $k.State
    foreach ($id in (Get-FwdPids $k.State)) { $script:Spawned.Add($id) }
    Check 'GPU の口が tcp:50060 になり、アプリ 0.1.3 の受け手が応答する' (($m[[string]$k.Port] -eq 'tcp:50060') -and (Test-Hello $k.Port))
    Check 'キットの受け手は起こさない' ((Get-AdbState $k.State).pids -eq '') 'safety'
    if ($mode -ne 'fix') { Check '直していないので、終わりのコード 1' ($r.Exit -eq 1) 'safety' }
}
# 何も壊れていない：アプリ 0.1.2 の新しい向け方・アプリ 0.1.1 の古い向け方・版が読めない（どちらの向け方も触らない）・
# アプリ 0.1.4 の GPU の USB 用の口・アプリ 0.1.4 で 50060 が空いていて tcp:50060 でも答える（古いキットの android_app.cmd の形。答えるなら触らない）
function Scn-PristineApp([string]$mode) {
    if ($mode -ne 'fix') { return }
    $cases = @(
        @{ Name = 'new'; Ver = '0.1.2'; Cam = $script:AppNew.Cam; Aud = $script:AppNew.Aud; Gpu = 'tcp:50060' },
        @{ Name = 'old'; Ver = '0.1.1'; Cam = $script:AppOld.Cam; Aud = $script:AppOld.Aud; Gpu = 'tcp:50060' },
        @{ Name = 'unknown-old-form'; Ver = 'dev'; Cam = $script:AppOld.Cam; Aud = $script:AppOld.Aud; Gpu = 'tcp:50060' },
        @{ Name = 'unknown-new-form'; Ver = 'dev'; Cam = $script:AppNew.Cam; Aud = $script:AppNew.Aud; Gpu = 'tcp:50060' },
        @{ Name = 'gpu-usb'; Ver = '0.1.4'; Cam = $script:AppNew.Cam; Aud = $script:AppNew.Aud; Gpu = $script:AppGpuUsb },
        @{ Name = 'gpu-tcp-free'; Ver = '0.1.4'; Cam = $script:AppNew.Cam; Aud = $script:AppNew.Aud; Gpu = 'tcp:50060' })
    foreach ($c in $cases) {
        $k = New-AndroidKit ('pristine-app-' + $c.Name) (New-AppState $c.Ver) $false @(
            @{ Port = '@gpu'; Target = $c.Gpu }, @{ Port = '@cam'; Target = $c.Cam }, @{ Port = '@aud'; Target = $c.Aud })
        $before = Get-FwdMap $k.State
        $r = Invoke-AndroidFix $k 'n,n,n'
        $after = Get-FwdMap $k.State
        $same = ($before.Count -eq $after.Count) -and (@($before.Keys | Where-Object { $before[$_] -ne $after[$_] }).Count -eq 0)
        Check ('正常（アプリ ' + $c.Ver + '・' + $c.Name + '）：誤検出なし・adb forward は何も変わらない・終わりのコード 0') (($r.Exit -eq 0) -and $same -and ($r.Summary -match '直った 0 / 直らなかった 0 / 気をつけたい 0 / 問題なし 1')) 'safety'
        Stop-Spawned
    }
}
# android\android_app.cmd を、偽の adb で動かす。版に合う向け方を選ぶこと、選べないときの決め方、stop
function Invoke-KitCmd($k, [string]$rel, [string]$cmdArgs = '', [hashtable]$envs = @{}, [string]$stdin = $null) {
    $cmd = Join-Path $k.Kit $rel
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $env:ComSpec
    $psi.Arguments = '/c ""' + $cmd + '" ' + $cmdArgs + '"'
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    if ($null -ne $stdin) { $psi.RedirectStandardInput = $true }
    $psi.WorkingDirectory = $Work
    foreach ($key in @($psi.EnvironmentVariables.Keys)) { if ($key -like 'PGPU_*' -or $key -in 'ADB', 'ANDROID_SERIAL', 'FAKE_ADB_STATE', 'FAKE_ADB_LOG', 'FAKE_NETSH_OUT') { $psi.EnvironmentVariables.Remove($key) } }
    $base = @{ ADB = $k.Adb; FAKE_ADB_STATE = $k.State; FAKE_ADB_LOG = (Join-Path (Split-Path -Parent $k.Kit) 'adb-calls.txt'); PGPU_NOPAUSE = '1'; PGPU_LANG = 'en'
               PGPU_PORT = $k.Port; PGPU_CAM_PORT = $k.CamPort; PGPU_AUDIO_PORT = $k.AudioPort }
    foreach ($key in $base.Keys) { $psi.EnvironmentVariables[$key] = [string]$base[$key] }
    foreach ($key in $envs.Keys) { if ($null -eq $envs[$key]) { [void]$psi.EnvironmentVariables.Remove($key) } else { $psi.EnvironmentVariables[$key] = [string]$envs[$key] } }
    $p = [Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync(); $e = $p.StandardError.ReadToEndAsync()
    if ($null -ne $stdin) { $p.StandardInput.Write($stdin); $p.StandardInput.Close() }
    if (-not $p.WaitForExit(60000)) { try { $p.Kill() } catch { }; return @{ Exit = -99; Out = $o.Result; Err = 'TIMEOUT'; Calls = '' } }
    [void]$p.WaitForExit()
    foreach ($id in (Get-FwdPids $k.State)) { if (-not $script:Spawned.Contains($id)) { $script:Spawned.Add($id) } }
    $calls = $(if (Test-Path $base.FAKE_ADB_LOG) { [IO.File]::ReadAllText($base.FAKE_ADB_LOG) } else { '' })
    return @{ Exit = $p.ExitCode; Out = $o.Result + $e.Result; Calls = $calls }
}
function Invoke-AppCmd($k, [string]$cmdArgs = '', [hashtable]$envs = @{}) { return (Invoke-KitCmd $k ('android' + [char]92 + 'android_app.cmd') $cmdArgs $envs) }
function Scn-AndroidAppCmd([string]$mode) {
    if ($mode -ne 'fix') { return }
    $idle = @{ serial = 'EMU1'; dstate = 'device'; pids = ''; hash = ''; file = '0'; next = '7001' }
    # 1) アプリ 0.1.2：新しい向け方
    $s = $idle.Clone(); $s.app = '0.1.2'
    $k = New-AndroidKit 'appcmd-new' $s $false
    $r = Invoke-AppCmd $k
    $m = Get-FwdMap $k.State
    Check 'android_app.cmd・アプリ 0.1.2：localabstract の向け方でつなぎ、PC の口でアプリが応答する' (($r.Exit -eq 0) -and ($m[[string]$k.CamPort] -eq $script:AppNew.Cam) -and ($m[[string]$k.AudioPort] -eq $script:AppNew.Aud) -and ((Test-AppAnswer $k.CamPort) -eq 'webcam') -and ((Test-AppAnswer $k.AudioPort) -eq 'audio')) 'safety'
    Check 'android_app.cmd・アプリ 0.1.2：GPU の口は、キットの受け手が無いので、アプリの受け手（tcp:50060）へ' (($m[[string]$k.Port] -eq 'tcp:50060') -and (Test-Hello $k.Port)) 'safety'
    Check 'android_app.cmd・アプリ 0.1.2：古い向け方の forward は 1 回も打たない' ($r.Calls -notmatch 'forward tcp:\d+ tcp:5006[34]') 'safety'
    $r2 = Invoke-AppCmd $k 'stop'
    Check 'android_app.cmd stop：3 つの adb forward が外れる' (($r2.Exit -eq 0) -and ((Get-FwdMap $k.State).Count -eq 0)) 'safety'
    Stop-Spawned
    # 2) アプリ 0.1.1：古い向け方。行末が CR CR LF の古い adb でも版を読める
    $s = $idle.Clone(); $s.app = '0.1.1'; $s.crlf = '1'
    $k = New-AndroidKit 'appcmd-old' $s $false
    $r = Invoke-AppCmd $k
    $m = Get-FwdMap $k.State
    Check 'android_app.cmd・アプリ 0.1.1：tcp:50064・tcp:50063 の向け方でつなぎ、PC の口でアプリが応答する（行末 CR CR LF の adb でも）' (($r.Exit -eq 0) -and ($m[[string]$k.CamPort] -eq $script:AppOld.Cam) -and ($m[[string]$k.AudioPort] -eq $script:AppOld.Aud) -and ((Test-AppAnswer $k.CamPort) -eq 'webcam') -and ((Test-AppAnswer $k.AudioPort) -eq 'audio')) 'safety'
    Check 'android_app.cmd・アプリ 0.1.1：古いアプリだと知らせ、更新のしかたを出す' ($r.Out -match 'The app on the phone is old') 'safety'
    Stop-Spawned
    # 3) 版の読めない形（0.1.10・0.2.0・1.0・0.1.2-debug は新、0.0.9・0.1 は古い）。GPU は 0.1.4 以上だけ USB 用の口
    foreach ($v in @(@('0.1.10', 'new', 'usb'), @('0.2.0', 'new', 'usb'), @('1.0', 'new', 'usb'), @('0.1.2-debug', 'new', 'tcp'), @('0.1.4-debug', 'new', 'usb'), @('0.0.9', 'old', 'tcp'), @('0.1', 'old', 'tcp'))) {
        $s = $idle.Clone(); $s.app = $v[0]
        $k = New-AndroidKit ('appcmd-ver-' + $v[0]) $s $false
        $r = Invoke-AppCmd $k
        $m = Get-FwdMap $k.State
        $want = $(if ($v[1] -eq 'new') { $script:AppNew.Cam } else { $script:AppOld.Cam })
        $wantGpu = $(if ($v[2] -eq 'usb') { $script:AppGpuUsb } else { 'tcp:50060' })
        Check ('android_app.cmd・アプリ ' + $v[0] + ' は ' + $v[1] + ' の向け方・GPU は ' + $wantGpu + ' を選ぶ') (($r.Exit -eq 0) -and ($m[[string]$k.CamPort] -eq $want) -and ($m[[string]$k.Port] -eq $wantGpu)) 'safety'
        Stop-Spawned
    }
    # 3b) アプリ 0.1.4：GPU の口は USB 用の口。端末の 50060 が使えず、受け手が 50061 で待っていても届く（10/14 の Redmagic 9S Pro の形）
    $s = $idle.Clone(); $s.app = '0.1.4'; $s.appport = '50061'
    $k = New-AndroidKit 'appcmd-014' $s $false
    $r = Invoke-AppCmd $k
    $m = Get-FwdMap $k.State
    Check 'android_app.cmd・アプリ 0.1.4：GPU の口を localabstract:pocketgpu-gpu へ向け、端末の 50060 が使えなくてもアプリの受け手が応答する' (($r.Exit -eq 0) -and ($m[[string]$k.Port] -eq $script:AppGpuUsb) -and (Test-Hello $k.Port) -and ($r.Out -match 'localabstract:pocketgpu-gpu')) 'safety'
    Check 'android_app.cmd・アプリ 0.1.4：GPU の tcp:50060 への forward は 1 回も打たない・更新の案内は出さない' (($r.Calls -notmatch ('forward tcp:' + $k.Port + ' tcp:50060')) -and ($r.Out -notmatch 'older than 0.1.4')) 'safety'
    Stop-Spawned
    # 3c) アプリ 0.1.3：GPU の口は tcp:50060。更新の案内を出す
    $s = $idle.Clone(); $s.app = '0.1.3'
    $k = New-AndroidKit 'appcmd-013' $s $false
    $r = Invoke-AppCmd $k
    $m = Get-FwdMap $k.State
    Check 'android_app.cmd・アプリ 0.1.3：GPU の口は tcp:50060・応答する・0.1.4 への更新の案内を出す' (($r.Exit -eq 0) -and ($m[[string]$k.Port] -eq 'tcp:50060') -and (Test-Hello $k.Port) -and ($r.Out -match 'older than 0.1.4')) 'safety'
    Check 'android_app.cmd・アプリ 0.1.3：USB 用の GPU の口は打たない' ($r.Calls -notmatch 'pocketgpu-gpu') 'safety'
    Stop-Spawned
    # 3d) アプリ 0.1.4 で、adb が localabstract を断る（古い adb）：GPU は tcp:50060 に切り替える
    $s = $idle.Clone(); $s.app = '0.1.4'; $s.refuse_abstract = '1'
    $k = New-AndroidKit 'appcmd-014-refuse' $s $false
    $r = Invoke-AppCmd $k
    $m = Get-FwdMap $k.State
    Check 'android_app.cmd・アプリ 0.1.4 で adb が localabstract を断ったら、GPU は tcp:50060 にして応答する' (($r.Exit -eq 0) -and ($m[[string]$k.Port] -eq 'tcp:50060') -and (Test-Hello $k.Port) -and ($r.Calls -match 'pocketgpu-gpu')) 'safety'
    Stop-Spawned
    # 3e) PGPU_APP_GPU=tcp：0.1.4 でも tcp:50060。PGPU_APP_GPU=usb で断られたら、切り替えずに失敗する
    $s = $idle.Clone(); $s.app = '0.1.4'
    $k = New-AndroidKit 'appcmd-014-forced-tcp' $s $false
    $r = Invoke-AppCmd $k '' @{ PGPU_APP_GPU = 'tcp' }
    Check 'android_app.cmd・PGPU_APP_GPU=tcp：アプリ 0.1.4 でも GPU は tcp:50060' (($r.Exit -eq 0) -and ((Get-FwdMap $k.State)[[string]$k.Port] -eq 'tcp:50060') -and (Test-Hello $k.Port)) 'safety'
    Stop-Spawned
    $s = $idle.Clone(); $s.app = '0.1.4'; $s.refuse_abstract = '1'
    $k = New-AndroidKit 'appcmd-014-forced-usb' $s $false
    $r = Invoke-AppCmd $k '' @{ PGPU_APP_GPU = 'usb'; PGPU_APP_FORWARD = 'old' }
    Check 'android_app.cmd・PGPU_APP_GPU=usb で断られたら、tcp:50060 に切り替えずに失敗する' (($r.Exit -eq 1) -and (-not (Get-FwdMap $k.State).ContainsKey([string]$k.Port))) 'safety'
    Stop-Spawned
    # 3f) アプリ 0.1.4 でも、キットの受け手が動いていれば、GPU の口はキットの受け手へ
    $s = $idle.Clone(); $s.app = '0.1.4'; $s.pids = '1111'; $s.file = '1'; $s.hash = 'ab' * 32
    $k = New-AndroidKit 'appcmd-014-kitrecv' $s $false
    $r = Invoke-AppCmd $k
    $m = Get-FwdMap $k.State
    Check 'android_app.cmd・アプリ 0.1.4 でもキットの受け手が動いていれば、GPU の口は tcp:口（キットの受け手）' (($m[[string]$k.Port] -eq ('tcp:' + $k.Port)) -and (Test-Hello $k.Port) -and ((Get-AdbState $k.State).apprecv -eq '0')) 'safety'
    Stop-Spawned
    # 4) 版が読めない（dumpsys が何も返さない）：新しい向け方。adb が新しい向け方を断ったら、古い向け方
    $s = $idle.Clone(); $s.app = 'dev'
    $k = New-AndroidKit 'appcmd-unknown' $s $false
    $r = Invoke-AppCmd $k
    Check 'android_app.cmd・版が読めない：新しい向け方を先に使う' (((Get-FwdMap $k.State)[[string]$k.CamPort] -eq $script:AppNew.Cam)) 'safety'
    Stop-Spawned
    $s = $idle.Clone(); $s.app = 'dev'; $s.refuse_abstract = '1'
    $k = New-AndroidKit 'appcmd-refuse' $s $false
    $r = Invoke-AppCmd $k
    $m = Get-FwdMap $k.State
    Check 'android_app.cmd・adb が新しい向け方を断ったら、古い向け方に切り替える' (($r.Exit -eq 0) -and ($m[[string]$k.CamPort] -eq $script:AppOld.Cam) -and ($m[[string]$k.AudioPort] -eq $script:AppOld.Aud) -and ($r.Calls -match 'localabstract')) 'safety'
    Stop-Spawned
    $s = $idle.Clone(); $s.app = '0.1.2'; $s.refuse_abstract = '1'
    $k = New-AndroidKit 'appcmd-refuse-forced' $s $false
    $r = Invoke-AppCmd $k '' @{ PGPU_APP_FORWARD = 'new' }
    Check 'android_app.cmd・PGPU_APP_FORWARD=new で断られたら、古い向け方に切り替えずに失敗する' (($r.Exit -eq 1) -and ((Get-FwdMap $k.State).Count -eq 0)) 'safety'
    Stop-Spawned
    # 5) PGPU_APP_FORWARD=old で、0.1.2 のアプリにも古い向け方（設定で古いツールを受け付けるとき）
    $s = $idle.Clone(); $s.app = '0.1.2'; $s.legacy = '1'
    $k = New-AndroidKit 'appcmd-forced-old' $s $false
    $r = Invoke-AppCmd $k '' @{ PGPU_APP_FORWARD = 'old' }
    Check 'android_app.cmd・PGPU_APP_FORWARD=old：古い向け方で、設定がオンの 0.1.2 のアプリが応答する' (((Get-FwdMap $k.State)[[string]$k.CamPort] -eq $script:AppOld.Cam) -and ((Test-AppAnswer $k.CamPort) -eq 'webcam')) 'safety'
    Stop-Spawned
    # 6) キットの受け手が動いていれば、GPU の口はそちらへ
    $s = $idle.Clone(); $s.app = '0.1.2'; $s.pids = '1111'; $s.file = '1'; $s.hash = 'ab' * 32
    $k = New-AndroidKit 'appcmd-kitrecv' $s $false
    $r = Invoke-AppCmd $k
    $m = Get-FwdMap $k.State
    Check 'android_app.cmd・キットの受け手が動いているときは、GPU の口をキットの受け手（tcp:口）へ向け、アプリの受け手は起こされない' (($m[[string]$k.Port] -eq ('tcp:' + $k.Port)) -and (Test-Hello $k.Port) -and ((Get-AdbState $k.State).apprecv -eq '0')) 'safety'
    Stop-Spawned
    # 7) アプリが入っていない
    $s = $idle.Clone()
    $k = New-AndroidKit 'appcmd-noapp' $s $false
    $r = Invoke-AppCmd $k
    Check 'android_app.cmd・アプリが入っていなければ、入れ方を出して終わりのコード 1・adb forward は作らない' (($r.Exit -eq 1) -and ($r.Out -match 'not installed') -and ((Get-FwdMap $k.State).Count -eq 0)) 'safety'
    # 8) 端末が無い stop：口は閉じる
    $s = $idle.Clone(); $s.serial = ''
    $k = New-AndroidKit 'appcmd-stop-nodev' $s $true @(@{ Port = '@cam'; Target = $script:AppNew.Cam })
    $r = Invoke-AppCmd $k 'stop'
    Check 'android_app.cmd stop・端末が無くても PC の口は閉じる（終わりのコードは 1）' (($r.Exit -eq 1) -and ((Get-FwdMap $k.State).Count -eq 0)) 'safety'
}

# =====================================================================
# PC 側の口が Windows に予約されているとき（Hyper-V・WSL などの「除外されたポートの範囲」）
# 本物の netsh の代わりに、予約の範囲を本物と同じ形で出す偽の netsh（tests\fakes\fake_netsh.cmd）を使う。予約の設定は読むだけ
# =====================================================================
# ranges：'開始-終了' の並び。空なら予約なし
function New-FakeNetsh([string]$dir, [string[]]$ranges) {
    $f = Join-Path $dir ('netsh-' + [guid]::NewGuid().ToString('N').Substring(0, 6) + '.txt')
    $l = New-Object System.Collections.Generic.List[string]
    $l.Add(''); $l.Add('Protocol tcp Port Exclusion Ranges'); $l.Add(''); $l.Add('Start Port    End Port'); $l.Add('----------    --------')
    foreach ($r in $ranges) { $a = $r -split '-'; $l.Add(('{0,10}    {1,8}' -f [int]$a[0], [int]$a[1])) }
    $l.Add(''); $l.Add('* - Administered port exclusions.')
    [IO.File]::WriteAllLines($f, $l.ToArray(), [Text.Encoding]::ASCII)
    return @{ PGPU_TEST_NETSH = (Join-Path $PSScriptRoot 'fakes\fake_netsh.cmd'); FAKE_NETSH_OUT = $f }
}
function Merge-Env([hashtable]$a, [hashtable]$b) { $m = @{}; foreach ($x in $a.Keys) { $m[$x] = $a[$x] }; foreach ($x in $b.Keys) { $m[$x] = $b[$x] }; return $m }
# android\port.txt の gpu= cam= audio= の値。無ければ $null
function Get-PortFileValue([string]$kit, [string]$key) {
    $f = Join-Path $kit 'android\port.txt'
    if (-not (Test-Path -LiteralPath $f)) { return $null }
    foreach ($l in [IO.File]::ReadAllLines($f)) { if ($l -match ('^' + $key + '=(\d+)\s*$')) { return [int]$Matches[1] } }
    return $null
}
function Convert-RawUtf8([string]$out) {
    $bytes = [Text.Encoding]::GetEncoding(28591).GetBytes($out)
    try { return (New-Object Text.UTF8Encoding($false, $true)).GetString($bytes) }
    catch { return [Text.Encoding]::GetEncoding([Globalization.CultureInfo]::CurrentCulture.TextInfo.OEMCodePage).GetString($bytes) }
}

# Fix-PocketGPU：PC の口が予約の範囲に入っている -> 聞いてから、範囲の外の口を選んで android\port.txt に覚える。直したあとの確かめはその口で行う
function Scn-PortFix([string]$mode) {
    $idle = @{ serial = 'EMU1'; dstate = 'device'; pids = '1111'; hash = '@local'; file = '1'; next = '7001' }
    $k = New-AndroidKit "port-fix-$mode" $idle $false
    # GPU の口とカメラの口が、予約の範囲に入っている
    $ns = New-FakeNetsh (Split-Path -Parent $k.Kit) @(('{0}-{1}' -f ($k.Port - 3), ($k.Port + 3)), ('{0}-{1}' -f ($k.CamPort - 3), ($k.CamPort + 3)))
    Use-Mode $mode $k.Kit
    $r = Invoke-AndroidFix $k (Get-Mode $mode 'y,y') @() $ns
    foreach ($id in (Get-FwdPids $k.State)) { $script:Spawned.Add($id) }
    $new = Get-PortFileValue $k.Kit 'gpu'
    $newCam = Get-PortFileValue $k.Kit 'cam'
    $m = Get-FwdMap $k.State
    Check 'android\port.txt に、予約の範囲の外の GPU の口（47300 から順）が覚えられる' (($null -ne $new) -and ($new -ne $k.Port) -and ($new -ge 47300) -and ($new -lt 47400))
    Check 'android\port.txt に、予約の範囲の外のカメラの口（47400 から順）も覚えられる' (($null -ne $newCam) -and ($newCam -ne $k.CamPort) -and ($newCam -ge 47400) -and ($newCam -lt 47500))
    Check '覚えた口に adb forward がつながり、受け手が応答する（実際につないで確認）' (($null -ne $new) -and ($m[[string]$new] -eq ('tcp:' + $new)) -and (Test-Hello $new))
    Check '予約の範囲に入っていた口には、adb forward を作らない' (-not $m.ContainsKey([string]$k.Port)) 'safety'
    $st = Get-AdbState $k.State
    Check '受け手（スマートフォン側）には触れていない（PID・SHA がそのまま）' (($st.pids -eq '1111') -and ($st.hash -eq $k.LocalHash)) 'safety'
    if ($mode -eq 'fix') {
        Check 'まとめに「直った」と、別の口にしたことが出る・終わりのコード 0' (($r.Summary -match '\[直った\] Android') -and ($r.Summary -match [regex]::Escape([string]$k.Port + ' -> ' + $new)) -and $r.Exit -eq 0) 'safety'
        Check '予約の範囲に入っていることと、予約の設定は変えないことを案内する' (($r.Log -match 'Windows の予約の範囲') -and ($r.Log -match 'netsh int ipv4 show excludedportrange') -and ($r.Log -match '予約の設定は、このツールでは変えません')) 'safety'
        # 予約されていなければ、何も聞かず、port.txt も作らない
        $k2 = New-AndroidKit 'port-fix-none' $idle $true
        $ns2 = New-FakeNetsh (Split-Path -Parent $k2.Kit) @()
        $r2 = Invoke-AndroidFix $k2 'n' @() $ns2
        Check '予約されていなければ、問題なし・何も聞かない・android\port.txt も作らない' (($r2.Exit -eq 0) -and ($r2.Summary -match '問題なし 1') -and ($r2.Log -notmatch '予約') -and ($null -eq (Get-PortFileValue $k2.Kit 'gpu'))) 'safety'
    } else {
        Check '直していないので、終わりのコード 1・まとめは「直らなかった」' (($r.Exit -eq 1) -and ($r.Summary -match '\[直らなかった\] Android')) 'safety'
        Check '直していないので、android\port.txt は作られない' (($null -eq $new) -and ($null -eq $newCam)) 'safety'
    }
}

# 口を決める部品 tools\pgpu_port.ps1 そのもの：画面の文（日本語は UTF-8 で動かして読む）・47300 から順・使っている物を避ける・adb が使っている口は使ってよい
function Scn-PortLib([string]$mode) {
    if ($mode -ne 'fix') { return }
    $kit = New-Kit 'port-lib'
    $dir = Split-Path -Parent $kit
    $lib = Join-Path $kit 'tools\pgpu_port.ps1'
    $P = Get-FreePort
    $C = Get-FreePort
    $A = Get-FreePort
    $ns = New-FakeNetsh $dir @(('{0}-{1}' -f ($P - 1), ($P + 1)), '47300-47309')
    $e = Merge-Env @{ PGPU_PORT = $P; PGPU_CAM_PORT = $C; PGPU_AUDIO_PORT = $A } $ns
    $script:RawOut = $true
    try { $r = Invoke-Fix $kit @('"' + $lib + '"', 'gpu') $e '' (Join-Path $PSScriptRoot 'fakes\utf8-run.ps1') } finally { $script:RawOut = $false }
    $ja = Convert-RawUtf8 ([string]$r.Out)
    $new = Get-PortFileValue $kit 'gpu'
    Check '日本語の画面：「ポート X は Windows に予約されているので、Y を使います」の 1 行を出す' (($ja -match ('ポート ' + $P + ' は Windows に予約されているので、(\d+) を使います')) -and ([int]$Matches[1] -eq $new)) 'safety'
    Check '47300 から順に探し、予約の範囲（47300-47309）の外の 47310 以上を選んで、android\port.txt に覚える' (($null -ne $new) -and ($new -ge 47310) -and ($new -lt 47400))
    Check 'カメラ・マイクの口は予約されていないので、そのまま（port.txt に書かない）' (($null -eq (Get-PortFileValue $kit 'cam')) -and ($null -eq (Get-PortFileValue $kit 'audio'))) 'safety'
    # 英語の画面（PGPU_LANG=en）
    $kit2 = New-Kit 'port-lib-en'
    $ns2 = New-FakeNetsh $dir @(('{0}-{1}' -f ($P - 1), ($P + 1)))
    $r2 = Invoke-Fix $kit2 @('gpu') (Merge-Env @{ PGPU_PORT = $P; PGPU_LANG = 'en' } $ns2) '' (Join-Path $kit2 'tools\pgpu_port.ps1')
    $new2 = Get-PortFileValue $kit2 'gpu'
    Check ('英語の画面：「Port ' + $P + ' is reserved by Windows, so port ' + $new2 + ' is used instead」の 1 行を出す') ($r2.Out -match ('Port ' + $P + ' is reserved by Windows, so port ' + $new2 + ' is used instead')) 'safety'
    Check '予約の範囲の外の先頭の 47300 を選ぶ' ($new2 -eq 47300)
    # 予約されていない口は、何も出さず、そのまま。port.txt も作らない
    $kit3 = New-Kit 'port-lib-none'
    $ns3 = New-FakeNetsh $dir @()
    $r3 = Invoke-Fix $kit3 @('gpu') (Merge-Env @{ PGPU_PORT = $P; PGPU_LANG = 'en' } $ns3) '' (Join-Path $kit3 'tools\pgpu_port.ps1')
    Check '予約されていなければ、画面に何も出さず、android\port.txt も作らない' (([string]$r3.Out).Trim() -eq '' -and ($null -eq (Get-PortFileValue $kit3 'gpu'))) 'safety'
    # ほかのプログラムが使っている口は避ける（そのプログラムは止めない）
    $B = Get-FreePort
    $blocker = Start-Blocker $B
    $kit4 = New-Kit 'port-lib-inuse'
    $r4 = Invoke-Fix $kit4 @('gpu') (Merge-Env @{ PGPU_PORT = $B; PGPU_LANG = 'en' } $ns3) '' (Join-Path $kit4 'tools\pgpu_port.ps1')
    $new4 = Get-PortFileValue $kit4 'gpu'
    Check 'ほかのプログラムが使っている口は、使っていると画面に出して、別の口を選ぶ' (($r4.Out -match ('Port ' + $B + ' is used by another program, so port ' + $new4 + ' is used instead')) -and ($null -ne $new4) -and ($new4 -ne $B)) 'safety'
    Check '使っていたプログラムは止めない' (Test-Alive $blocker.Id) 'safety'
    # adb が使っている口（前に作った adb forward）は、使っているうちに入れない
    $k = New-AndroidKit 'port-lib-adb' @{ serial = 'EMU1'; dstate = 'device'; pids = '1111'; hash = '@local'; file = '1'; next = '7001' } $true
    $r5 = Invoke-Fix $k.Kit @('gpu') (Merge-Env @{ PGPU_PORT = $k.Port; PGPU_LANG = 'en' } $ns3) '' (Join-Path $k.Kit 'tools\pgpu_port.ps1')
    Check 'adb が使っている口（前に作った adb forward）は、そのまま使う。画面に何も出さず、android\port.txt も作らない' ((([string]$r5.Out).Trim() -eq '') -and ($null -eq (Get-PortFileValue $k.Kit 'gpu')) -and (Test-Hello $k.Port)) 'safety'
}

# android\ の cmd：選んだ口を android\port.txt に覚え、あとから単体で動かした cmd や run.ps1 も同じ口を使う。stop_android.cmd は覚えた口の forward を外す
# fix：本物の動き。sabotage：口を選ぶ部品 tools\pgpu_port.ps1 を外した物（効果の検査が偽になること）
function Scn-PortCmd([string]$mode) {
    if ($mode -eq 'decline') { return }
    $idle = @{ serial = 'EMU1'; dstate = 'device'; pids = ''; hash = ''; file = '0'; next = '7001'; app = '0.1.4' }
    $noEnv = @{ PGPU_PORT = $null; PGPU_CAM_PORT = $null; PGPU_AUDIO_PORT = $null }
    $S = 'android\start_android.cmd'
    # 1) 50300 が予約されている（この PC と同じ形：50167-51066）-> 別の口を選ぶ。そのあと別々に動かす cmd も、同じ口を使う
    $k = New-AndroidKit 'portcmd-reserved' $idle $false
    if ($mode -eq 'sabotage') { Remove-Item -LiteralPath (Join-Path $k.Kit 'tools\pgpu_port.ps1') }
    $ns = New-FakeNetsh (Split-Path -Parent $k.Kit) @('50167-51066')
    $r = Invoke-KitCmd $k $S '' (Merge-Env $noEnv $ns)
    $gpu = Get-PortFileValue $k.Kit 'gpu'
    $m = Get-FwdMap $k.State
    $st = Get-AdbState $k.State
    Check 'start_android.cmd：50300 が予約されているとき、「Port 50300 is reserved by Windows, so port N is used instead」の 1 行を出す' (($r.Out -match 'Port 50300 is reserved by Windows, so port (\d+) is used instead') -and ($null -ne $gpu) -and ([int]$Matches[1] -eq $gpu))
    Check 'start_android.cmd：選ぶ口は予約の範囲（50167-51066）の外で、47300 から順の 47300 台' (($null -ne $gpu) -and ($gpu -ge 47300) -and ($gpu -lt 47400))
    Check 'start_android.cmd：選んだ口で受け手を起動し（スマートフォンへ渡す口）、adb forward をつなぎ、受け手が応答する' (($null -ne $gpu) -and ($st.lport -eq [string]$gpu) -and ($m[[string]$gpu] -eq ('tcp:' + $gpu)) -and (Test-Hello $gpu) -and ($r.Exit -eq 0))
    Check 'start_android.cmd：予約された 50300 には adb forward を作らない' (-not $m.ContainsKey('50300') -and ($null -ne $gpu))
    if ($mode -eq 'fix') {
        $g = $gpu
        # pocketgpu_android.cmd：口を指定せずに単体で動かしても、覚えた口（IPHONE_VK=127.0.0.1:口）
        $r = Invoke-KitCmd $k 'pocketgpu_android.cmd' 'cmd.exe /c set' $noEnv
        Check 'pocketgpu_android.cmd：口を指定しなくても、覚えた口を IPHONE_VK に渡す' ($r.Out -match ('IPHONE_VK=127\.0\.0\.1:' + $g + '\b')) 'safety'
        # check_android.cmd：vklist.exe の代わりに、IPHONE_VK を出すだけの小さな exe を置く
        # （cmd.exe の写しに標準入力で打たせる形は、GitHub の試験機では IPHONE_VK を出さなかった）
        $envExe = Join-Path $Work 'fakeenv\printenv.exe'
        if (-not (Test-Path $envExe)) {
            [void](New-Item -ItemType Directory -Path (Split-Path -Parent $envExe) -Force)
            Add-Type -TypeDefinition 'public static class PrintEnv { public static void Main() { System.Console.WriteLine("IPHONE_VK=" + System.Environment.GetEnvironmentVariable("IPHONE_VK")); } }' -OutputAssembly $envExe -OutputType ConsoleApplication
        }
        Copy-Item $envExe (Join-Path $k.Kit 'vklist.exe') -Force
        $r = Invoke-KitCmd $k 'check_android.cmd' '' $noEnv
        Check 'check_android.cmd：口を指定しなくても、覚えた口を IPHONE_VK に渡す' ($r.Out -match ('IPHONE_VK=127\.0\.0\.1:' + $g + '\b')) 'safety'
        # tools\run.ps1（Android の形）：IPHONE_VK を覚えた口にする
        $root = Join-Path (Split-Path -Parent $k.Kit) 'games'
        $game = New-Game $root 'vk' 'x64' @('kernel32.dll', 'vulkan-1.dll')
        $rr = Invoke-RunDx $k.Kit $game (Merge-Env @{ PGPU_ANDROID = '1'; PGPU_PORT = $null } @{}) 'y,y'
        Check 'tools\run.ps1（Android）：口を指定しなくても、覚えた口を IPHONE_VK に渡す' ($rr.Out -match ('DRYRUN set IPHONE_VK=127\.0\.0\.1:' + $g + '\b')) 'safety'
        # android_app.cmd：覚えた口を使う（adb が使っている口なので選び直さない）。キットの受け手が動いているので GPU の口はそちらへ
        $r = Invoke-KitCmd $k 'android\android_app.cmd' '' (Merge-Env @{ PGPU_PORT = $null; PGPU_CAM_PORT = $k.CamPort; PGPU_AUDIO_PORT = $k.AudioPort } $ns)
        $m = Get-FwdMap $k.State
        Check 'android_app.cmd：覚えた口を使う（GPU の口はキットの受け手 tcp:口・口は変わらない・選び直しの案内は出ない）' (($r.Exit -eq 0) -and ($m[[string]$g] -eq ('tcp:' + $g)) -and ($r.Out -match ('adb forward tcp:' + $g + ' tcp:' + $g)) -and ($r.Out -notmatch 'is reserved by Windows') -and ((Get-PortFileValue $k.Kit 'gpu') -eq $g)) 'safety'
        # stop_android.cmd：このウィンドウに別の PGPU_PORT が残っていても、覚えた口の forward を外す。port.txt は残る
        $r = Invoke-KitCmd $k 'android\stop_android.cmd' '' @{ PGPU_PORT = (Get-FreePort) }
        $m = Get-FwdMap $k.State
        Check 'stop_android.cmd：覚えた口の adb forward を外す（口も閉じる）。android\port.txt は残る' (($r.Exit -eq 0) -and (-not $m.ContainsKey([string]$g)) -and (Wait-Until { -not (Test-Port $g) } 4) -and ((Get-PortFileValue $k.Kit 'gpu') -eq $g))
        Stop-Spawned
        # 2) 予約されていない -> 50300 のまま（何も出さず、port.txt も作らない）。この PC では本当に 50300 が予約されていて adb forward は作れないので、forward の成否は見ない
        $k = New-AndroidKit 'portcmd-none' $idle $false
        $ns = New-FakeNetsh (Split-Path -Parent $k.Kit) @()
        $r = Invoke-KitCmd $k $S '' (Merge-Env $noEnv $ns)
        Check '予約されていなければ、start_android.cmd は 50300 のまま（Port: 50300）・案内は出ない・android\port.txt も作らない' (($r.Out -match 'Port: 50300') -and ($r.Out -notmatch 'is reserved by Windows') -and ($r.Out -notmatch 'used by another program') -and ($null -eq (Get-PortFileValue $k.Kit 'gpu'))) 'safety'
        Stop-Spawned
        # 3) 口を指定したのに予約の範囲に入っている -> 別の口を選ぶ。選んだ口は、指定が残っていても start_android.cmd のあとの cmd（Start-Android-GPU.cmd と同じ読み方）が使える
        $k = New-AndroidKit 'portcmd-explicit' $idle $false
        $p = $k.Port
        $ns = New-FakeNetsh (Split-Path -Parent $k.Kit) @(('{0}-{1}' -f ($p - 2), ($p + 2)))
        $r = Invoke-KitCmd $k $S '' (Merge-Env @{ PGPU_PORT = $p; PGPU_CAM_PORT = $null; PGPU_AUDIO_PORT = $null } $ns)
        $gpu = Get-PortFileValue $k.Kit 'gpu'
        $m = Get-FwdMap $k.State
        Check '口を指定しても予約の範囲に入っていれば、別の口を選んで覚え、そこにつなぐ' (($r.Out -match ('Port ' + $p + ' is reserved by Windows, so port (\d+) is used instead')) -and ($null -ne $gpu) -and ($gpu -ne $p) -and ($m[[string]$gpu] -eq ('tcp:' + $gpu)) -and (-not $m.ContainsKey([string]$p)))
        Stop-Spawned
        # 4) 覚えた口が、あとから予約された -> 選び直す（47300 台も、既定の 50300 も予約されているので、47310 以上）
        $k = New-AndroidKit 'portcmd-reserved-later' $idle $false
        [IO.File]::WriteAllText((Join-Path $k.Kit 'android\port.txt'), "gpu=47300`r`n", [Text.Encoding]::ASCII)
        $ns = New-FakeNetsh (Split-Path -Parent $k.Kit) @('47300-47309', '50167-51066')
        $r = Invoke-KitCmd $k $S '' (Merge-Env $noEnv $ns)
        $gpu = Get-PortFileValue $k.Kit 'gpu'
        Check '覚えた口 47300 があとから予約されたら、選び直して覚え直す（47310 以上）' (($r.Out -match 'Port 47300 is reserved by Windows') -and ($null -ne $gpu) -and ($gpu -ge 47310) -and ($gpu -lt 47400) -and (Get-FwdMap $k.State)[[string]$gpu] -eq ('tcp:' + $gpu))
        Stop-Spawned
        # 5) 覚えた口が予約されていなければ、環境変数なしでも start_android.cmd がその口を使う
        $k = New-AndroidKit 'portcmd-remembered' $idle $false
        $F = Get-FreePort
        [IO.File]::WriteAllText((Join-Path $k.Kit 'android\port.txt'), "gpu=$F`r`n", [Text.Encoding]::ASCII)
        $ns = New-FakeNetsh (Split-Path -Parent $k.Kit) @('50167-51066')
        $r = Invoke-KitCmd $k $S '' (Merge-Env $noEnv $ns)
        Check '覚えた口が使えるなら、環境変数がなくても start_android.cmd はその口を使う（案内は出ない）' (($r.Out -match ('Port: ' + $F)) -and ($r.Out -notmatch 'is reserved') -and ((Get-FwdMap $k.State)[[string]$F] -eq ('tcp:' + $F)) -and ((Get-PortFileValue $k.Kit 'gpu') -eq $F)) 'safety'
        Stop-Spawned
    }
    Stop-Spawned
    # 6) PocketGPU アプリのカメラとマイクの口（既定 50064・50063）が予約されている -> 47400 台・47500 台。表示する webcam_rx.exe / ipaudio.exe の口もそれ
    $s2 = $idle.Clone(); $s2.app = '0.1.2'
    $k = New-AndroidKit 'portcmd-app' $s2 $false
    if ($mode -eq 'sabotage') { Remove-Item -LiteralPath (Join-Path $k.Kit 'tools\pgpu_port.ps1') }
    $ns = New-FakeNetsh (Split-Path -Parent $k.Kit) @('50060-50070')
    $r = Invoke-KitCmd $k 'android\android_app.cmd' '' (Merge-Env @{ PGPU_PORT = $k.Port; PGPU_CAM_PORT = $null; PGPU_AUDIO_PORT = $null } $ns)
    $cam = Get-PortFileValue $k.Kit 'cam'
    $aud = Get-PortFileValue $k.Kit 'audio'
    $m = Get-FwdMap $k.State
    Check 'android_app.cmd：カメラ（50064）とマイク（50063）が予約されているとき、1 行ずつ出して 47400 台・47500 台を選び、android\port.txt に覚える' (($null -ne $cam) -and ($null -ne $aud) -and ($cam -ge 47400) -and ($cam -lt 47500) -and ($aud -ge 47500) -and ($aud -lt 47600) -and ($r.Out -match ('Port 50064 \(webcam\) is reserved by Windows, so port ' + $cam + ' is used instead')) -and ($r.Out -match ('Port 50063 \(audio\) is reserved by Windows, so port ' + $aud + ' is used instead')))
    Check 'android_app.cmd：選んだ口に adb forward をつなぎ、アプリが応答し、使い方の表示もその口' (($null -ne $cam) -and ($m[[string]$cam] -eq $script:AppNew.Cam) -and ($m[[string]$aud] -eq $script:AppNew.Aud) -and ((Test-AppAnswer $cam) -eq 'webcam') -and ((Test-AppAnswer $aud) -eq 'audio') -and ($r.Out -match ('webcam_rx\.exe --addr 127\.0\.0\.1:' + $cam)) -and ($r.Out -match ('ipaudio\.exe --connect 127\.0\.0\.1:' + $aud)))
    if ($mode -eq 'fix') {
        $r = Invoke-KitCmd $k 'android\android_app.cmd' 'stop' (Merge-Env @{ PGPU_PORT = $k.Port; PGPU_CAM_PORT = $null; PGPU_AUDIO_PORT = $null } @{})
        Check 'android_app.cmd stop：覚えたカメラとマイクの口の adb forward を外す' (($r.Exit -eq 0) -and ((Get-FwdMap $k.State).Count -eq 0)) 'safety'
    }
}

# iPhone の GPU が見えない -> 原因の案内。ウイルス対策の気配
function Scn-GpuAv([string]$mode) {
    if ($mode -ne 'fix') { return }
    $kit = New-Kit 'gpu-av'
    Remove-Item (Join-Path $kit 'iphone_icd.dll')
    $r = Invoke-Fix $kit @('-Only', 'gpu') @{ PGPU_TEST_AV_EVENT = '1'; IPHONE_VK_MUX = (Get-FreePort) } 'n'
    Check 'GPU が見えないとき、隔離の気配と案内を出し、直らなかったとする' (($r.Log -match 'ウイルス対策ソフト') -and ($r.Log -match '除外の追加も') -and $r.Exit -eq 1) 'safety'
    Check '自分では何も復元しない・勝手にページを開かない' ((-not (Test-Path (Join-Path $kit 'iphone_icd.dll'))) -and ((Get-Launch $kit) -notmatch 'windowsdefender')) 'safety'
    $kit2 = New-Kit 'gpu-ok'
    $r2 = Invoke-Fix $kit2 @('-Only', 'gpu') @{ IPHONE_VK_MUX = (Get-FreePort) } 'n'
    Check '正常なキットでは、iPhone の GPU が見えて問題なし' (($r2.Log -match 'Apple A19 Pro GPU') -and $r2.Exit -eq 0) 'safety'
}

# -CheckOnly：何も変えない
function Scn-CheckOnly([string]$mode) {
    if ($mode -ne 'fix') { return }
    $kit = New-Kit 'checkonly'
    foreach ($n in 'pocketgpu.cmd', 'pocketgpu_android.cmd', 'check_android.cmd') { Make-StaleLauncher (Join-Path $kit $n) }
    Remove-Item (Join-Path $kit 'x86\iphone_icd.dll')
    [void](New-Item -ItemType Directory -Path (Join-Path $kit 'extras') -Force)
    $exe = Join-Path $kit 'extras\webcam_rx.exe'
    Copy-Item (Join-Path $env:WINDIR 'System32\ping.exe') $exe
    $p1 = Start-Hidden $exe '-t 127.0.0.1'; Start-Sleep -Milliseconds 1200; $p2 = Start-Hidden $exe '-t 127.0.0.1'
    $mux = Get-FreePort
    [void](Start-FakeMux $mux)
    Wait-Until { Test-Port $mux } 10 | Out-Null
    $before = Get-TreeHash $kit
    $r = Invoke-Fix $kit @('-CheckOnly') @{ PGPU_TEST_VK_LOADER = '1.3.204'; IPHONE_VK_MUX = $mux } 'y,y,y,y,y,y,y,y,y,y'
    Start-Sleep -Milliseconds 400
    Check '-CheckOnly は、y と答える設定でも何も変えない（ファイル一式が同じ）' ((Get-TreeHash $kit) -eq $before)
    Check '-CheckOnly は、プロセスを止めない・何も開かない' ((Test-Alive $p1.Id) -and (Test-Alive $p2.Id) -and ((Get-Launch $kit) -notmatch '(?m)^(url|app|service) ') -and -not (Test-Path (Join-Path $kit 'fix-backup')) -and -not (Test-Path (Join-Path $kit 'update-backup'))) 'safety'
    Check '-CheckOnly でも、直らない物があるので終わりのコード 1・まとめに「直らなかった」' (($r.Exit -eq 1) -and ($r.Summary -match '\[直らなかった\]')) 'safety'
}

# まとめ：個人情報・合言葉・パスを含まない
function Scn-Privacy([string]$mode) {
    if ($mode -ne 'fix') { return }
    $kit = New-Kit 'privacy'
    $port = Get-FreePort
    $token = 'SECRETTOKENabcdefghij0123456789'  # pgpu-allow-secret: 試験用の偽の値
    [IO.File]::WriteAllText((Join-Path $kit 'tools\update.ini'), "url=http://127.0.0.1:$port`r`ntoken=$token`r`n", [Text.Encoding]::ASCII)
    Make-StaleLauncher (Join-Path $kit 'pocketgpu.cmd')
    $mux = Get-FreePort
    [void](Start-FakeMux $mux)
    Wait-Until { Test-Port $mux } 10 | Out-Null
    $r = Invoke-Fix $kit @() @{ PGPU_TEST_VK_LOADER = '1.3.204'; IPHONE_VK_MUX = $mux } 'y,n,y'
    $all = $r.Out + "`n" + $r.Log + "`n" + $r.Summary
    $leak = @()
    if ($all -match [regex]::Escape($token)) { $leak += 'token' }
    if ($env:USERNAME -and $env:USERNAME.Length -ge 3 -and $r.Summary -match [regex]::Escape($env:USERNAME)) { $leak += 'username(summary)' }
    if ($env:USERNAME -and $env:USERNAME.Length -ge 3 -and $r.Log -match [regex]::Escape($env:USERNAME)) { $leak += 'username(log)' }
    if ($env:COMPUTERNAME -and ($r.Summary + $r.Log) -match [regex]::Escape($env:COMPUTERNAME)) { $leak += 'pcname' }
    if (($r.Summary + $r.Log) -match [regex]::Escape($kit)) { $leak += 'kit path' }
    if ($r.Summary -match '[A-Za-z]:\\') { $leak += 'drive path in summary' }
    Check ('まとめ・ログに、合言葉・ユーザー名・PC 名・フォルダの場所が出ていない ' + ($leak -join ',')) ($leak.Count -eq 0) 'safety'
    Check 'まとめは Discord に貼れる長さ（15 行以内）で、PC・キットの版・結果を含む' ((@($r.Summary -split "`r?`n" | Where-Object { $_ }).Count -le 15) -and $r.Summary -match 'PC: ' -and $r.Summary -match 'Kit: app 0.7.58' -and $r.Summary -match '結果：') 'safety'
    Check 'クリップボードへのコピーは、試験では本物に書かず記録だけ' ((Get-Launch $kit) -match '(?m)^clipboard') 'safety'
}
# 英語
function Scn-English([string]$mode) {
    if ($mode -ne 'fix') { return }
    $kit = New-Kit 'english'
    $mux = Get-FreePort
    [void](Start-FakeMux $mux)
    Wait-Until { Test-Port $mux } 10 | Out-Null
    $r = Invoke-Fix $kit @('-CheckOnly') @{ PGPU_LANG = 'en'; IPHONE_VK_MUX = $mux } 'n'
    Check '英語の画面（PGPU_LANG=en）' (($r.Out -match 'Fix PocketGPU') -and ($r.Summary -match 'Result: fixed'))  'safety'
    Check '何も壊れていないキットでは、全部「問題なし」か「対象外」で終わりのコード 0' (($r.Exit -eq 0) -and ($r.Summary -notmatch '\[not fixed\]')) 'safety'
}
# 何も壊れていないキット（誤検出がない）
function Scn-Pristine([string]$mode) {
    if ($mode -ne 'fix') { return }
    $k = New-AndroidKit 'pristine-android' @{ serial = 'EMU1'; dstate = 'device'; pids = '1111'; hash = '@local'; file = '1'; next = '7001' } $true
    $mux = Get-FreePort
    [void](Start-FakeMux $mux)
    Wait-Until { Test-Port $mux } 10 | Out-Null
    $before = Get-TreeHash $k.Kit
    $r = Invoke-Fix $k.Kit @() @{ IPHONE_VK_MUX = $mux; ADB = $k.Adb; FAKE_ADB_STATE = $k.State; PGPU_PORT = $k.Port; PGPU_CAM_PORT = $k.CamPort; PGPU_AUDIO_PORT = $k.AudioPort } 'n,n,n,n'
    Check '正常なキット・正常な環境では、8 項目すべてが「問題なし」（誤検出なし）・何も変わらない' (($r.Exit -eq 0) -and ((Get-TreeHash $k.Kit) -eq $before) -and ($r.Summary -match '直った 0 / 直らなかった 0 / 気をつけたい 0') -and ((Get-Launch $k.Kit) -notmatch '(?m)^(url|app|service) ')) 'safety'
}
# 管理者権限で動いているふり -> 直せないので案内だけ（iPhone の GPU の確認は対象外）
function Scn-Elevated([string]$mode) {
    if ($mode -ne 'fix') { return }
    $kit = New-Kit 'elevated'
    $r = Invoke-Fix $kit @('-Only', 'pc,gpu') @{ PGPU_TEST_ELEVATED = '1' } 'n'
    Check '管理者権限のときは、開き直す案内を出して「直らなかった」・GPU の確認は対象外・終わりのコード 1' (($r.Log -match '管理者として実行') -and ($r.Summary -match '\[直らなかった\] この PC') -and ($r.Exit -eq 1)) 'safety'
}
# キットの外に置いた fix.ps1 から -KitDir で直す
function Scn-KitDir([string]$mode) {
    if ($mode -ne 'fix') { return }
    $kit = New-Kit 'kitdir'
    $tool = Join-Path $Work 'kitdir\outside'
    Copy-Item -Path (Initialize-Pristine) -Destination $tool -Recurse -Force
    foreach ($n in 'iphone_icd.dll', 'iphone_icd.json', 'vklist.exe', 'tools\kit.ini') { Remove-Item (Join-Path $tool $n) }
    Remove-Item (Join-Path $tool 'x86') -Recurse -Force
    Make-StaleLauncher (Join-Path $kit 'pocketgpu.cmd')
    $r = Invoke-Fix $kit @('-Only', 'loader', '-KitDir', ('"' + $kit + '"')) @{ PGPU_TEST_VK_LOADER = '1.3.204'; IPHONE_VK_MUX = (Get-FreePort) } 'y,n,n,n' (Join-Path $tool 'tools\fix.ps1')
    Check '-KitDir で指定したキットの入口が直る（fix.ps1 はキットの外・新しい入口は fix.ps1 自身の横から）' ([bool](Get-LauncherEnvValue $kit 'pocketgpu.cmd'))
    $r2 = Invoke-Fix $kit @('-Only', 'loader') @{ IPHONE_VK_MUX = (Get-FreePort); PGPU_ANSWERS = 'n' } 'n' (Join-Path $tool 'tools\fix.ps1')
    Check 'キットの外で -KitDir 無し・近くに無ければ、終わりのコード 2' ($r2.Exit -eq 2) 'safety'
}

# =====================================================================
# 「iPhone の GPU で起動」（tools\run.ps1）が、DirectX 9・10・11 を見分けて DXVK を置くこと
# =====================================================================
# 試験用の exe：取り込む DLL の名前だけを持つ最小の PE を書く。Machine は 0x14c（32 ビット）か 0x8664（64 ビット）。中身は動かさない
function New-FakePe([string]$path, [string]$arch, [string[]]$imports) {
    $x64 = $arch -eq 'x64'
    $ms = New-Object IO.MemoryStream
    $bw = New-Object IO.BinaryWriter($ms)
    $bw.Write([uint16]0x5A4D); $bw.Write((New-Object byte[] 58)); $bw.Write([int32]0x40)    # DOS 見出し（e_lfanew = 0x40）
    $bw.Write([uint32]0x4550)                                                           # PE
    $optSize = $(if ($x64) { 240 } else { 224 })
    $bw.Write([uint16]$(if ($x64) { 0x8664 } else { 0x14c })); $bw.Write([uint16]1)     # Machine・セクション 1 つ
    $bw.Write([uint32]0); $bw.Write([uint32]0); $bw.Write([uint32]0)
    $bw.Write([uint16]$optSize); $bw.Write([uint16]0x22)
    $opt = New-Object byte[] $optSize
    [BitConverter]::GetBytes([uint16]$(if ($x64) { 0x20b } else { 0x10b })).CopyTo($opt, 0)
    $dd = $(if ($x64) { 112 } else { 96 })
    [BitConverter]::GetBytes([uint32]0x1000).CopyTo($opt, $dd + 8)                      # 取り込み表の RVA
    [BitConverter]::GetBytes([uint32](20 * ($imports.Count + 1))).CopyTo($opt, $dd + 12)
    $bw.Write($opt)
    $tab = 20 * ($imports.Count + 1)
    $names = New-Object IO.MemoryStream
    $rvas = @()
    foreach ($n in $imports) { $rvas += (0x1000 + $tab + $names.Length); $b = [Text.Encoding]::ASCII.GetBytes($n + [char]0); $names.Write($b, 0, $b.Length) }
    $body = New-Object byte[] ($tab + $names.Length)
    for ($i = 0; $i -lt $imports.Count; $i++) { [BitConverter]::GetBytes([uint32]$rvas[$i]).CopyTo($body, 20 * $i + 12) }
    $names.ToArray().CopyTo($body, $tab)
    $raw = [int]([Math]::Ceiling($body.Length / 512.0) * 512)
    $sec = New-Object byte[] 8; [Text.Encoding]::ASCII.GetBytes('.idata').CopyTo($sec, 0)
    $bw.Write($sec); $bw.Write([uint32]$body.Length); $bw.Write([uint32]0x1000); $bw.Write([uint32]$raw); $bw.Write([uint32]0x200)
    $bw.Write((New-Object byte[] 12)); $bw.Write([uint32]0x40000040)
    while ($ms.Length -lt 0x200) { $bw.Write([byte]0) }
    $bw.Write($body)
    while (($ms.Length % 512) -ne 0) { $bw.Write([byte]0) }
    [IO.File]::WriteAllBytes($path, $ms.ToArray())
}
# キットの dxvk\x64・x32 に、ファイルごとに中身が違う偽の DLL を置く（どの物がゲームの横に来たかを中身で見分けるため）
function Add-FakeDxvk([string]$kit) {
    foreach ($a in 'x64', 'x32') {
        $d = Join-Path $kit ('dxvk\' + $a)
        [void](New-Item -ItemType Directory -Path $d -Force)
        foreach ($n in 'd3d9.dll', 'd3d10core.dll', 'd3d11.dll', 'dxgi.dll') {
            [IO.File]::WriteAllBytes((Join-Path $d $n), [Text.Encoding]::ASCII.GetBytes('FAKE-DXVK-' + $a + '-' + $n))
        }
    }
}
# 配置の動作だけを何もしない物に差し替えた run.ps1（対照）
function Make-SabotagedRun([string]$kit) {
    $p = Join-Path $kit 'tools\run.ps1'
    $t = [IO.File]::ReadAllText($p, [Text.Encoding]::UTF8)
    $m = 'foreach ($n in $absent) { Copy-Item -LiteralPath (Join-Path $dxvkSrc $n) -Destination $dir }'
    if (-not $t.Contains($m)) { throw 'run.ps1 copy line not found' }
    [IO.File]::WriteAllText($p, $t.Replace($m, 'foreach ($n in $absent) { }'), (New-Object Text.UTF8Encoding($true)))
}
# ゲームのフォルダを作って exe を置く。置かれた DLL の名前と、その中身が kit のどの物かを返す
function New-Game([string]$root, [string]$name, [string]$arch, [string[]]$imports) {
    $d = Join-Path $root $name
    [void](New-Item -ItemType Directory -Path $d -Force)
    $exe = Join-Path $d 'game.exe'
    New-FakePe $exe $arch $imports
    return @{ Dir = $d; Exe = $exe }
}
function Get-Placed($g) {
    $o = @{}
    foreach ($n in 'd3d9.dll', 'd3d10.dll', 'd3d10_1.dll', 'd3d10core.dll', 'd3d11.dll', 'dxgi.dll') {
        $f = Join-Path $g.Dir $n
        if (Test-Path -LiteralPath $f) { $o[$n] = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($f)) }
    }
    return $o
}
function Test-Placed($g, [string]$arch, [string[]]$want) {
    $p = Get-Placed $g
    if ($p.Count -ne $want.Count) { return $false }
    foreach ($n in $want) { if ($p[$n] -ne ('FAKE-DXVK-' + $arch + '-' + $n)) { return $false } }
    return $true
}
function Invoke-RunDx($kit, $g, [hashtable]$envs, [string]$answers) {
    $e = @{ PGPU_DRYRUN = '1' }
    foreach ($k in $envs.Keys) { $e[$k] = $envs[$k] }
    $script:RawOut = $true
    # run.ps1 は画面の文を Windows の既定の文字コードで出すので、英語の Windows（GitHub の試験機）では日本語が化ける。試験では UTF-8 にしてから動かす
    $run = Join-Path $kit 'tools' | Join-Path -ChildPath 'run.ps1'
    try { $r = Invoke-Fix $kit @('"' + $run + '"', '"' + $g.Exe + '"') $e $answers (Join-Path $PSScriptRoot 'fakes\utf8-run.ps1') } finally { $script:RawOut = $false }
    $bytes = [Text.Encoding]::GetEncoding(28591).GetBytes([string]$r.Out)
    try { $r.Out = (New-Object Text.UTF8Encoding($false, $true)).GetString($bytes) }
    catch { $r.Out = [Text.Encoding]::GetEncoding([Globalization.CultureInfo]::CurrentCulture.TextInfo.OEMCodePage).GetString($bytes) }
    return $r
}
function Scn-RunDx([string]$mode) {
    $kit = New-Kit "rundx-$mode"
    Add-FakeDxvk $kit
    if ($mode -eq 'sabotage') { Make-SabotagedRun $kit }
    $root = Join-Path (Split-Path -Parent $kit) 'games'
    $yes = $(if ($mode -eq 'decline') { 'n' } else { 'y' })
    $mux = Get-FreePort
    [void](Start-FakeMux $mux)
    $ip = @{ IPHONE_VK_MUX = $mux }

    # DirectX 9（32 ビット。Need for Speed Carbon と同じ形）-> dxvk\x32 の d3d9.dll だけ
    $g = New-Game $root 'dx9-32' 'x32' @('kernel32.dll', 'user32.dll', 'd3d9.dll')
    $r = Invoke-RunDx $kit $g $ip $yes
    Check 'DirectX 9（32 ビット）のゲームに、dxvk\x32 の d3d9.dll が置かれる' (Test-Placed $g 'x32' @('d3d9.dll'))
    Check 'DirectX 9 のゲームの横に、d3d9.dll 以外（d3d11.dll・dxgi.dll など）は置かない' (@((Get-Placed $g).Keys | Where-Object { $_ -ne 'd3d9.dll' }).Count -eq 0) 'safety'
    Check 'DirectX 9 と見分けて、置く前に y/N で聞く' (($r.Out -match 'DirectX 9 のゲームのようです') -and ($r.Out -match 'DXVK を配置しますか') -and ($r.Out -match '元に戻すときは、そのフォルダの d3d9\.dll を削除') -and ($r.Out -notmatch 'まだ DirectX 9') ) 'safety'
    Check 'iPhone の形で、起動の行（DRYRUN）まで進む' ($r.Out -match 'DRYRUN start') 'safety'

    # DirectX 9（64 ビット）-> dxvk\x64
    $g = New-Game $root 'dx9-64' 'x64' @('kernel32.dll', 'd3d9.dll')
    [void](Invoke-RunDx $kit $g $ip $yes)
    Check 'DirectX 9（64 ビット）のゲームに、dxvk\x64 の d3d9.dll が置かれる' (Test-Placed $g 'x64' @('d3d9.dll'))

    # DirectX 10（d3d10.dll を取り込む）-> d3d10core.dll・d3d11.dll・dxgi.dll。d3d10.dll は置かない
    $g = New-Game $root 'dx10' 'x64' @('kernel32.dll', 'd3d10.dll', 'dxgi.dll')
    $r = Invoke-RunDx $kit $g $ip $yes
    Check 'DirectX 10 のゲームに、dxvk\x64 の d3d10core.dll・d3d11.dll・dxgi.dll の 3 つが置かれる' (Test-Placed $g 'x64' @('d3d10core.dll', 'd3d11.dll', 'dxgi.dll'))
    Check 'DirectX 10 のゲームの横に、d3d9.dll・d3d10.dll・d3d10_1.dll は置かない' (@((Get-Placed $g).Keys | Where-Object { $_ -in 'd3d9.dll', 'd3d10.dll', 'd3d10_1.dll' }).Count -eq 0) 'safety'
    Check 'DirectX 10 と見分けて、3 つのファイルを置くと案内する' (($r.Out -match 'DirectX 10 のゲームのようです') -and ($r.Out -match 'DXVK の 3 つのファイル') -and ($r.Out -match 'd3d10core\.dll、d3d11\.dll と dxgi\.dll を削除')) 'safety'
    foreach ($imp in 'd3d10_1.dll', 'd3d10core.dll') {
        $g = New-Game $root ('dx10-' + $imp) 'x32' @('kernel32.dll', $imp)
        [void](Invoke-RunDx $kit $g $ip $yes)
        Check ($imp + ' を取り込むゲームも DirectX 10 と見分けて、dxvk\x32 の 3 つが置かれる') (Test-Placed $g 'x32' @('d3d10core.dll', 'd3d11.dll', 'dxgi.dll'))
    }

    # DirectX 11（今までどおり 2 つだけ）
    $g = New-Game $root 'dx11' 'x64' @('kernel32.dll', 'd3d11.dll')
    $r = Invoke-RunDx $kit $g $ip $yes
    Check 'DirectX 11 のゲームに、d3d11.dll と dxgi.dll の 2 つだけが置かれる（今までと同じ）' (Test-Placed $g 'x64' @('d3d11.dll', 'dxgi.dll'))
    Check 'DirectX 11 の案内は「2 つのファイル」と「d3d11.dll と dxgi.dll を削除」' (($r.Out -match 'DirectX 11 のゲームのようです') -and ($r.Out -match 'DXVK の 2 つのファイル') -and ($r.Out -match 'd3d11\.dll と dxgi\.dll を削除')) 'safety'

    # DirectX 9 と DirectX 11 の両方を取り込む -> 別々に聞いて、両方置く（答えは 2 回）
    $g = New-Game $root 'dx9-11' 'x64' @('kernel32.dll', 'd3d9.dll', 'd3d11.dll')
    [void](Invoke-RunDx $kit $g $ip ($yes + ',' + $yes))
    Check 'DirectX 9 と 11 の両方を取り込むゲームには、d3d9.dll・d3d11.dll・dxgi.dll が置かれる' (Test-Placed $g 'x64' @('d3d9.dll', 'd3d11.dll', 'dxgi.dll'))

    # 同じ名前の別の dll（ReShade など）は上書きしない。聞きもしない
    $g = New-Game $root 'dx9-other' 'x32' @('kernel32.dll', 'd3d9.dll')
    [IO.File]::WriteAllBytes((Join-Path $g.Dir 'd3d9.dll'), [Text.Encoding]::ASCII.GetBytes('SOMEONE-ELSES-D3D9'))
    $r = Invoke-RunDx $kit $g $ip 'y'
    $p = Get-Placed $g
    Check '別の d3d9.dll があるときは上書きしない（中身がそのまま）' (($p.Count -eq 1) -and ($p['d3d9.dll'] -eq 'SOMEONE-ELSES-D3D9')) 'safety'
    Check '別の d3d9.dll があるときは、上書きしない理由を出して y/N は聞かない' (($r.Out -match '別の d3d9\.dll があります。上書きしない') -and ($r.Out -notmatch 'DXVK を配置しますか')) 'safety'
    $g = New-Game $root 'dx10-other' 'x64' @('kernel32.dll', 'd3d10.dll')
    [IO.File]::WriteAllBytes((Join-Path $g.Dir 'dxgi.dll'), [Text.Encoding]::ASCII.GetBytes('SOMEONE-ELSES-DXGI'))
    $r = Invoke-RunDx $kit $g $ip 'y'
    $p = Get-Placed $g
    Check '別の dxgi.dll があるときは、DirectX 10 の 3 つのどれも置かない・上書きしない' (($p.Count -eq 1) -and ($p['dxgi.dll'] -eq 'SOMEONE-ELSES-DXGI')) 'safety'
    # DirectX 9 と 11 で、d3d9.dll だけ別の物 -> 11 の分は置ける
    $g = New-Game $root 'dx9-11-other' 'x64' @('kernel32.dll', 'd3d9.dll', 'd3d11.dll')
    [IO.File]::WriteAllBytes((Join-Path $g.Dir 'd3d9.dll'), [Text.Encoding]::ASCII.GetBytes('SOMEONE-ELSES-D3D9'))
    [void](Invoke-RunDx $kit $g $ip $yes)
    $p = Get-Placed $g
    Check '9 と 11 の両方を取り込み d3d9.dll が別の物なら、d3d9.dll はそのまま・11 の 2 つは置かれる' (($p['d3d9.dll'] -eq 'SOMEONE-ELSES-D3D9') -and ($p['d3d11.dll'] -eq 'FAKE-DXVK-x64-d3d11.dll') -and ($p['dxgi.dll'] -eq 'FAKE-DXVK-x64-dxgi.dll') -and ($p.Count -eq 3))

    # すでに置いてある（キットと同じ中身）-> 聞かない
    if ($mode -eq 'fix') {
        $g = New-Game $root 'dx9-already' 'x32' @('kernel32.dll', 'd3d9.dll')
        Copy-Item (Join-Path $kit 'dxvk\x32\d3d9.dll') $g.Dir
        $r = Invoke-RunDx $kit $g $ip 'y'
        Check 'すでにキットの d3d9.dll が横にあれば、聞かずに「配置されています」と出す' (($r.Out -match 'DXVK（DirectX 9）がゲームと同じフォルダに配置されています') -and ($r.Out -notmatch 'DXVK を配置しますか')) 'safety'
        # キットに d3d9.dll が無い（古いキット）-> 更新を案内して、何も置かない
        $kit2 = New-Kit 'rundx-oldkit'
        Add-FakeDxvk $kit2
        Remove-Item (Join-Path $kit2 'dxvk\x32\d3d9.dll')
        $g = New-Game $root 'dx9-oldkit' 'x32' @('kernel32.dll', 'd3d9.dll')
        $r = Invoke-RunDx $kit2 $g $ip 'y'
        Check 'キットに d3d9.dll が無いときは、キットの更新を案内して何も置かない' (($r.Out -match 'Update-PocketGPU\.cmd') -and ((Get-Placed $g).Count -eq 0)) 'safety'
    }

    # 置かない物：DirectX 12・Vulkan のゲーム
    $g = New-Game $root 'dx12' 'x64' @('kernel32.dll', 'd3d12.dll', 'd3d9.dll', 'dxgi.dll')
    $r = Invoke-RunDx $kit $g $ip 'y'
    Check 'DirectX 12 のゲームには何も置かず、まだ描画できないと出す' (((Get-Placed $g).Count -eq 0) -and ($r.Out -match 'DirectX 12 のゲームのようです')) 'safety'
    $g = New-Game $root 'vulkan' 'x64' @('kernel32.dll', 'vulkan-1.dll', 'd3d9.dll')
    [void](Invoke-RunDx $kit $g $ip 'y')
    Check 'Vulkan を取り込むゲームには何も置かない' ((Get-Placed $g).Count -eq 0) 'safety'

    # Android の形（PGPU_ANDROID=1）でも同じ。受け手がつながっていない（最初に「このまま起動しますか」と聞かれる）ので答えは 2 つ
    $aport = Get-FreePort
    $ae = @{ PGPU_ANDROID = '1'; PGPU_PORT = $aport }
    $g = New-Game $root 'android-dx9' 'x32' @('kernel32.dll', 'd3d9.dll')
    $r = Invoke-RunDx $kit $g $ae ('y,' + $yes)
    Check 'Android の形でも、DirectX 9（32 ビット）のゲームに dxvk\x32 の d3d9.dll が置かれる' (Test-Placed $g 'x32' @('d3d9.dll'))
    Check 'Android の形では DXVK にスマートフォンの GPU を選ばせる（DXVK_FILTER_DEVICE_NAME=PocketGPU）・案内は「Android のスマートフォン」' (($r.Out -match 'DRYRUN set DXVK_FILTER_DEVICE_NAME=PocketGPU') -and ($r.Out -match 'Android のスマートフォン の GPU で描画')) 'safety'
    $g = New-Game $root 'android-dx10' 'x64' @('kernel32.dll', 'd3d10.dll')
    [void](Invoke-RunDx $kit $g $ae ('y,' + $yes))
    Check 'Android の形でも、DirectX 10 のゲームに 3 つが置かれる' (Test-Placed $g 'x64' @('d3d10core.dll', 'd3d11.dll', 'dxgi.dll'))
}

# =====================================================================
$scenarios = [ordered]@{
    'loader'        = { param($m) Scn-Loader $m }
    'loader-ok'     = { param($m) Scn-LoaderOkLaunchers $m }
    'loader-none'   = { param($m) Scn-LoaderNone $m }
    'files'         = { param($m) Scn-Files $m }
    'files-noini'   = { param($m) Scn-FilesNoIni $m }
    'apple-service' = { param($m) Scn-AppleService $m }
    'apple-denied'  = { param($m) Scn-AppleDenied $m }
    'apple-devices' = { param($m) Scn-AppleDevices $m }
    'apple-none'    = { param($m) Scn-AppleNone $m }
    'apple-blocker' = { param($m) Scn-AppleBlocker $m }
    'tools'         = { param($m) Scn-ToolsDup $m }
    'iphone-holder' = { param($m) Scn-IphoneHolder $m }
    'iphone-other'  = { param($m) Scn-IphoneOther $m }
    'android-dup'   = { param($m) Scn-AndroidDupOld $m }
    'android-stale' = { param($m) Scn-AndroidStaleFwd $m }
    'android-port'  = { param($m) Scn-AndroidPortConflict $m }
    'android-fwd'   = { param($m) Scn-AndroidFwdMissing $m }
    'android-app-oldform' = { param($m) Scn-AndroidAppOldFormOnNew $m }
    'android-app-newform' = { param($m) Scn-AndroidAppNewFormOnOld $m }
    'android-app-half'    = { param($m) Scn-AndroidAppHalf $m }
    'android-app-stale'   = { param($m) Scn-AndroidAppStale $m }
    'android-app-conflict' = { param($m) Scn-AndroidAppConflict $m }
    'android-app-left'    = { param($m) Scn-AndroidAppLeft $m }
    'android-app-gputaken' = { param($m) Scn-AndroidAppGpuTaken $m }
    'android-app-gpuusb-old' = { param($m) Scn-AndroidAppGpuUsbOnOld $m }
    'android-app-cmd'     = { param($m) Scn-AndroidAppCmd $m }
    'pristine-app'        = { param($m) Scn-PristineApp $m }
    'port-fix'            = { param($m) Scn-PortFix $m }
    'port-lib'            = { param($m) Scn-PortLib $m }
    'port-cmd'            = { param($m) Scn-PortCmd $m }
    'gpu-av'        = { param($m) Scn-GpuAv $m }
    'checkonly'     = { param($m) Scn-CheckOnly $m }
    'privacy'       = { param($m) Scn-Privacy $m }
    'english'       = { param($m) Scn-English $m }
    'pristine'      = { param($m) Scn-Pristine $m }
    'kitdir'        = { param($m) Scn-KitDir $m }
    'run-dx'        = { param($m) Scn-RunDx $m }
    'elevated'      = { param($m) Scn-Elevated $m }
}

$marker = Join-Path $Work '.pgpu-testwork'
if (Test-Path $Work) {
    if (-not (Test-Path $marker)) { throw "$Work は、この試験が作った作業場所ではないので使いません（空のフォルダか、無いフォルダを -Work に指定してください）" }
    if (-not $KeepWork) { Get-ChildItem -LiteralPath $Work -Force | Where-Object { $_.Name -ne '.pgpu-testwork' } | Remove-Item -Recurse -Force }
} else { [void](New-Item -ItemType Directory -Path $Work -Force) }
[IO.File]::WriteAllText($marker, 'pgpu tests')
$null = New-FakeAdb
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'fakes\noop.cmd'), "@echo off`r`nexit /b 0`r`n", [Text.Encoding]::ASCII)

# 静的な検査
$script:Now = 'static'
$bad = & $script:PS -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Normalize-Files.ps1') -Check
Check '.ps1 は BOM 付き UTF-8・CRLF、.cmd は ASCII・CRLF' ($LASTEXITCODE -eq 0) 'safety'
$parseBad = @()
foreach ($f in (Get-ChildItem -LiteralPath $script:Repo -Recurse -File -Filter '*.ps1' | Where-Object { $_.FullName -notmatch '\\\.git\\' })) {
    $tokens = $null; $errs = $null
    [void][Management.Automation.Language.Parser]::ParseFile($f.FullName, [ref]$tokens, [ref]$errs)
    if ($errs -and $errs.Count -gt 0) { $parseBad += ($f.Name + ': ' + $errs[0].Message) }
}
Check ('すべての .ps1 が PowerShell ' + $PSVersionTable.PSVersion + ' の構文として読める ' + ($parseBad -join '; ')) ($parseBad.Count -eq 0) 'safety'
$ctl = @()
foreach ($f in (Get-ChildItem -LiteralPath $script:Repo -Recurse -File | Where-Object { $_.Extension -in '.ps1', '.cmd', '.py', '.cs', '.md', '.yml' -and $_.FullName -notmatch '\\\.git\\' })) {
    $bytes = [IO.File]::ReadAllBytes($f.FullName)
    for ($i = 0; $i -lt $bytes.Length; $i++) { if ($bytes[$i] -lt 32 -and $bytes[$i] -ne 9 -and $bytes[$i] -ne 10 -and $bytes[$i] -ne 13) { $ctl += ($f.Name + ' @' + $i + ' 0x' + $bytes[$i].ToString('x2')); break } }
}
Check ('制御文字（バックスペースなど）が混ざっていない（正規表現の \b が文字になる事故の見張り）' + ($ctl -join '; ')) ($ctl.Count -eq 0) 'safety'
$sec = & $script:PS -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Test-NoSecrets.ps1')
Check '秘密・個人の物が入っていない（Test-NoSecrets.ps1）' ($LASTEXITCODE -eq 0) 'safety'

$todo = @($scenarios.Keys | Where-Object { $Scenario.Count -eq 0 -or $Scenario -contains $_ })
foreach ($name in $todo) {
    foreach ($mode in 'fix', 'decline', 'sabotage') {
        $script:Now = $name + ' / ' + $mode
        $before = $script:Checks.Count
        try { & $scenarios[$name] $mode }
        catch { Check ('場面が例外で止まった：' + $_.Exception.Message + ' @ ' + $_.InvocationInfo.ScriptLineNumber) $false 'error' }
        finally { Stop-Spawned }
        Write-Host ('{0,-34} {1,3} 検査' -f $script:Now, ($script:Checks.Count - $before))
    }
}

# ---- 集計 ----
Write-Host ''
$fail = 0
foreach ($c in $script:Checks) {
    $mode = $(if ($c.Scn -match ' / (\w+)$') { $Matches[1] } else { 'static' })
    # fix と static では全部が真。decline・sabotage では「効果」が偽になる（直っていない）ことを期待し、「安全」は真のまま
    if ($mode -eq 'fix' -or $mode -eq 'static') { $expect = $true }
    elseif ($c.Kind -eq 'effect') { $expect = $false }
    else { $expect = $true }
    $good = ($c.Ok -eq $expect)
    if ($c.Kind -eq 'error') { $good = $false }
    $tag = $(if ($good) { 'OK  ' } else { 'FAIL' })
    if (-not $good) { $fail++ }
    $exp = $(if ($mode -eq 'fix' -or $mode -eq 'static') { '' } elseif ($c.Kind -eq 'effect') { '（対照：直っていないことを期待）' } else { '' })
    Write-Host ('{0} [{1}] {2}{3}' -f $tag, $c.Scn, $c.Name, $exp)
}
Write-Host ''
Write-Host ('検査 {0} 個・失敗 {1} 個' -f $script:Checks.Count, $fail)
# 対照の効き目：sabotage / decline のそれぞれで、少なくとも 1 つの効果の検査が偽になった場面だけを数える
$ctlBad = @()
foreach ($name in $todo) {
    foreach ($mode in 'decline', 'sabotage') {
        $eff = @($script:Checks | Where-Object { $_.Scn -eq ($name + ' / ' + $mode) -and $_.Kind -eq 'effect' })
        if ($eff.Count -gt 0 -and @($eff | Where-Object { -not $_.Ok }).Count -eq 0) { $ctlBad += ($name + ' / ' + $mode) }
    }
}
if ($ctlBad.Count -gt 0) { Write-Host ('対照が効いていない（直す部分を外しても効果の検査が通ってしまった）：' + ($ctlBad -join ', ')); $fail += $ctlBad.Count }
Stop-Spawned
if (-not $KeepWork) { }
exit $(if ($fail -eq 0) { 0 } else { 1 })
