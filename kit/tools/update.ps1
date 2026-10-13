# PocketGPU：キットの更新。Update-PocketGPU.cmd と、PocketGPU.exe の「アップデート」が使う。Windows PowerShell 5.1 だけで動く。
# 日本語を含むので UTF-8（BOM 付き）で保存する。BOM が無いと 5.1 は Shift_JIS として読んで壊れる。
# 画面の文は M '日本語' 'English' の組で並べてある。言い回しはここで直す。
#
# 流れ（10/12）：
#   1. tools\update.ini の url= と token= で、kit-worker の /u/<合言葉>/update.json に聞く。最新の版のファイルの一覧と、その人の印が返る
#   2. キットのファイルの SHA-256 を一覧と比べる。dll の印の 32 文字は「#」に戻してから比べる（人ごとの印があっても、同じひな形なら同じ）
#   3. 違うファイルだけを update-backup\<日時>\new\ にダウンロードし、大きさ・印・SHA-256 を確かめる。この時点ではキットは変えない
#   4. キットの中で動いている道具（PocketGPU.exe・extras\*.exe・vklist.exe・tools\*.ps1 を動かす PowerShell）を PID で止める。
#      ゲームが iphone_icd.dll を使っている間は置き換えられないので、そのときは何も変えずに止まる
#   5. 古いファイルを update-backup\<日時>\files\ に移してから、新しいファイルを置く。1 つごとに journal.json に書く。
#      失敗したら、置いた物を消して控えを戻す。途中で落ちたときは、次に起動したときに journal.json を見て戻す
#   6. iPhone のアプリ（RpcServer-*.ipa）も新しければ、このフォルダに置く。AltStore で更新できないとき、AltServer で入れられるように
# 引数：
#   -Check            チェックだけ。何も変えない。終わりのコード：0 最新・10 新しい版あり・1 失敗・2 更新の情報なし・3 合言葉が使えない
#   -Json             最後に 1 行「PGPU-UPDATE-JSON {...}」を出す（PocketGPU.exe が読む）。ASCII の外の字は \u でにげる
#   -Yes              聞かずに進める（PocketGPU.exe から）
#   -FromApp 番号     その PID の PocketGPU.exe が閉じるのを待ってから始め、終わったら PocketGPU.exe を起動し直す
#   -NoIpa            iPhone のアプリをダウンロードしない
#   -KitDir フォルダ  更新するキットのフォルダ。無ければ、このファイルの 1 つ上。更新の道具だけを別に展開したときは探す
# 試す人は使わない、試験用の差し替え：
#   PGPU_NOPROMPT=1・PGPU_ANSWERS=y,n,...・PGPU_LANG=ja|en  pgpu_common.ps1 と同じ
#   PGPU_TEST_UPDATE_FAIL=N    N 個目を置いたところで失敗したことにする（戻しの試験）
#   PGPU_TEST_UPDATE_KILL=N    N 個目を置いたところで、戻さずにこのプロセスを終わらせる（途中で落ちたときの試験）
#   PGPU_TEST_NO_RESTART=1     -FromApp でも PocketGPU.exe を起動し直さない
#   PGPU_TEST_FOLDER=フォルダ   キットのフォルダを選ぶウィンドウの代わりに、このフォルダを選んだことにする
param(
    [switch]$Check,
    [switch]$Json,
    [switch]$Yes,
    [int]$FromApp = 0,
    [switch]$NoIpa,
    [string]$KitDir = ''
)

$ErrorActionPreference = 'Stop'
if ($env:PGPU_LANG) { $script:Ja = $env:PGPU_LANG -eq 'ja' }
else { $script:Ja = [Globalization.CultureInfo]::CurrentUICulture.Name -like 'ja*' }
function M([string]$ja, [string]$en) { if ($script:Ja) { $ja } else { $en } }
$script:NoPrompt = $env:PGPU_NOPROMPT -eq '1'
$script:Answers = New-Object System.Collections.Generic.Queue[string]
if ($env:PGPU_ANSWERS) { foreach ($x in $env:PGPU_ANSWERS.Split(',')) { $script:Answers.Enqueue($x.Trim()) } }
$script:Log = New-Object System.Collections.Generic.List[string]
$script:Result = [ordered]@{ state = 'error'; reason = ''; current = ''; version = ''; files = 0; bytes = 0; remove = 0; ipa = @(); notes = ''; message = '' }

# ---- 画面と記録 ----
function Out-Line([string]$s, [string]$color = '') {
    if ($color) { Write-Host $s -ForegroundColor $color } else { Write-Host $s }
    $script:Log.Add($s)
}
function Say-OK([string]$t)   { Out-Line ('  OK  ' + $t) 'Green' }
function Say-Warn([string]$t) { Out-Line ('  !   ' + $t) 'Yellow' }
function Say-NG([string]$t)   { Out-Line ('  NG  ' + $t) 'Red' }
function Say-Info([string]$t) { Out-Line ('      ' + $t) }

function Ask-YesNo([string]$q) {
    if ($Yes) { Out-Line ('      ' + $q + ' -> ' + (M 'はい' 'yes')); return $true }
    if ($script:NoPrompt) {
        $y = $script:Answers.Count -gt 0 -and $script:Answers.Dequeue() -eq 'y'
        Out-Line ('      ' + $q + ' -> ' + $(if ($y) { M 'はい' 'yes' } else { M 'いいえ' 'no' }))
        return $y
    }
    $a = Read-Host ('      ' + $q + ' [y/N]')
    $script:Log.Add('      ' + $q + ' -> ' + $a)
    return $a -match '^(y|yes|はい)$'
}

function Format-Size([int64]$n) {
    if ($n -ge 1MB) { return ('{0:0.0} MB' -f ($n / 1MB)) }
    if ($n -ge 1KB) { return ('{0:0} KB' -f ($n / 1KB)) }
    return ('{0} B' -f $n)
}

