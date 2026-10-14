# PocketGPU（Android）：PC 側で待つ口を選ぶ。adb forward は PC の 127.0.0.1 の口で待つ。Windows がその口を予約していると（Hyper-V・WSL などの
# 「除外されたポートの範囲」。netsh int ipv4 show excludedportrange protocol=tcp で見える）、待てずに失敗する。そこで、使う前に確かめて、
# だめなら予約の範囲の外の口を選び、android\port.txt に覚える。android\ の cmd・tools\run.ps1・Fix-PocketGPU が、同じ口を使う。
# 日本語を含むので UTF-8（BOM 付き）で保存する。BOM が無いと 5.1 は Shift_JIS として読んで壊れる。Windows PowerShell 5.1 だけで動く。
#
# 使い方：
#   pgpu_port.ps1 -PgpuPick gpu,cam,audio   android\_common.cmd が呼ぶ。口を確かめ、必要なら選び直して android\port.txt に書く
#   . pgpu_port.ps1                         何も付けずにドットで読み込むと、関数だけが使える（fix.ps1 が使う）
# 口は 3 つ：gpu（環境変数 PGPU_PORT・既定 50300）・cam（PGPU_CAM_PORT・50064）・audio（PGPU_AUDIO_PORT・50063）。
# 決め方：環境変数 > android\port.txt に覚えた口 > 既定。それが予約の範囲に入っている・ほかのプログラムが使っているときは、
# gpu は 47300 から、cam は 47400 から、audio は 47500 から順に、予約の範囲の外で空いている口を選ぶ。
# adb 自身が使っている口（前に作った adb forward）は、使っているうちに入れない。覚えた口を消したいときは、android\port.txt を削除する。
#
# 試す人は使わない、試験用の差し替え：
#   PGPU_TEST_NETSH=パス        netsh の代わりに、この .cmd を動かす（引数は本物と同じ。予約の範囲を出す物）。
#                               このときは、口を開けられないこと（Windows の予約）は見ず、netsh の出力だけで予約を決める
param([Parameter(Position = 0)][string]$PgpuPick = '')
if (-not (Get-Command M -CommandType Function -ErrorAction SilentlyContinue)) { . (Join-Path $PSScriptRoot 'pgpu_common.ps1') }

# 口の種類。First：選び直すときに、ここから順に探す（100 個）
$script:PortRoles = @(
    @{ Key = 'gpu'; Env = 'PGPU_PORT'; Default = 50300; First = 47300; Ja = 'GPU'; En = 'GPU' },
    @{ Key = 'cam'; Env = 'PGPU_CAM_PORT'; Default = 50064; First = 47400; Ja = 'ウェブカメラ'; En = 'webcam' },
    @{ Key = 'audio'; Env = 'PGPU_AUDIO_PORT'; Default = 50063; First = 47500; Ja = '音声'; En = 'audio' }
)
function Get-PortRole([string]$key) { return ($script:PortRoles | Where-Object { $_.Key -eq $key } | Select-Object -First 1) }

# ---- 覚えた口（android\port.txt。gpu=47300 のような行） ----
function Get-AndroidDirOfKit([string]$kitDir) { return (Join-Path $kitDir 'android') }
function Read-PortFile([string]$androidDir) {
    $d = @{}
    $p = Join-Path $androidDir 'port.txt'
    if (Test-Path -LiteralPath $p) {
        foreach ($l in [IO.File]::ReadAllLines($p)) {
            if ($l -match '^\s*(gpu|cam|audio)\s*=\s*(\d{1,5})\s*$') {
                $n = [int]$Matches[2]
                if ($n -ge 1024 -and $n -le 65535) { $d[$Matches[1]] = $n }
            }
        }
    }
    return $d
}
function Write-PortFile([string]$androidDir, [hashtable]$d) {
    $lines = @()
    foreach ($k in 'gpu', 'cam', 'audio') { if ($d.ContainsKey($k)) { $lines += ($k + '=' + $d[$k]) } }
    $text = ($lines -join "`r`n") + "`r`n"
    [IO.File]::WriteAllText((Join-Path $androidDir 'port.txt'), $text, [Text.Encoding]::ASCII)
}
# 口を 1 つ覚える。ほかの口の行はそのまま残す
function Set-RememberedPort([string]$key, [int]$port, [string]$androidDir) {
    $d = Read-PortFile $androidDir
    $d[$key] = $port
    Write-PortFile $androidDir $d
}
# 今使うことになっている口：環境変数 > 覚えた口 > 既定
function Get-PortSetting([string]$key, [string]$androidDir) {
    $r = Get-PortRole $key
    $e = [Environment]::GetEnvironmentVariable($r.Env)
    if ($e -and $e -match '^\d{1,5}$' -and [int]$e -ge 1 -and [int]$e -le 65535) { return [int]$e }
    $d = Read-PortFile $androidDir
    if ($d.ContainsKey($key)) { return [int]$d[$key] }
    return [int]$r.Default
}

# ---- Windows の予約（除外されたポートの範囲） ----
function Invoke-PortProc([string]$exe, [string]$arguments, [int]$ms = 15000) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = $arguments
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    try { $p = [Diagnostics.Process]::Start($psi) } catch { return @{ Ok = $false; Out = '' } }
    $o = $p.StandardOutput.ReadToEndAsync()
    $e = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($ms)) { try { $p.Kill() } catch { }; return @{ Ok = $false; Out = '' } }
    [void]$p.WaitForExit()
    return @{ Ok = ($p.ExitCode -eq 0); Out = $o.Result }
}
# 予約の範囲の一覧。Known が偽なら netsh の結果が読めなかった（そのときは、口を開けてみるしかない）
function Get-ExcludedPortRanges {
    $res = $null
    if ($env:PGPU_TEST_NETSH) { $res = Invoke-PortProc $env:ComSpec ('/c ""' + $env:PGPU_TEST_NETSH + '" int ipv4 show excludedportrange protocol=tcp"') }
    else { $res = Invoke-PortProc (Join-Path $env:WINDIR 'System32\netsh.exe') 'int ipv4 show excludedportrange protocol=tcp' }
    $ranges = @()
    foreach ($l in ($res.Out -split "`r?`n")) {
        if ($l -match '^\s*(\d+)\s+(\d+)\s*\*?\s*$') { $ranges += [pscustomobject]@{ Start = [int]$Matches[1]; End = [int]$Matches[2] } }
    }
    return @{ Known = ($res.Ok -and $ranges.Count -gt 0); Ranges = $ranges }
}
function Test-PortExcluded([int]$port, $ranges) {
    foreach ($r in @($ranges)) { if ($port -ge $r.Start -and $port -le $r.End) { return $true } }
    return $false
}
# 127.0.0.1 の口を開けてみる。free 空き・denied Windows に断られた（予約）・adb adb が使っている・inuse ほかが使っている
function Get-PortState([int]$port) {
    try {
        $l = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, $port)
        $l.Start()
        $l.Stop()
        return 'free'
    } catch {
        $ex = $_.Exception
        if ($ex.InnerException) { $ex = $ex.InnerException }
        $code = $null
        if ($ex -is [Net.Sockets.SocketException]) { $code = $ex.SocketErrorCode }
        if ($code -eq [Net.Sockets.SocketError]::AccessDenied) {
            if ($env:PGPU_TEST_NETSH) { return 'free' }
            return 'denied'
        }
        if ($code -ne [Net.Sockets.SocketError]::AddressAlreadyInUse) { return 'inuse' }
    }
    # 使われている。使っているのが adb だけなら、前に作った adb forward なので、使ってよい
    $names = @()
    try {
        foreach ($c in @(Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction Stop)) {
            $pr = Get-Process -Id $c.OwningProcess -ErrorAction SilentlyContinue
            $names += $(if ($pr) { $pr.ProcessName } else { '?' })
        }
    } catch { return 'inuse' }
    if ($names.Count -gt 0 -and @($names | Where-Object { $_ -notmatch '^adb' }).Count -eq 0) { return 'adb' }
    return 'inuse'
}
# その口が使えない理由。使えるなら空。reserved 予約されている・inuse ほかが使っている
function Get-PortProblem([int]$port, $ranges) {
    if (Test-PortExcluded $port $ranges) { return 'reserved' }
    $s = Get-PortState $port
    if ($s -eq 'denied') { return 'reserved' }
    if ($s -eq 'inuse') { return 'inuse' }
    return ''
}
# wanted が使えればそのまま。だめなら既定、次に First から順に探す。見つからなければ Port = 0。taken は同じ回に選んだ口（重ねない）
function Select-PortFor([string]$key, [int]$wanted, $ranges, [int[]]$taken = @()) {
    $r = Get-PortRole $key
    $why = Get-PortProblem $wanted $ranges
    if (-not $why -and $taken -notcontains $wanted) { return @{ Port = $wanted; Why = '' } }
    if (-not $why) { $why = 'inuse' }
    $cands = @()
    for ($i = 0; $i -lt 100; $i++) { $cands += ([int]$r.First + $i) }
    foreach ($c in $cands) {
        if ($taken -contains $c) { continue }
        if (-not (Get-PortProblem $c $ranges)) { return @{ Port = $c; Why = $why } }
    }
    return @{ Port = 0; Why = $why }
}
# 「口 X は…ので、Y を使います」の 1 行
function Get-PortChangeText([string]$key, [int]$from, [int]$to, [string]$why) {
    $r = Get-PortRole $key
    $label = $(if ($key -eq 'gpu') { '' } else { ' (' + (M $r.Ja $r.En) + ')' })
    if ($why -eq 'reserved') {
        return ((M 'ポート {0}{2} は Windows に予約されているので、{1} を使います（android\port.txt に覚えます）。' 'Port {0}{2} is reserved by Windows, so port {1} is used instead (saved in android\port.txt).') -f $from, $to, $label)
    }
    return ((M 'ポート {0}{2} は別のプログラムが使っているので、{1} を使います（android\port.txt に覚えます）。' 'Port {0}{2} is used by another program, so port {1} is used instead (saved in android\port.txt).') -f $from, $to, $label)
}