# 記録をキットのフォルダに書く。ユーザー名と PC 名は伏せる。合言葉は書かない
function Save-Log {
    if (-not $script:KitRoot) { return '' }
    $body = ($script:Log -join "`r`n")
    if ($env:USERNAME) { $body = $body -replace [regex]::Escape($env:USERNAME), '(user)' }
    if ($env:COMPUTERNAME) { $body = $body -replace [regex]::Escape($env:COMPUTERNAME), '(pc)' }
    if ($script:Token) { $body = $body.Replace($script:Token, '(token)') }
    $path = Join-Path $script:KitRoot 'pocketgpu-update-log.txt'
    try { [IO.File]::WriteAllText($path, $body + "`r`n", (New-Object Text.UTF8Encoding($true))) } catch { return '' }
    return $path
}

# 終わり方をそろえる。-Json なら最後の 1 行に結果を出す。人が見ているときは Enter を待つ
function Finish([int]$code, [bool]$wait = $true) {
    $log = Save-Log
    if ($log) { Out-Line ''; Out-Line ((M 'ログ: {0}' 'Log: {0}') -f $log) }
    if ($Json) {
        $j = ConvertTo-Json $script:Result -Compress -Depth 5
        $j = [regex]::Replace($j, '[^\x00-\x7F]', { param($m) '\u{0:x4}' -f [int][char]$m.Value })
        Write-Output ('PGPU-UPDATE-JSON ' + $j)
    }
    if ($wait -and -not $Json -and -not $script:NoPrompt -and $env:PGPU_NOPAUSE -eq $null) {
        Out-Line ''
        [void](Read-Host (M 'Enter キーを押すと閉じます。' 'Press Enter to close.'))
    }
    if ($script:Mutex) { try { $script:Mutex.ReleaseMutex() } catch { } }
    exit $code
}

# ---- ファイルとハッシュ ----
$script:Placeholder = New-Object byte[] 32
for ($i = 0; $i -lt 32; $i++) { $script:Placeholder[$i] = 0x23 }

function Get-Crc32([byte[]]$b) {
    if (-not $script:CrcTable) {
        $t = New-Object 'int64[]' 256
        for ($n = 0; $n -lt 256; $n++) {
            $c = [int64]$n
            for ($k = 0; $k -lt 8; $k++) { if ($c -band 1) { $c = [int64]3988292384 -bxor ($c -shr 1) } else { $c = $c -shr 1 } }
            $t[$n] = $c
        }
        $script:CrcTable = $t
    }
    $c = [int64]4294967295
    foreach ($x in $b) { $c = $script:CrcTable[[int](($c -bxor $x) -band 255)] -bxor ($c -shr 8) }
    return [BitConverter]::GetBytes([uint32]($c -bxor [int64]4294967295))
}
$script:PlaceholderCrc = Get-Crc32 $script:Placeholder

function Get-Sha256Hex([byte[]]$b) {
    $h = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($h.ComputeHash($b)) -replace '-', '').ToLowerInvariant() } finally { $h.Dispose() }
}

function Test-Bytes([byte[]]$b, [int64]$at, [byte[]]$want) {
    if ($at -lt 0 -or $at + $want.Length -gt $b.Length) { return $false }
    for ($i = 0; $i -lt $want.Length; $i++) { if ($b[$at + $i] -ne $want[$i]) { return $false } }
    return $true
}

# 印の場所を「#」32 個に、CRC の場所を「#」32 個の CRC に戻した写し。ひな形の SHA-256 と比べるため
function Get-Normalized([byte[]]$b, $e) {
    if ($null -eq $e.mark_at) { return , $b }
    $c = [byte[]]$b.Clone()
    $at = [int64]$e.mark_at
    if ($at + 32 -le $c.Length) { [Array]::Copy($script:Placeholder, 0, $c, $at, 32) }
    foreach ($o in @($e.crc_at)) {
        if ($null -ne $o -and [int64]$o + 4 -le $c.Length) { [Array]::Copy($script:PlaceholderCrc, 0, $c, [int64]$o, 4) }
    }
    return , $c
}

# 手元のファイルが一覧と同じか
function Test-Current([string]$path, $e) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    if ((Get-Item -LiteralPath $path).Length -ne [int64]$e.size) { return $false }
    $b = [IO.File]::ReadAllBytes($path)
    return (Get-Sha256Hex (Get-Normalized $b $e)) -eq $e.sha
}

# ダウンロードした物が正しいか：大きさ・その人の印・CRC・ひな形の SHA-256
function Test-Downloaded([string]$path, $e) {
    $b = [IO.File]::ReadAllBytes($path)
    if ($b.Length -ne [int64]$e.size) { return $false }
    if ($null -ne $e.mark_at) {
        if (-not (Test-Bytes $b ([int64]$e.mark_at) $script:MarkBytes)) { return $false }
        $crc = Get-Crc32 $script:MarkBytes
        foreach ($o in @($e.crc_at)) { if ($null -ne $o -and -not (Test-Bytes $b ([int64]$o) $crc)) { return $false } }
    }
    return (Get-Sha256Hex (Get-Normalized $b $e)) -eq $e.sha
}