# ---- 入口：android\_common.cmd から ----
function Invoke-PortPick([string[]]$keys, [string]$androidDir) {
    $ex = Get-ExcludedPortRanges
    $file = Read-PortFile $androidDir
    $taken = @()
    $changed = $false
    $bad = $false
    foreach ($key in $keys) {
        $r = Get-PortRole $key
        if (-not $r) { continue }
        $wanted = Get-PortSetting $key $androidDir
        $sel = Select-PortFor $key $wanted $ex.Ranges $taken
        if ($sel.Port -eq 0) {
            Out-Line ((M 'ポート {0} は使えず、代わりの空いている口も見つかりませんでした。' 'Port {0} cannot be used and no free port was found instead.') -f $wanted)
            $bad = $true
            $taken += $wanted
            continue
        }
        $taken += $sel.Port
        if ($sel.Port -ne $wanted) { Out-Line (Get-PortChangeText $key $wanted $sel.Port $sel.Why) }
        # 覚えるのは、選び直したとき（wanted と違う口にしたとき）と、覚えてある口が今の口と違うとき。環境変数で指定した口がそのまま使えるなら、覚えない
        if ($sel.Port -ne $wanted -or ($file.ContainsKey($key) -and $file[$key] -ne $sel.Port)) { $file[$key] = $sel.Port; $changed = $true }
    }
    if ($changed) {
        try { Write-PortFile $androidDir $file }
        catch { Out-Line ((M 'android\port.txt に書けませんでした：{0}' 'Could not write android\port.txt: {0}') -f $_.Exception.Message); $bad = $true }
    }
    return (-not $bad)
}

if ($PgpuPick) {
    $ok = Invoke-PortPick @($PgpuPick -split '[,\s]+' | Where-Object { $_ }) (Join-Path (Split-Path -Parent $PSScriptRoot) 'android')
    exit $(if ($ok) { 0 } else { 1 })
}