# サーバーが返したパスがキットの中に収まるか（.. や絶対パスは断る）。人ごとの update.ini は触らない
function Test-SafePath([string]$p) {
    if (-not $p -or $p.Length -gt 200) { return $false }
    if ($p -notmatch '^[A-Za-z0-9._ -]+(/[A-Za-z0-9._ -]+)*$') { return $false }
    if ($p -match '(^|/)\.\.?(/|$)') { return $false }
    if ($p -ieq 'tools/update.ini') { return $false }
    return $true
}
function Get-Local([string]$rel) { return Join-Path $script:KitRoot ($rel -replace '/', '\') }

# ---- 設定ファイル（key=value） ----
function Read-Ini([string]$path) {
    $h = [ordered]@{}
    if (Test-Path -LiteralPath $path) {
        foreach ($line in [IO.File]::ReadAllLines($path)) {
            if ($line -match '^\s*([A-Za-z_]+)\s*=\s*(.*?)\s*$') { $h[$Matches[1].ToLowerInvariant()] = $Matches[2] }
        }
    }
    return $h
}
function Write-Ini([string]$path, $h) {
    $lines = foreach ($k in $h.Keys) { $k + '=' + $h[$k] }
    [IO.File]::WriteAllText($path, (($lines -join "`r`n") + "`r`n"), [Text.Encoding]::ASCII)
}

# ---- HTTP ----
try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch { }
function Invoke-Get([string]$url, [string]$outFile = '') {
    $req = [Net.HttpWebRequest]::Create($url)
    $req.Method = 'GET'
    $req.Timeout = 30000
    $req.ReadWriteTimeout = 120000
    $req.UserAgent = 'PocketGPU-update/1'
    $req.AutomaticDecompression = [Net.DecompressionMethods]::GZip -bor [Net.DecompressionMethods]::Deflate
    try { $res = $req.GetResponse() }
    catch [Net.WebException] {
        $ex = $_.Exception
        if ($ex.Response) { $st = [int]$ex.Response.StatusCode; $ex.Response.Close(); return @{ Ok = $false; Status = $st; Error = ('HTTP ' + $st) } }
        return @{ Ok = $false; Status = 0; Error = $ex.Message }
    }
    catch { return @{ Ok = $false; Status = 0; Error = $_.Exception.Message } }
    try {
        $s = $res.GetResponseStream()
        if ($outFile) {
            $fs = [IO.File]::Create($outFile)
            try { $s.CopyTo($fs, 1MB) } finally { $fs.Close() }
            return @{ Ok = $true; Status = 200 }
        }
        $ms = New-Object IO.MemoryStream
        $s.CopyTo($ms)
        return @{ Ok = $true; Status = 200; Bytes = $ms.ToArray() }
    } catch { return @{ Ok = $false; Status = 0; Error = $_.Exception.Message } }
    finally { $res.Close() }
}

# ---- キットのフォルダ ----
function Test-KitDir([string]$d) {
    if (-not $d) { return $false }
    # iphone_icd.dll が欠けていても（ウイルス対策ソフトが隔離したときなど）、更新用の情報（tools\update.ini）があればキットとして扱う。欠けた物を取り直すため
    if (Test-Path -LiteralPath (Join-Path $d 'tools\update.ini')) { return $true }
    return (Test-Path -LiteralPath (Join-Path $d 'Start-PocketGPU.cmd')) -and (Test-Path -LiteralPath (Join-Path $d 'iphone_icd.dll'))
}
# 更新の道具だけを展開したとき：近くと、ダウンロード・デスクトップ・ドキュメントの PocketGPU-* を 2 段まで探す
function Find-KitCandidates([string]$from) {
    $found = New-Object System.Collections.Generic.List[string]
    $parent = Split-Path -Parent $from
    if (Test-KitDir $parent) { $found.Add($parent) }
    $roots = @($parent, (Join-Path $env:USERPROFILE 'Downloads'), (Join-Path $env:USERPROFILE 'Desktop'), [Environment]::GetFolderPath('MyDocuments'))
    foreach ($r in $roots) {
        if (-not $r -or -not (Test-Path -LiteralPath $r)) { continue }
        foreach ($d in @(Get-ChildItem -LiteralPath $r -Directory -Filter 'PocketGPU*' -ErrorAction SilentlyContinue)) {
            if (Test-KitDir $d.FullName) { $found.Add($d.FullName) }
            foreach ($e in @(Get-ChildItem -LiteralPath $d.FullName -Directory -Filter 'PocketGPU*' -ErrorAction SilentlyContinue)) {
                if (Test-KitDir $e.FullName) { $found.Add($e.FullName) }
            }
        }
    }
    return @($found | ForEach-Object { $_.TrimEnd('\') } | Sort-Object -Unique)
}
function Select-Folder {
    if ($env:PGPU_TEST_FOLDER) { return $env:PGPU_TEST_FOLDER }
    if ($script:NoPrompt) { return $null }
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description = M 'キットのフォルダを選んでください。Start-PocketGPU.cmd があるフォルダです。' 'Choose your kit folder, the folder with Start-PocketGPU.cmd.'
        $dlg.ShowNewFolderButton = $false
        if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dlg.SelectedPath }
    } catch { }
    return $null
}

# ---- 動いている道具 ----
# このプロセスを呼び出した側（Fix-PocketGPU.cmd の PowerShell など）の PID。止める相手に入れない
function Get-AncestorPids {
    $set = New-Object System.Collections.Generic.HashSet[int]
    try {
        $all = @(Get-CimInstance Win32_Process -ErrorAction Stop)
        $cur = $PID
        for ($i = 0; $i -lt 12; $i++) {
            $row = $all | Where-Object { $_.ProcessId -eq $cur } | Select-Object -First 1
            if (-not $row -or -not $row.ParentProcessId -or $row.ParentProcessId -eq 0) { break }
            [void]$set.Add([int]$row.ParentProcessId)
            $cur = [int]$row.ParentProcessId
        }
    } catch { }
    return $set
}
function Get-KitProcesses {
    $root = $script:KitRoot.TrimEnd('\') + '\'
    $list = @()
    $anc = Get-AncestorPids
    foreach ($p in @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue)) {
        if ($p.ProcessId -eq $PID) { continue }
        $exe = $p.ExecutablePath
        if ($exe -and $exe.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { $list += $p; continue }
        if ($p.Name -match '^(powershell|pwsh)\.exe$' -and $p.CommandLine -and -not $anc.Contains([int]$p.ProcessId) -and
            $p.CommandLine.IndexOf($root + 'tools\', [StringComparison]::OrdinalIgnoreCase) -ge 0) { $list += $p }
    }
    return $list
}
function Stop-KitProcesses($procs) {
    # PocketGPU.exe は閉じるように頼む（子の道具を自分で止め、PC の音のミュートも戻す）。10 秒で閉じなければ止める
    foreach ($p in @($procs | Where-Object { $_.Name -ieq 'PocketGPU.exe' })) {
        $proc = Get-Process -Id $p.ProcessId -ErrorAction SilentlyContinue
        if (-not $proc) { continue }
        Say-Info ((M '停止しています: {0} PID {1}' 'Stopping {0}, PID {1}') -f $p.Name, $p.ProcessId)
        try { [void]$proc.CloseMainWindow() } catch { }
        if (-not $proc.WaitForExit(10000)) { try { Stop-Process -Id $p.ProcessId -Force } catch { } }
    }
    foreach ($p in @(Get-KitProcesses)) {
        Say-Info ((M '停止しています: {0} PID {1}' 'Stopping {0}, PID {1}') -f $p.Name, $p.ProcessId)
        try { Stop-Process -Id $p.ProcessId -Force } catch { }
    }
    $until = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $until) {
        $left = @(Get-KitProcesses)
        if ($left.Count -eq 0) { return @() }
        Start-Sleep -Milliseconds 300
    }
    return @(Get-KitProcesses)
}
function Test-Locked([string]$p) {
    if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { return $false }
    try {
        $it = Get-Item -LiteralPath $p
        if ($it.IsReadOnly) { $it.IsReadOnly = $false }
        $fs = [IO.File]::Open($p, 'Open', 'ReadWrite', 'None')
        $fs.Close()
        return $false
    } catch { return $true }
}

# ---- 控えと journal.json ----
function Save-Journal($j, [string]$path) {
    [IO.File]::WriteAllText($path, (ConvertTo-Json $j -Depth 6), (New-Object Text.UTF8Encoding($false)))
}
# 控えから戻す。控えにある物は元の場所へ。元に無かった物で、置き始めた物は消す。戻せなかった物の一覧を返す
function Restore-Journal($j, [string]$bdir) {
    $bad = @()
    # @($j.items) は使わない：PowerShell 5.1 は List[object] に入れた [ordered] を @() で包むと
    # 「Argument types do not match」で落ちる（10/12 の試験で、戻しの途中で止まった）。foreach で写す
    $items = New-Object System.Collections.ArrayList
    foreach ($x in $j.items) { [void]$items.Add($x) }
    $items.Reverse()
    foreach ($it in $items) {
        $target = Get-Local $it.path
        $bk = Join-Path (Join-Path $bdir 'files') ($it.path -replace '/', '\')
        try {
            if (Test-Path -LiteralPath $bk -PathType Leaf) {
                if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force }
                $d = Split-Path -Parent $target
                if (-not (Test-Path -LiteralPath $d)) { [void](New-Item -ItemType Directory -Path $d -Force) }
                Move-Item -LiteralPath $bk -Destination $target
            } elseif (-not $it.existed -and $it.started -and (Test-Path -LiteralPath $target)) {
                Remove-Item -LiteralPath $target -Force
            }
        } catch { $bad += $it.path }
    }
    return $bad
}
# 前に途中で止まった更新があれば戻す
function Repair-Interrupted {
    $root = Join-Path $script:KitRoot 'update-backup'
    if (-not (Test-Path -LiteralPath $root)) { return }
    foreach ($d in @(Get-ChildItem -LiteralPath $root -Directory | Sort-Object Name -Descending)) {
        $jp = Join-Path $d.FullName 'journal.json'
        if (-not (Test-Path -LiteralPath $jp)) { continue }
        try { $j = Get-Content -LiteralPath $jp -Raw -Encoding UTF8 | ConvertFrom-Json } catch { continue }
        if ($j.state -ne 'replacing') { continue }
        $bad = Restore-Journal $j $d.FullName
        $j.state = 'rolledback'
        Save-Journal $j $jp
        if ($bad.Count -eq 0) { Say-Warn (M '前回の更新が途中で止まっていたため、元のファイルに戻しました。' 'The last update stopped halfway, so the old files were put back.') }
        else {
            Say-NG ((M '元のファイルに戻せなかったファイルがあります。{0} から手動でコピーしてください。' 'Some files could not be put back. Copy them by hand from {0}.') -f (Join-Path $d.FullName 'files'))
            foreach ($b in $bad) { Say-Info $b }
        }
    }
}
# 控えは、終わった更新の新しい 3 つだけ残す。戻した更新の控えは、中のファイルが元に戻って空なので消す
function Remove-OldBackups([string]$keep) {
    $root = Join-Path $script:KitRoot 'update-backup'
    $all = @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending)
    $n = 0
    foreach ($d in $all) {
        if ($d.FullName -eq $keep) { $n++; continue }
        $jp = Join-Path $d.FullName 'journal.json'
        $state = ''
        try { $state = (Get-Content -LiteralPath $jp -Raw -Encoding UTF8 | ConvertFrom-Json).state } catch { }
        if ($state -eq 'replacing') { continue }
        $files = @(Get-ChildItem -LiteralPath (Join-Path $d.FullName 'files') -Recurse -File -ErrorAction SilentlyContinue)
        if ($state -eq 'done') { $n++ }
        if (($state -eq 'done' -and $n -gt 3) -or ($state -ne 'done' -and $files.Count -eq 0)) {
            try { Remove-Item -LiteralPath $d.FullName -Recurse -Force } catch { }
        }
    }
}

# =====================================================================
Out-Line (M 'PocketGPU のアップデート' 'PocketGPU update')

# 同時に 2 つ動かさない
$created = $false
$script:Mutex = [Threading.Mutex]::new($true, 'Local\PocketGPU.Update', [ref]$created)
if (-not $created) {
    $script:Mutex = $null
    $script:Result.reason = 'busy'
    Say-NG (M '更新はすでに実行中です。' 'An update is already running.')
    Finish 1
}

# キットのフォルダ。このファイルの 1 つ上がキットでなければ、更新の道具だけを展開したと見て探す
$here = Split-Path -Parent $PSScriptRoot
$script:KitRoot = ''
$bootstrap = $false
if ($KitDir) {
    if (-not (Test-KitDir $KitDir)) {
        $script:Result.reason = 'nokit'
        Say-NG (M '選んだフォルダはキットのフォルダではありません。Start-PocketGPU.cmd があるフォルダを選んでください。' 'That is not a kit folder. Choose the folder with Start-PocketGPU.cmd.')
        Finish 1
    }
    $script:KitRoot = (Resolve-Path -LiteralPath $KitDir).Path.TrimEnd('\')
} elseif (Test-KitDir $here) {
    $script:KitRoot = $here.TrimEnd('\')
} else {
    $cands = @(Find-KitCandidates $here)
    $pick = $null
    if ($cands.Count -eq 1) {
        Out-Line ''
        Out-Line (M 'キットのフォルダが見つかりました。' 'Found a kit folder.')
        if (Ask-YesNo ((M 'このフォルダのキットを更新しますか？ {0}' 'Update the kit in this folder? {0}') -f $cands[0])) { $pick = $cands[0] }
    } elseif ($cands.Count -gt 1) {
        Out-Line ''
        Out-Line (M 'キットのフォルダが複数見つかりました。' 'Found more than one kit folder.')
        foreach ($c in $cands) { Say-Info $c }
    }
    if (-not $pick) {
        Out-Line (M 'キットのフォルダを選んでください。' 'Choose your kit folder.')
        $pick = Select-Folder
    }
    if (-not $pick -or -not (Test-KitDir $pick)) {
        $script:Result.reason = 'nokit'
        Say-NG (M 'キットのフォルダが見つかりません。Start-PocketGPU.cmd があるフォルダで、もう一度実行してください。' 'The kit folder was not found. Run this again and choose the folder with Start-PocketGPU.cmd.')
        Finish 1
    }
    $script:KitRoot = (Resolve-Path -LiteralPath $pick).Path.TrimEnd('\')
    $bootstrap = $true
}
Out-Line ((M 'キットのフォルダ: {0}' 'Kit folder: {0}') -f $script:KitRoot)

# 更新の道具だけを展開したとき：update.ini と道具をキットにコピーしてから進める（-Check のときはコピーしない）
$iniPath = Join-Path $script:KitRoot 'tools\update.ini'
if ($bootstrap) {
    $srcIni = Join-Path $PSScriptRoot 'update.ini'
    if (Test-Path -LiteralPath $srcIni) { $iniPath = $srcIni }
    if (-not $Check -and (Test-Path -LiteralPath $srcIni)) {
        $tdir = Join-Path $script:KitRoot 'tools'
        if (-not (Test-Path -LiteralPath $tdir)) { [void](New-Item -ItemType Directory -Path $tdir -Force) }
        Copy-Item -LiteralPath $srcIni -Destination (Join-Path $tdir 'update.ini') -Force
        Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $tdir 'update.ps1') -Force
        $cmd = Join-Path $here 'Update-PocketGPU.cmd'
        if (Test-Path -LiteralPath $cmd) { Copy-Item -LiteralPath $cmd -Destination (Join-Path $script:KitRoot 'Update-PocketGPU.cmd') -Force }
        $iniPath = Join-Path $tdir 'update.ini'
        Say-OK (M '更新用のファイルを、キットのフォルダにコピーしました。' 'Copied the updater into the kit folder.')
    }
}

$ini = Read-Ini $iniPath
$script:Token = [string]$ini['token']
$base = ([string]$ini['url']).TrimEnd('/')
if (-not $base -or $script:Token -notmatch '^[A-Za-z0-9_-]{16,64}$' -or $base -notmatch '^https?://[A-Za-z0-9.:-]+$') {
    $script:Result.reason = 'noini'
    Say-NG (M 'このキットには更新用の情報がありません。Discord の #get-kit で /kit と入力して、表示される手順で更新用のファイルを追加してください。' 'This kit has no update information. Type /kit in #get-kit on Discord and follow the steps to add the updater.')
    Finish 2
}
$kitIni = Read-Ini (Join-Path $script:KitRoot 'tools\kit.ini')
$current = [string]$ini['version']
if (-not $current) { $current = [string]$kitIni['app'] }
$script:Result.current = $current

# 前に途中で止まった更新を戻す（-Check のときは何も変えない）
if (-not $Check) { Repair-Interrupted }

# 1. 最新の版の一覧を聞く
Out-Line ''
Out-Line (M '新しいバージョンがあるかをチェックしています…' 'Checking for a new version...')
$r = Invoke-Get ($base + '/u/' + $script:Token + '/update.json')
if (-not $r.Ok) {
    if ($r.Status -eq 404 -or $r.Status -eq 403) {
        $script:Result.reason = 'token'
        Say-NG (M 'このキットの更新用の情報が使えません。Discord の #get-kit で /kit と入力してください。' 'The update information in this kit is not valid. Type /kit in #get-kit on Discord.')
        Finish 3
    }
    if ($r.Status -eq 503) {
        $script:Result.reason = 'norelease'
        Say-Warn (M '配布中の更新はまだありません。' 'No update has been published yet.')
        $script:Result.state = 'latest'
        Finish 0
    }
    $script:Result.reason = 'network'
    $script:Result.message = $r.Error
    Say-NG ((M 'サーバーに接続できませんでした。インターネットの接続をチェックしてから、もう一度実行してください。{0}' 'Could not connect to the server. Check the internet connection and try again. {0}') -f $r.Error)
    Finish 1
}
try {
    $man = [Text.Encoding]::UTF8.GetString($r.Bytes) | ConvertFrom-Json
    if (-not $man.version -or -not $man.files) { throw 'empty' }
    foreach ($f in @($man.files)) { if (-not (Test-SafePath $f.path) -or $f.sha -notmatch '^[0-9a-f]{64}$') { throw ('bad path ' + $f.path) } }
    foreach ($p in @($man.remove)) { if ($p -and -not (Test-SafePath $p)) { throw ('bad path ' + $p) } }
} catch {
    $script:Result.reason = 'manifest'
    $script:Result.message = $_.Exception.Message
    Say-NG (M 'サーバーから届いた更新の一覧が正しくありません。' 'The update list from the server is not valid.')
    Finish 1
}
$script:MarkBytes = [Text.Encoding]::ASCII.GetBytes([string]$man.mark)
if ($script:MarkBytes.Length -ne 32) { $script:MarkBytes = $script:Placeholder }
$notes = $(if ($script:Ja) { [string]$man.notes.ja } else { [string]$man.notes.en })
$script:Result.version = [string]$man.version
$script:Result.notes = $notes

# 2. 手元と比べる
$need = New-Object System.Collections.Generic.List[object]
foreach ($f in @($man.files)) {
    if (-not (Test-Current (Get-Local $f.path) $f)) { $need.Add($f) }
}
$removes = @(@($man.remove) | Where-Object { $_ -and (Test-Path -LiteralPath (Get-Local $_) -PathType Leaf) })
# iPhone のアプリ：RpcServer はいつも、ほかはキットにその名前の IPA があるときだけ。同じ名前の物があれば足りている
$ipaNeed = New-Object System.Collections.Generic.List[object]
if (-not $NoIpa) {
    foreach ($a in @($man.apps)) {
        if (-not $a -or [string]$a.file -notmatch '^[A-Za-z0-9._-]+\.ipa$' -or [string]$a.prefix -notmatch '^[A-Za-z0-9]+$') { continue }
        $have = @(Get-ChildItem -LiteralPath $script:KitRoot -Filter ($a.prefix + '-*.ipa') -File -ErrorAction SilentlyContinue)
        if ($have.Count -eq 0 -and $a.prefix -ne 'RpcServer') { continue }
        if (@($have | Where-Object { $_.Name -ieq $a.file }).Count -gt 0) { continue }
        $ipaNeed.Add([pscustomobject]@{ App = $a; Old = @($have | ForEach-Object { $_.Name }) })
    }
}
[int64]$bytes = 0
foreach ($f in $need) { $bytes += [int64]$f.size }
foreach ($x in $ipaNeed) { $bytes += [int64]$x.App.size }
$script:Result.files = $need.Count
$script:Result.remove = $removes.Count
$script:Result.bytes = $bytes
$script:Result.ipa = @($ipaNeed | ForEach-Object { [string]$_.App.file })

Out-Line ((M '現在のバージョン: {0}' 'Current version: {0}') -f $(if ($current) { $current } else { M '不明' 'unknown' }))
if ($need.Count -eq 0 -and $removes.Count -eq 0 -and $ipaNeed.Count -eq 0) {
    $script:Result.state = 'latest'
    Say-OK ((M '最新の状態です。バージョン {0}' 'Up to date. Version {0}') -f $man.version)
    if (-not $Check -and ($ini['version'] -ne [string]$man.version -or $ini['release'] -ne [string]$man.rel)) {
        $ini['version'] = [string]$man.version
        $ini['release'] = [string]$man.rel
        try { Write-Ini $iniPath $ini } catch { }
    }
    Finish 0
}

$script:Result.state = 'available'
Out-Line ''
Out-Line ((M '新しいバージョン {0} があります。' 'Version {0} is available.') -f $man.version) 'Cyan'
if ($notes) {
    Out-Line (M '変更内容:' 'What is new:')
    foreach ($l in ($notes -split "`r?`n")) { Say-Info $l }
}
if ($need.Count -gt 0) {
    Out-Line ((M '更新するファイル: {0} 個、{1}' 'Files to update: {0}, {1}') -f $need.Count, (Format-Size ($bytes - (@($ipaNeed | ForEach-Object { [int64]$_.App.size }) | Measure-Object -Sum).Sum)))
    foreach ($f in $need) { Say-Info $f.path }
}
if ($removes.Count -gt 0) {
    Out-Line ((M '不要になったファイル: {0} 個。バックアップのフォルダに移します。' 'Files no longer used: {0}. They are moved to the backup folder.') -f $removes.Count)
    foreach ($p in $removes) { Say-Info $p }
}
foreach ($x in $ipaNeed) {
    Out-Line ((M 'iPhone のアプリ: {0} をこのフォルダに保存します。{1}' 'iPhone app: {0} will be saved in this folder. {1}') -f $x.App.file, (Format-Size ([int64]$x.App.size)))
}
if ($Check) { Finish 10 }

Out-Line ''
if (-not (Ask-YesNo (M '今すぐ更新しますか？' 'Update now?'))) {
    $script:Result.state = 'cancelled'
    Out-Line (M '更新を中止しました。何も変更していません。' 'Update cancelled. Nothing was changed.')
    Finish 0
}

# 3. ダウンロードして確かめる。キットはまだ変えない
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$bdir = Join-Path $script:KitRoot ('update-backup\' + $stamp)
$newDir = Join-Path $bdir 'new'
[void](New-Item -ItemType Directory -Path $newDir -Force)
$jobs = New-Object System.Collections.Generic.List[object]
foreach ($f in $need) { $jobs.Add([pscustomobject]@{ Path = [string]$f.path; Entry = $f; Url = [string]$f.url }) }
foreach ($x in $ipaNeed) { $jobs.Add([pscustomobject]@{ Path = [string]$x.App.file; Entry = $x.App; Url = [string]$x.App.url }) }
Out-Line ''
foreach ($jb in $jobs) {
    $dst = Join-Path $newDir ($jb.Path -replace '/', '\')
    $dd = Split-Path -Parent $dst
    if (-not (Test-Path -LiteralPath $dd)) { [void](New-Item -ItemType Directory -Path $dd -Force) }
    Out-Line ((M 'ダウンロード中: {0} {1}' 'Downloading: {0} {1}') -f $jb.Path, (Format-Size ([int64]$jb.Entry.size)))
    $ok = $false
    $err = ''
    for ($try = 1; $try -le 3 -and -not $ok; $try++) {
        $g = Invoke-Get $jb.Url $dst
        if (-not $g.Ok) { $err = $g.Error; Start-Sleep -Seconds $try; continue }
        if (Test-Downloaded $dst $jb.Entry) { $ok = $true } else { $err = M 'ファイルが壊れています' 'the file is damaged' }
    }
    if (-not $ok) {
        $script:Result.state = 'error'
        $script:Result.reason = 'download'
        $script:Result.message = $jb.Path + ': ' + $err
        Say-NG ((M 'ダウンロードできませんでした: {0} {1}' 'Could not download {0}: {1}') -f $jb.Path, $err)
        Say-Info (M 'キットのファイルは何も変更していません。しばらくしてから、もう一度実行してください。' 'No kit files were changed. Try again later.')
        try { Remove-Item -LiteralPath $bdir -Recurse -Force } catch { }
        Finish 1
    }
}
Say-OK (M 'ダウンロードしたファイルをすべてチェックしました。' 'All downloaded files were checked.')

# 4. 動いている道具を止める。PocketGPU.exe から呼ばれたときは、先にそれが閉じるのを待つ
$restartApp = $false
if ($FromApp -gt 0) {
    $restartApp = $true
    $ap = Get-Process -Id $FromApp -ErrorAction SilentlyContinue
    if ($ap) { Out-Line (M 'PocketGPU が閉じるのを待っています…' 'Waiting for PocketGPU to close...'); [void]$ap.WaitForExit(30000) }
}
$procs = @(Get-KitProcesses)
if ($procs.Count -gt 0) {
    Out-Line ''
    Out-Line (M '次のツールが動いています。停止してから更新します。' 'These PocketGPU tools are running. They are stopped before the update.')
    foreach ($p in $procs) { Say-Info ('{0} PID {1}' -f $p.Name, $p.ProcessId) }
    if (@($procs | Where-Object { $_.Name -ieq 'PocketGPU.exe' }).Count -gt 0) { $restartApp = $true }
    if (-not (Ask-YesNo (M 'ツールを停止して続けますか？' 'Stop them and continue?'))) {
        $script:Result.state = 'cancelled'
        Out-Line (M '更新を中止しました。何も変更していません。' 'Update cancelled. Nothing was changed.')
        try { Remove-Item -LiteralPath $bdir -Recurse -Force } catch { }
        Finish 0
    }
    $left = @(Stop-KitProcesses $procs)
    if ($left.Count -gt 0) {
        $script:Result.state = 'error'
        $script:Result.reason = 'running'
        foreach ($p in $left) { Say-NG ((M '停止できませんでした: {0} PID {1}。閉じてから、もう一度実行してください。' 'Could not stop {0}, PID {1}. Close it and try again.') -f $p.Name, $p.ProcessId) }
        try { Remove-Item -LiteralPath $bdir -Recurse -Force } catch { }
        Finish 1
    }
}

# 置き換える物の一覧。existed は始める前に決める（途中で落ちたときに、元から有ったかを間違えない）
$items = New-Object System.Collections.Generic.List[object]
foreach ($jb in $jobs) { $items.Add([ordered]@{ path = $jb.Path; new = $true; existed = (Test-Path -LiteralPath (Get-Local $jb.Path) -PathType Leaf); started = $false }) }
foreach ($p in $removes) { $items.Add([ordered]@{ path = [string]$p; new = $false; existed = $true; started = $false }) }
foreach ($x in $ipaNeed) { foreach ($o in $x.Old) { $items.Add([ordered]@{ path = [string]$o; new = $false; existed = $true; started = $false }) } }

# ゲームなどが使っているファイルは置き換えられない。そのときは何も変えずに止まる
$locked = @($items | Where-Object { $_.existed -and (Test-Locked (Get-Local $_.path)) } | ForEach-Object { $_.path })
if ($locked.Count -gt 0) {
    $script:Result.state = 'error'
    $script:Result.reason = 'locked'
    $script:Result.message = ($locked -join ', ')
    Say-NG (M '次のファイルを使用中のアプリがあります。iPhone の GPU で動いているゲームやアプリを終了してから、もう一度実行してください。' 'An app is using these files. Close games and apps running on the iPhone GPU, then try again.')
    foreach ($p in $locked) { Say-Info $p }
    Say-Info (M 'キットのファイルは何も変更していません。' 'No kit files were changed.')
    try { Remove-Item -LiteralPath $bdir -Recurse -Force } catch { }
    Finish 1
}

# 5. 置き換える。1 つごとに journal.json に書く
Out-Line ''
Out-Line (M 'ファイルを置き換えています…' 'Replacing files...')
$jpath = Join-Path $bdir 'journal.json'
$journal = [ordered]@{ state = 'replacing'; from = $current; to = [string]$man.version; release = $man.rel; at = (Get-Date).ToString('s'); items = $items }
Save-Journal $journal $jpath
$filesDir = Join-Path $bdir 'files'
$n = 0
try {
    foreach ($it in $items) {
        $target = Get-Local $it.path
        if ($it.existed) {
            $bk = Join-Path $filesDir ($it.path -replace '/', '\')
            $bd = Split-Path -Parent $bk
            if (-not (Test-Path -LiteralPath $bd)) { [void](New-Item -ItemType Directory -Path $bd -Force) }
            Move-Item -LiteralPath $target -Destination $bk
        }
        if ($it.new) {
            $it.started = $true
            Save-Journal $journal $jpath
            $td = Split-Path -Parent $target
            if (-not (Test-Path -LiteralPath $td)) { [void](New-Item -ItemType Directory -Path $td -Force) }
            Copy-Item -LiteralPath (Join-Path $newDir ($it.path -replace '/', '\')) -Destination $target
        }
        $n++
        if ($env:PGPU_TEST_UPDATE_KILL -and $n -eq [int]$env:PGPU_TEST_UPDATE_KILL) { [Environment]::Exit(9) }
        if ($env:PGPU_TEST_UPDATE_FAIL -and $n -eq [int]$env:PGPU_TEST_UPDATE_FAIL) { throw 'PGPU_TEST_UPDATE_FAIL' }
    }
    # 置いた物をもう一度確かめる
    foreach ($jb in $jobs) { if (-not (Test-Downloaded (Get-Local $jb.Path) $jb.Entry)) { throw ('verify ' + $jb.Path) } }
    $journal.state = 'done'
    Save-Journal $journal $jpath
} catch {
    $why = $_.Exception.Message
    $bad = Restore-Journal $journal $bdir
    $journal.state = 'rolledback'
    try { Save-Journal $journal $jpath } catch { }
    $script:Result.state = 'error'
    $script:Result.reason = 'replace'
    $script:Result.message = $why
    Say-NG ((M '更新に失敗したため、元のファイルに戻しました: {0}' 'The update failed, so the old files were put back: {0}') -f $why)
    if ($bad.Count -gt 0) {
        Say-NG ((M '元のファイルに戻せなかったファイルがあります。{0} から手動でコピーしてください。' 'Some files could not be put back. Copy them by hand from {0}.') -f $filesDir)
        foreach ($b in $bad) { Say-Info $b }
    }
    try { Remove-Item -LiteralPath $newDir -Recurse -Force } catch { }
    if ($restartApp -and $env:PGPU_TEST_NO_RESTART -ne '1' -and (Test-Path -LiteralPath (Join-Path $script:KitRoot 'PocketGPU.exe'))) {
        try { Start-Process -FilePath (Join-Path $script:KitRoot 'PocketGPU.exe') -WorkingDirectory $script:KitRoot } catch { }
    }
    Finish 1
}

try { Remove-Item -LiteralPath $newDir -Recurse -Force } catch { }
# 外したファイルで空になったフォルダを消す（キットのフォルダより上には行かない）
foreach ($it in $items) {
    if ($it.new) { continue }
    $d = Split-Path -Parent (Get-Local $it.path)
    while ($d -and $d.Length -gt $script:KitRoot.Length -and (Test-Path -LiteralPath $d) -and
           -not (Get-ChildItem -LiteralPath $d -Force | Select-Object -First 1)) {
        try { Remove-Item -LiteralPath $d -Force } catch { break }
        $d = Split-Path -Parent $d
    }
}
$ini['version'] = [string]$man.version
$ini['release'] = [string]$man.rel
try { Write-Ini $iniPath $ini } catch { }
Remove-OldBackups $bdir
$script:Result.state = 'updated'
Out-Line ''
Say-OK ((M '更新しました。バージョン {0}' 'Updated to version {0}.') -f $man.version)
Say-Info ((M '古いファイルは {0} に残しました。' 'The old files are kept in {0}.') -f $filesDir)
foreach ($x in $ipaNeed) {
    Out-Line ''
    Say-OK ((M 'iPhone のアプリの新しいバージョンを保存しました: {0}' 'Saved the new iPhone app: {0}') -f $x.App.file)
    Say-Info (M 'AltStore にソースを追加している場合は、AltStore の「Update」で更新してください。AltStore で更新できない場合は、AltServer の「Sideload .ipa」でこのファイルをインストールしてください。' 'If you added the source in AltStore, tap Update in AltStore. If that does not work, install this file with Sideload .ipa in AltServer.')
    Say-Info (M 'PC 側と iPhone のアプリは、同じバージョンにそろえてください。そろっていないと接続できません。' 'Keep the PC side and the iPhone app on the same version. They cannot connect otherwise.')
}
if ($restartApp) {
    if ($env:PGPU_TEST_NO_RESTART -eq '1') { Say-Info 'PGPU_TEST_NO_RESTART' }
    elseif (Test-Path -LiteralPath (Join-Path $script:KitRoot 'PocketGPU.exe')) {
        Out-Line (M 'PocketGPU を起動し直します。' 'Starting PocketGPU again.')
        try { Start-Process -FilePath (Join-Path $script:KitRoot 'PocketGPU.exe') -WorkingDirectory $script:KitRoot } catch { }
    }
}
# PocketGPU.exe から呼ばれたときは閉じる。iPhone のアプリを保存したときは、案内を読めるよう Enter を待つ
Finish 0 ((-not ($FromApp -gt 0)) -or $ipaNeed.Count -gt 0)
