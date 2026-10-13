# PocketGPU を直す：Start-PocketGPU.cmd のチェックと同じ所を見て、直せる物は 1 つずつ聞いてから直し、直せない物は何をすればよいかを出す。
# Fix-PocketGPU.cmd から呼ぶ。Windows PowerShell 5.1 だけで動く（Python は要らない）。
# 日本語を含むので UTF-8（BOM 付き）で保存する。BOM が無いと 5.1 は Shift_JIS として読んで壊れる。
# 画面の文は仮。M '日本語' 'English' の組で並べてあるので、言い回しはここで直す。
#
# 決まり：
#   - 自分では何も消さない。止める・入れ替える・取り直す前に、必ず「y/N」で聞く。聞かれて y と答えた物だけを直す
#   - 入れ替える前の物は fix-backup\<日時>\ に残す（取り直しは update.ps1 が update-backup\ に残す）
#   - レジストリ・Windows の設定・ウイルス対策ソフトの設定は変えない
#   - 結果は pocketgpu-fix-log.txt と pocketgpu-fix-summary.txt（Discord に貼れる短い文）に書く。合言葉・ユーザー名・PC 名・フォルダの場所は載せない
#
#   Fix-PocketGPU.cmd                    順にチェックして、直せる物は聞いてから直す
#   Fix-PocketGPU.cmd -CheckOnly         チェックだけ。何も変えない（何を直せるかだけ出す）
#   Fix-PocketGPU.cmd -Only apple,gpu    その項目だけ。pc・files・loader・apple・tools・iphone・android・gpu
#   Fix-PocketGPU.cmd -KitDir フォルダ    直すキットのフォルダ。無ければ、このファイルの 1 つ上（無ければ近くを探す）
#   終わりのコード：0 問題なし（または直った）・1 直らなかった物がある・2 キットのフォルダが見つからない
#
# 試す人は使わない、試験用の差し替え（pgpu_common.ps1 の物に加えて）：
#   PGPU_TEST_SERVICE_START=パス   Apple のサービスを始める・Apple Devices を開く代わりに、この .cmd を動かす
#   PGPU_TEST_SERVICE_DENIED=1     サービスを始めようとして、権限が無くて断られたことにする
#   PGPU_TEST_LAUNCH_LOG=パス      ページ・ストア・アプリを開く代わりに、開くはずだった物をこのファイルに書く。クリップボードにも書かない
#   PGPU_TEST_HOLDER_ROOT=フォルダ  GPU を握っているプログラムの探し方を、この下の iphone_icd.dll を読み込んだ物だけにする
#   PGPU_TEST_STOP_ALLOW=PID,PID    止めてよいプログラムを、この PID だけにする（試験が、自分の物以外を止めないための保険）
#   PGPU_TEST_AV_EVENT=1           ウイルス対策ソフトが iphone_icd.dll を検出した記録があることにする
#   ADB=パス・PGPU_PORT=番号        Android の adb.exe と、PC 側の口（既定は 50300。キットの android\ の cmd と同じ）
#   PGPU_CAM_PORT=番号・PGPU_AUDIO_PORT=番号   Android のアプリのカメラとマイクの、PC 側の口（既定は 50064 と 50063。android\android_app.cmd と同じ）
param(
    [switch]$CheckOnly,
    [string[]]$Only = @(),
    [Alias('KitDir')][string]$Kit = '',
    [switch]$NoSummary
)
. (Join-Path $PSScriptRoot 'pgpu_common.ps1')

$script:FixVersion = '1'
$script:LoaderMin = [Version]'1.3.234'
$script:Total = 8
$script:CheckOnly = [bool]$CheckOnly
$script:Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ })
$script:Results = New-Object System.Collections.Generic.List[object]
$script:Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$script:Elevated = Test-Elevated
$script:Ctx = @{ MuxOk = $false; Dev = $null }

# ---- 小さな道具 ----
function Want([string]$id) { return ($script:Only.Count -eq 0) -or ($script:Only -contains $id) }
function Add-Result([string]$id, [string]$state, [string]$title, [string]$note = '') {
    # state：ok 問題なし・fixed 直した・unfixed 直らなかった（または直せない）・warn 気をつけたい・skip 対象外
    $script:Results.Add([pscustomobject]@{ Id = $id; State = $state; Title = $title; Note = $note })
}
# 直してよいかを聞く。-CheckOnly のときは聞かずに「いいえ」
function Offer([string]$q) {
    if ($script:CheckOnly) { Out-Line ('      ' + $q + ' -> ' + (M '（-CheckOnly なので聞きません）' '(-CheckOnly: not asking)')); return $false }
    return (Ask-YesNo $q)
}
function Test-LooksLikeKit([string]$d) {
    foreach ($n in 'iphone_icd.dll', 'iphone_icd.json', 'vklist.exe', 'tools\update.ini', 'tools\kit.ini') {
        if (Test-Path -LiteralPath (Join-Path $d $n)) { return $true }
    }
    return $false
}
function Read-KitIni {
    $script:ProtoVersion = 3
    $script:AppVersion = ''
    $ini = Join-Path $script:KitDir 'tools\kit.ini'
    if (Test-Path -LiteralPath $ini) {
        foreach ($line in [IO.File]::ReadAllLines($ini)) {
            if ($line -match '^proto=(\d+)') { $script:ProtoVersion = [int]$Matches[1] }
            if ($line -match '^app=(.+)$') { $script:AppVersion = $Matches[1].Trim() }
        }
    }
}
# 外のプログラムを窓を出さずに動かし、出力と終わりのコードを返す
function Invoke-Native([string]$exe, [string]$arguments, [int]$timeoutMs = 20000, $envs = $null, [switch]$Oem) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = $arguments
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
    if (-not $p.WaitForExit($timeoutMs)) {
        try { $p.Kill() } catch { }
        return @{ Ok = $false; Exit = -1; Out = ''; Err = 'timeout'; TimedOut = $true }
    }
    [void]$p.WaitForExit()
    return @{ Ok = ($p.ExitCode -eq 0); Exit = $p.ExitCode; Out = $o.Result; Err = $er.Result; TimedOut = $false }
}
# ページ・ストア・アプリを開く。試験では開かずに記録だけする
function Invoke-Launch([string]$kind, [string]$target) {
    if ($env:PGPU_TEST_LAUNCH_LOG -or $env:PGPU_TEST_SERVICE_START) {
        if ($env:PGPU_TEST_LAUNCH_LOG) { Add-Content -LiteralPath $env:PGPU_TEST_LAUNCH_LOG -Value ($kind + ' ' + $target) }
        if ($env:PGPU_TEST_SERVICE_START -and ($kind -eq 'app' -or $kind -eq 'service')) {
            [void](Invoke-Native $env:ComSpec ('/c "' + $env:PGPU_TEST_SERVICE_START + '"') 15000)
        }
        return $true
    }
    try {
        if ($kind -eq 'app') { Start-Process ('shell:AppsFolder\' + $target) } else { Start-Process $target }
        return $true
    } catch { Say-Warn ((M '開けませんでした：' 'Could not open it: ') + $_.Exception.Message); return $false }
}
# ページを開くか聞く。URL は先に画面に出す
function Offer-Page([string]$url, [string]$question) {
    Say-Info ((M '開くページ：' 'Page: ') + $url)
    if (Offer $question) { [void](Invoke-Launch 'url' $url) }
}
function Format-List2($items) { return (@($items) -join ', ') }

# ---- 動いているプログラム ----
function Get-SelfPids {
    $set = New-Object System.Collections.Generic.HashSet[int]
    [void]$set.Add($PID)
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
# キットのフォルダの中の .exe が動いている物
function Get-KitProcs {
    $root = $script:KitDir.TrimEnd('\') + '\'
    $self = Get-SelfPids
    $list = @()
    try {
        foreach ($p in @(Get-CimInstance Win32_Process -ErrorAction Stop)) {
            if ($self.Contains([int]$p.ProcessId)) { continue }
            $exe = $p.ExecutablePath
            if ($exe -and $exe.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { $list += $p }
        }
    } catch { }
    return $list
}
# iphone_icd.dll を読み込んでいるプログラム（= iPhone の GPU の受け手を握っている）。tasklist /m で名前から探し、読み込んだ場所を調べる
function Get-DriverHolders {
    $r = Invoke-Native (Join-Path $env:WINDIR 'System32\tasklist.exe') '/m iphone_icd.dll /fo csv /nh' 20000 $null -Oem
    $self = Get-SelfPids
    $root = $(if ($env:PGPU_TEST_HOLDER_ROOT) { $env:PGPU_TEST_HOLDER_ROOT.TrimEnd('\') + '\' } else { '' })
    $list = @()
    foreach ($line in ($r.Out -split "`r?`n")) {
        if ($line -notmatch '^"([^"]+)","(\d+)","([^"]*)"') { continue }
        $procName = $Matches[1]
        $procId = [int]$Matches[2]
        if ($self.Contains($procId)) { continue }
        $path = ''
        try {
            $m = (Get-Process -Id $procId -ErrorAction Stop).Modules | Where-Object { $_.ModuleName -ieq 'iphone_icd.dll' } | Select-Object -First 1
            if ($m) { $path = $m.FileName }
        } catch { }
        if ($root -and -not ($path -and $path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase))) { continue }
        $exe = ''
        try { $exe = (Get-CimInstance Win32_Process -Filter ('ProcessId=' + $procId) -ErrorAction Stop).ExecutablePath } catch { }
        $list += [pscustomobject]@{ Name = $procName; Pid = $procId; Exe = $exe; Dll = $path }
    }
    return $list
}
# 止める。窓があれば閉じるように頼み、閉じなければ止める。聞いたあとでだけ呼ぶ
function Stop-ByPid([int]$procId) {
    $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
    if (-not $p) { return $true }
    if ($env:PGPU_TEST_STOP_ALLOW -and (@($env:PGPU_TEST_STOP_ALLOW -split ',') -notcontains [string]$procId)) { Say-Warn ('PGPU_TEST_STOP_ALLOW: refused to stop PID ' + $procId); return $false }
    $asked = $false
    try { $asked = $p.CloseMainWindow() } catch { }
    if (-not ($asked -and $p.WaitForExit(10000))) {
        try { Stop-Process -Id $procId -Force -ErrorAction Stop } catch { Say-Warn ((M '止められませんでした：' 'Could not stop it: ') + $_.Exception.Message); return $false }
        [void]$p.WaitForExit(5000)
    }
    return -not (Get-Process -Id $procId -ErrorAction SilentlyContinue)
}
function Get-ProcLabel($name, $procId, $exe) {
    $s = ('{0} (PID {1})' -f $name, $procId)
    if ($exe) { $s += ' ' + $exe }
    return $s
}
# 口を使っている物。state は Listen か Established
function Get-TcpOwners([int]$port, [string]$state, [switch]$Remote) {
    try {
        if ($Remote) { $c = @(Get-NetTCPConnection -RemotePort $port -State $state -ErrorAction Stop) }
        else { $c = @(Get-NetTCPConnection -LocalPort $port -State $state -ErrorAction Stop) }
    } catch { return @() }
    $out = @()
    foreach ($x in $c) {
        $pr = Get-Process -Id $x.OwningProcess -ErrorAction SilentlyContinue
        $exe = ''
        try { $exe = (Get-CimInstance Win32_Process -Filter ('ProcessId=' + $x.OwningProcess) -ErrorAction Stop).ExecutablePath } catch { }
        $out += [pscustomobject]@{ Pid = [int]$x.OwningProcess; Name = $(if ($pr) { $pr.ProcessName + '.exe' } else { '?' }); Exe = $exe }
    }
    return @($out | Sort-Object Pid -Unique)
}

# ---- キットのファイル ----
function Test-ScriptParses([string]$path) {
    try {
        $tokens = $null
        $errs = $null
        [void][Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errs)
        return (-not $errs -or $errs.Count -eq 0)
    } catch { return $false }
}
# 必要な物の一覧：相対の場所と種類
function Get-RequiredFiles {
    $l = @(
        @{ P = 'iphone_icd.dll'; K = 'dll64' }, @{ P = 'iphone_icd.json'; K = 'json' },
        @{ P = 'x86\iphone_icd.dll'; K = 'dll32' }, @{ P = 'x86\iphone_icd.json'; K = 'json' },
        @{ P = 'vklist.exe'; K = 'exe' },
        @{ P = 'Start-PocketGPU.cmd'; K = 'text' }, @{ P = 'Run-on-iPhone-GPU.cmd'; K = 'text' }, @{ P = 'Update-PocketGPU.cmd'; K = 'text' },
        @{ P = 'tools\pgpu_common.ps1'; K = 'ps1' }, @{ P = 'tools\start.ps1'; K = 'ps1' }, @{ P = 'tools\run.ps1'; K = 'ps1' },
        @{ P = 'tools\update.ps1'; K = 'ps1' }, @{ P = 'tools\kit.ini'; K = 'text' }
    )
    if (Test-Path -LiteralPath (Join-Path $script:KitDir 'android')) {
        $l += @{ P = 'android\_common.cmd'; K = 'text' }, @{ P = 'android\start_android.cmd'; K = 'text' }, @{ P = 'android\stop_android.cmd'; K = 'text' }, @{ P = 'android\server_aarch64'; K = 'text' }
    }
    return $l
}
# 欠け・壊れの一覧を返す。1 つ = @{ P = 場所; Why = missing|empty|notpe|wrongarch|badjson|badlib|parse }
function Test-KitFiles {
    $bad = @()
    foreach ($f in (Get-RequiredFiles)) {
        $full = Join-Path $script:KitDir $f.P
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { $bad += @{ P = $f.P; Why = 'missing' }; continue }
        if ((Get-Item -LiteralPath $full).Length -eq 0) { $bad += @{ P = $f.P; Why = 'empty' }; continue }
        switch ($f.K) {
            { $_ -in 'dll64', 'dll32', 'exe' } {
                $pe = Get-PeInfo $full
                if (-not $pe) { $bad += @{ P = $f.P; Why = 'notpe' } }
                elseif ($f.K -eq 'dll64' -and $pe.Machine -ne 0x8664) { $bad += @{ P = $f.P; Why = 'wrongarch' } }
                elseif ($f.K -eq 'dll32' -and $pe.Machine -ne 0x14c) { $bad += @{ P = $f.P; Why = 'wrongarch' } }
            }
            'json' {
                try {
                    $j = [IO.File]::ReadAllText($full) | ConvertFrom-Json
                    $lib = [string]$j.ICD.library_path
                    if (-not $lib) { throw 'no library_path' }
                    $target = $(if ([IO.Path]::IsPathRooted($lib)) { $lib } else { Join-Path (Split-Path -Parent $full) $lib })
                    if (-not (Test-Path -LiteralPath $target -PathType Leaf)) { $bad += @{ P = $f.P; Why = 'badlib' } }
                } catch { $bad += @{ P = $f.P; Why = 'badjson' } }
            }
            'ps1' { if (-not (Test-ScriptParses $full)) { $bad += @{ P = $f.P; Why = 'parse' } } }
        }
    }
    return $bad
}
function Get-WhyText([string]$why) {
    switch ($why) {
        'missing' { return (M '無い' 'missing') }
        'empty' { return (M '中身が空' 'empty') }
        'notpe' { return (M '壊れている（実行ファイルとして読めない）' 'damaged (not a valid program file)') }
        'wrongarch' { return (M '64 ビット・32 ビットが違う' 'wrong 32/64-bit type') }
        'badjson' { return (M '壊れている（JSON として読めない）' 'damaged (not valid JSON)') }
        'badlib' { return (M '指している DLL が無い' 'points to a DLL that is missing') }
        'parse' { return (M '壊れている（途中で切れているかもしれない）' 'damaged (may be cut off)') }
        default { return $why }
    }
}
# 取り直す。update.ps1 に任せる（差分だけダウンロード・SHA-256 の確認・動いている道具は聞いてから止める・古い物は update-backup\ に残す）
function Invoke-KitUpdate {
    $upd = Join-Path $script:KitDir 'tools\update.ps1'
    if (-not (Test-Path -LiteralPath (Join-Path $script:KitDir 'tools\update.ini'))) { return @{ Ran = $false; Why = 'noini' } }
    if (-not (Test-Path -LiteralPath $upd) -or -not (Test-ScriptParses $upd)) { return @{ Ran = $false; Why = 'noupdater' } }
    $args2 = '-NoProfile -ExecutionPolicy Bypass -File "' + $upd + '" -NoIpa -KitDir "' + $script:KitDir + '"'
    $saved = @{ A = $env:PGPU_ANSWERS; N = $env:PGPU_NOPAUSE }
    $env:PGPU_NOPAUSE = '1'
    # 試験（聞かずに進む設定）では、この修復で y と答えた分として、update.ps1 の問いにも y と答える
    if ($script:NoPrompt) { $env:PGPU_ANSWERS = 'y,y,y,y' }
    try {
        $p = Start-Process -FilePath (Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe') -ArgumentList $args2 -NoNewWindow -Wait -PassThru
        $code = $p.ExitCode
    } finally { $env:PGPU_ANSWERS = $saved.A; $env:PGPU_NOPAUSE = $saved.N }
    return @{ Ran = $true; Exit = $code }
}

# ---- 起動の入口（pocketgpu.cmd など） ----
function Get-Template([string]$name) {
    foreach ($d in @((Join-Path $PSScriptRoot 'fix-data'), (Join-Path $script:KitDir 'tools\fix-data'))) {
        $p = Join-Path $d $name
        if (Test-Path -LiteralPath $p -PathType Leaf) { return $p }
    }
    return $null
}
# 入口を実際に動かして、渡る環境変数に VK_ICD_FILENAMES があるか見る。分からなければ $null
function Test-LauncherEnv([string]$name) {
    $full = Join-Path $script:KitDir $name
    $r = Invoke-Native $env:ComSpec ('/c ""' + $full + '" cmd.exe /c set"') 20000 @{ IPHONE_VK_ONLY = '1' }
    if ($r.TimedOut) { return $null }
    $found = $false
    foreach ($l in ($r.Out -split "`r?`n")) {
        if ($l -match '^VK_ICD_FILENAMES=(.+)$') {
            $found = $true
            # 指しているファイルが本当にあるか
            foreach ($f in ($Matches[1] -split ';')) { if ($f -and -not (Test-Path -LiteralPath $f)) { return 'target-missing' } }
        }
    }
    if ($found) { return 'ok' } else { return 'stale' }
}
function Test-Launcher([string]$name) {
    $full = Join-Path $script:KitDir $name
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { return 'missing' }
    if ($name -ieq 'check_android.cmd') {
        $t = [IO.File]::ReadAllText($full)
        if ($t -match '(?im)^\s*set\s+"?VK_ICD_FILENAMES=') { return 'ok' } else { return 'stale' }
    }
    $r = Test-LauncherEnv $name
    if ($null -eq $r) { return 'unknown' }
    return $r
}
function Get-LauncherNames {
    $l = @('pocketgpu.cmd')
    if ((Test-Path -LiteralPath (Join-Path $script:KitDir 'android')) -or (Test-Path -LiteralPath (Join-Path $script:KitDir 'pocketgpu_android.cmd'))) {
        $l += 'pocketgpu_android.cmd', 'check_android.cmd'
    }
    return $l
}
# 入れ替える。前の物は fix-backup\<日時>\ に写してから、同じ場所に置く
function Replace-Launcher([string]$name) {
    $src = Get-Template $name
    if (-not $src) { return @{ Ok = $false; Why = 'notemplate' } }
    $dst = Join-Path $script:KitDir $name
    $backup = ''
    try {
        if (Test-Path -LiteralPath $dst) {
            $bdir = Join-Path $script:KitDir ('fix-backup\' + $script:Stamp)
            if (-not (Test-Path -LiteralPath $bdir)) { [void](New-Item -ItemType Directory -Path $bdir -Force) }
            $backup = Join-Path $bdir $name
            Copy-Item -LiteralPath $dst -Destination $backup -Force
            if ((Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash) { throw 'backup mismatch' }
        }
        $tmp = $dst + '.new'
        Copy-Item -LiteralPath $src -Destination $tmp -Force
        Move-Item -LiteralPath $tmp -Destination $dst -Force
        if ((Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash) { throw 'verify' }
    } catch { return @{ Ok = $false; Why = $_.Exception.Message } }
    return @{ Ok = $true; Backup = $backup }
}
function Get-DriverPage {
    $names = ''
    try { $names = (@(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object { $_.Name }) -join ' ') } catch { }
    if ($names -match 'NVIDIA') { return 'https://www.nvidia.com/Download/index.aspx' }
    if ($names -match 'AMD|Radeon') { return 'https://www.amd.com/en/support' }
    if ($names -match 'Intel') { return 'https://www.intel.com/content/www/us/en/support/detect.html' }
    return 'ms-settings:windowsupdate'
}

# ---- Apple のサービス ----
# Apple Devices か Store の iTunes を起動して、サービスが始まるのを待つ。始まれば Done。聞いてから起動する
function Try-OpenAppleApp($apple) {
    $pkg = $(if ($apple.AppleDevices) { $apple.AppleDevices } else { $apple.StoreITunes })
    $appName = $(if ($apple.AppleDevices) { 'Apple Devices' } else { 'iTunes' })
    $aumid = Get-Aumid $pkg
    if (-not $aumid) { return @{ Done = $false; Note = (M '起動する名前が分からなかった' 'could not find the app name') } }
    Say-Info ((M '{0} を起動すると、Apple のサービスが始まります。iPhone を USB ケーブルでつないでから起動してください。' 'Opening {0} starts the Apple service. Plug the iPhone in with USB first.') -f $appName)
    if (-not (Offer ((M '{0} を起動しますか？' 'Open {0}?') -f $appName))) { return @{ Done = $false; Note = (M '起動を選ばなかった' 'opening not chosen') } }
    [void](Invoke-Launch 'app' $aumid)
    Say-Info (M 'サービスが開始されるまで、最大 30 秒待ちます' 'Waiting up to 30 seconds for the service')
    if (Wait-Mux 30) { return @{ Done = $true; Note = '' } }
    return @{ Done = $false; Note = (M '起動したが、サービスが始まらなかった' 'opened it, but the service did not start') }
}
# Apple のサービス（usbmuxd）として答えるか。口が開いているだけでは足りない（ほかのプログラムが口を使っているかもしれない）
function Test-MuxAnswers {
    $c = Connect-Tcp '127.0.0.1' $script:MuxPort 1500
    if (-not $c) { return $false }
    try { [void](Invoke-Mux $c 'ListDevices'); return $true } catch { return $false } finally { $c.Close() }
}
function Wait-Mux([int]$seconds) {
    for ($i = 0; $i -lt $seconds -and -not (Test-MuxAnswers); $i++) { Start-Sleep -Seconds 1 }
    return (Test-MuxAnswers)
}

# ---- Android ----
function Find-Adb {
    if ($env:ADB -and (Test-Path -LiteralPath $env:ADB)) { return $env:ADB }
    $c = Get-Command adb.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { return $c.Source }
    $dirs = @((Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools'), (Join-Path $env:USERPROFILE 'platform-tools'), (Join-Path $env:USERPROFILE 'Downloads\platform-tools'),
        (Join-Path $env:USERPROFILE 'Desktop\platform-tools'), 'C:\platform-tools', 'C:\adb', (Join-Path $env:ProgramFiles 'platform-tools'),
        (Join-Path $script:KitDir 'platform-tools'), (Join-Path (Split-Path -Parent $script:KitDir) 'platform-tools'))
    foreach ($d in $dirs) { $p = Join-Path $d 'adb.exe'; if (Test-Path -LiteralPath $p) { return $p } }
    return $null
}
function Invoke-Adb([string]$adb, [string]$arguments, [int]$ms = 20000) { return (Invoke-Native $adb $arguments $ms) }
function Get-AndroidState($adb, [string]$serial, [int]$port) {
    $s = @{ Pids = @(); PhoneHash = ''; Forward = $null; Owners = @() }
    $sa = $(if ($serial) { '-s ' + $serial + ' ' } else { '' })
    $r = Invoke-Adb $adb ($sa + 'shell pidof pgpu_server')
    $s.Pids = @(($r.Out -split '\s+') | Where-Object { $_ -match '^\d+$' })
    $r = Invoke-Adb $adb ($sa + 'shell sha256sum /data/local/tmp/pgpu_server')
    if ($r.Out -match '^([0-9a-fA-F]{64})') { $s.PhoneHash = $Matches[1].ToLowerInvariant() }
    $s.Forward = (Get-AdbForwards $adb)[[string]$port]
    $s.Owners = @(Get-TcpOwners $port 'Listen')
    return $s
}
# adb forward の一覧：PC の口 -> 向け先（tcp:50060 や localabstract:pocketgpu-webcam）。端末が複数でも、口の番号で見る
function Get-AdbForwards($adb) {
    $m = @{}
    $r = Invoke-Adb $adb 'forward --list'
    foreach ($l in ($r.Out -split "`r?`n")) {
        if ($l -match '\btcp:(\d+)\s+(\S+)') { $m[$Matches[1]] = $Matches[2] }
    }
    return $m
}
# PocketGPU アプリ（Android）の USB の口。アプリ 0.1.2 から、カメラとマイクの口は localabstract に変わった（安全のため。adb だけが入れる）。
#   0.1.2 以上：adb forward tcp:50064 localabstract:pocketgpu-webcam・adb forward tcp:50063 localabstract:pocketgpu-audio
#   0.1.1 以前：adb forward tcp:50064 tcp:50064・adb forward tcp:50063 tcp:50063（0.1.2 でも、アプリの設定で古い PC のツールを受け付けるときは使える）
# PC の口は PGPU_CAM_PORT・PGPU_AUDIO_PORT で変えられる（既定は 50064・50063。キットの android\android_app.cmd と同じ）。端末の側の番号は変わらない
$script:AppPkg = 'com.tsurutanmen.pocketgpu'
function Get-AppPortSpecs {
    return @(
        @{ Name = 'webcam'; Port = $(if ($env:PGPU_CAM_PORT) { [int]$env:PGPU_CAM_PORT } else { 50064 }); New = 'localabstract:pocketgpu-webcam'; Old = 'tcp:50064' },
        @{ Name = 'audio'; Port = $(if ($env:PGPU_AUDIO_PORT) { [int]$env:PGPU_AUDIO_PORT } else { 50063 }); New = 'localabstract:pocketgpu-audio'; Old = 'tcp:50063' }
    )
}
# アプリの版 -> 向け方。0.1.2 以上は new、それより前は old、読めなければ unknown
function Get-AppForm([string]$ver) {
    if ($ver -match '^(\d+)\.(\d+)(?:\.(\d+))?') {
        $third = $(if ($Matches[3]) { $Matches[3] } else { '0' })
        $v = [Version]('{0}.{1}.{2}' -f $Matches[1], $Matches[2], $third)
        if ($v -ge [Version]'0.1.2') { return 'new' } else { return 'old' }
    }
    return 'unknown'
}
# アプリの中の GPU の受け手の口。アプリ 0.1.4 から、USB 用の localabstract:pocketgpu-gpu でも待つ（端末の 50060 が使えなくても動く。
# 10/14 テスターの Redmagic 9S Pro では、0.1.3 の受け手が 50060 で待てずにすぐ終わった）。キットの android\android_app.cmd と同じ：
#   0.1.4 以上：adb forward tcp:50300 localabstract:pocketgpu-gpu
#   0.1.3 以前：adb forward tcp:50300 tcp:50060
#   版が読めない：どちらも認める（android_app.cmd は tcp:50060 を使う。0.1.4 も 50060 が空いていればそこで待つ）
$script:AppGpuUsb = 'localabstract:pocketgpu-gpu'
$script:AppGpuTcp = 'tcp:50060'
function Get-AppGpuForm([string]$ver) {
    if ($ver -match '^(\d+)\.(\d+)(?:\.(\d+))?') {
        $third = $(if ($Matches[3]) { $Matches[3] } else { '0' })
        $v = [Version]('{0}.{1}.{2}' -f $Matches[1], $Matches[2], $third)
        if ($v -ge [Version]'0.1.4') { return 'usb' } else { return 'tcp' }
    }
    return 'unknown'
}
function Get-AndroidApp($adb, [string]$serial) {
    $sa = $(if ($serial) { '-s ' + $serial + ' ' } else { '' })
    $a = @{ Installed = $false; Version = ''; Form = 'unknown'; GpuForm = 'unknown'; GpuDest = $script:AppGpuTcp }
    $r = Invoke-Adb $adb ($sa + 'shell pm path ' + $script:AppPkg)
    if ($r.Out -notmatch 'package:') { return $a }
    $a.Installed = $true
    $r = Invoke-Adb $adb ($sa + 'shell dumpsys package ' + $script:AppPkg)
    if ($r.Out -match '(?m)^\s*versionName=(\S+)') {
        $a.Version = $Matches[1]; $a.Form = Get-AppForm $a.Version; $a.GpuForm = Get-AppGpuForm $a.Version
        if ($a.GpuForm -eq 'usb') { $a.GpuDest = $script:AppGpuUsb }
    }
    return $a
}
# アプリの口の adb forward のうち、このキットの形（新旧どちらか）をしている物の PC の口の番号
function Get-AppLeftoverPorts($fw) {
    $l = @()
    foreach ($sp in (Get-AppPortSpecs)) {
        $t = $fw[[string]$sp.Port]
        if ($t -and ($t -eq $sp.New -or $t -eq $sp.Old)) { $l += $sp.Port }
    }
    return $l
}
# 今の状態の問題を数える。Quiet でなければ、画面にも出す。Problems の名前：
#   GPU の口   norecv dup oldrecv portconflict fwdmissing fwdwrong fwdstale busy nohello
#              appgpufwd（アプリの中の受け手へ、版に合わない向け方でつないでいる。0.1.4 以上に tcp:50060、0.1.3 以前に localabstract）
#   アプリの口 appleft（アプリが無いのに残っている）・appfwdwrong（版に合わない向け方、または別の物を指す）・appfwdhalf（片方しか無い）・appfwdstale（口が開いていない）・appportconflict
function Get-AndroidProblems($adb, [string]$serial, [int]$port, [string]$localHash, [bool]$Quiet) {
    $r = @{ Problems = @(); Conflict = @(); AppFix = @(); AppLeft = @(); AppConflict = @(); AppRecv = $false; Hello = $null; Note = ''; App = $null }
    $pr = New-Object System.Collections.Generic.List[string]
    $out = { param([string]$kind, [string]$text) if (-not $Quiet) { switch ($kind) { 'ok' { Say-OK $text } 'ng' { Say-NG $text } default { Say-Info $text } } } }
    $s = Get-AndroidState $adb $serial $port
    $app = Get-AndroidApp $adb $serial
    $r.App = $app
    # アプリの中の受け手だけが動いている形：android\android_app.cmd が作る。キットの受け手は動いていない。
    # GPU の口の向け先は、端末の 50060（tcp:50060）か、0.1.4 からの USB 用の口（localabstract:pocketgpu-gpu）
    $appRecv = ($s.Pids.Count -eq 0) -and $app.Installed -and ($s.Forward -eq $script:AppGpuTcp -or $s.Forward -eq $script:AppGpuUsb)
    $r.AppRecv = $appRecv
    if ($s.Pids.Count -eq 0) {
        if ($appRecv) { & $out 'ok' ((M 'スマートフォンでは、PocketGPU アプリの中の受け手（{0}）を使っています。' 'The phone uses the receiver inside the PocketGPU app ({0}).') -f $s.Forward) }
        else { $pr.Add('norecv'); & $out 'ng' (M 'スマートフォンで受け手（pgpu_server）が動いていません。' 'The receiver (pgpu_server) is not running on the phone.') }
    }
    elseif ($s.Pids.Count -gt 1) { $pr.Add('dup'); & $out 'ng' ((M 'スマートフォンで受け手（pgpu_server）が {0} 個、二重に動いています。PID：{1}' 'The receiver (pgpu_server) runs {0} times on the phone. PIDs: {1}') -f $s.Pids.Count, ($s.Pids -join ' ')) }
    else { & $out 'ok' ((M 'スマートフォンで受け手が 1 個、動いています。PID {0}' 'One receiver runs on the phone. PID {0}') -f $s.Pids[0]) }
    if ($s.Pids.Count -gt 0 -and $localHash -and $s.PhoneHash -and $s.PhoneHash -ne $localHash) {
        $pr.Add('oldrecv'); & $out 'ng' (M 'スマートフォンで動いている受け手が、このキットの受け手（android\server_aarch64）と違います。古い受け手が残っています。' 'The receiver on the phone differs from the one in this kit (android\server_aarch64). An old receiver remains.')
    }
    $conflict = @($s.Owners | Where-Object { $_.Name -notmatch '^adb' })
    $r.Conflict = $conflict
    if ($conflict.Count -gt 0) {
        $pr.Add('portconflict')
        & $out 'ng' ((M 'PC の口 {0} を、adb ではないプログラムが使っています：' 'Port {0} on this PC is used by a program that is not adb: ') -f $port)
        foreach ($o in $conflict) { & $out 'info' (Get-ProcLabel $o.Name $o.Pid $o.Exe) }
    } elseif (-not $s.Forward) { $pr.Add('fwdmissing'); & $out 'ng' ((M '口 {0} の adb forward がありません。' 'There is no adb forward for port {0}.') -f $port) }
    elseif ($s.Forward -ne ('tcp:' + $port) -and -not $appRecv) { $pr.Add('fwdwrong'); & $out 'ng' ((M '口 {0} の adb forward が、別の口 {1} を指しています。' 'The adb forward for port {0} points at another port, {1}.') -f $port, $s.Forward) }
    elseif ($appRecv -and $app.GpuForm -eq 'tcp' -and $s.Forward -eq $script:AppGpuUsb) {
        # 0.1.3 以前は USB 用の口で待たない。0.1.4 以上に tcp:50060 は、答えるなら問題にしない（答えないときは下のあいさつで見る）
        $pr.Add('appgpufwd')
        & $out 'ng' ((M 'GPU の口 {0} の adb forward が、アプリ {1} に合わない向け方 {2} です。アプリ 0.1.4 以上は localabstract:pocketgpu-gpu、0.1.3 以前は tcp:50060 です。' 'The adb forward for the GPU port {0} uses {2}, which does not fit app {1}. App 0.1.4 and newer use localabstract:pocketgpu-gpu, 0.1.3 and older use tcp:50060.') -f $port, $app.Version, $s.Forward)
    }
    elseif ($s.Owners.Count -eq 0) { $pr.Add('fwdstale'); & $out 'ng' ((M 'adb forward は残っていますが、PC の口 {0} が開いていません（adb が再起動したあと）。' 'The adb forward is listed but port {0} on this PC is not open (after an adb restart).') -f $port) }
    else { & $out 'ok' ((M 'PC の口 {0} は adb の forward でスマートフォンにつながっています。' 'Port {0} on this PC is connected to the phone by adb forward.') -f $port) }

    $hello = $null
    if ($pr.Count -eq 0) {
        $c = Connect-Tcp '127.0.0.1' $port 1500
        if ($c) { try { $hello = Invoke-Hello $c 3000 } finally { $c.Close() } } else { $hello = @{ Ok = $false; Why = 'closed' } }
        if ($hello.Ok) { & $out 'ok' ((M '受け手が応答しました：' 'The receiver answered: ') + $hello.Name) }
        elseif ($hello.Why -eq 'timeout') { $pr.Add('busy'); & $out 'ng' (M '受け手が応答しません。PC のほかのプログラムが使っているか、前の接続が残っています。' 'The receiver does not answer. Another program on this PC is using it, or an old connection remains.') }
        elseif ($appRecv -and $app.GpuForm -eq 'usb' -and $s.Forward -eq $script:AppGpuTcp) {
            # 0.1.4 以上のアプリは、端末の 50060 が使えないと 50061 などで待つ。USB 用の口なら番号に関係なく届く
            $pr.Add('appgpufwd')
            & $out 'ng' ((M 'GPU の口 {0} は端末の 50060 につながっていますが、アプリ {1} の受け手が答えません。50060 が使えず、別の番号で待っているのかもしれません。アプリ 0.1.4 以上は localabstract:pocketgpu-gpu でつなぎます。' 'The GPU port {0} goes to port 50060 on the phone, but the receiver of app {1} does not answer. It may listen on another port because 50060 is taken. App 0.1.4 and newer are reached with localabstract:pocketgpu-gpu.') -f $port, $app.Version)
        }
        else {
            $pr.Add('nohello'); & $out 'ng' (M '口はつながっていますが、受け手が答えません。' 'The port is connected but the receiver does not answer.')
            if ($appRecv) { & $out 'info' (M 'アプリの中の受け手です。PocketGPU アプリを開いて、GPU がオンになっているか確かめてください。キットの受け手に切り替えることもできます。' 'This is the receiver inside the app. Open the PocketGPU app and check that GPU is on. You can also switch to the kit receiver.') }
            if ($appRecv -and $app.GpuForm -eq 'tcp') { & $out 'info' (M 'アプリが 0.1.3 以前です。スマートフォンの 50060 を別の物が使っていると、アプリの受け手は動きません。キットの android\pocketgpu.apk（0.1.4 以上）に更新すると、USB 用の口で待つので、ぶつかりません。' 'The app is 0.1.3 or older. If something else on the phone uses port 50060, the app receiver cannot run. Update to android\pocketgpu.apk from the kit (0.1.4 or newer): it also listens on a USB-only port, so nothing is in the way.') }
        }
    }
    $r.Hello = $hello

    # ---- PocketGPU アプリのカメラとマイクの口 ----
    $fw = Get-AdbForwards $adb
    $specs = Get-AppPortSpecs
    $form = $app.Form
    if (-not $app.Installed) {
        $left = @(Get-AppLeftoverPorts $fw)
        if ($left.Count -gt 0) {
            $pr.Add('appleft'); $r.AppLeft = $left
            & $out 'ng' ((M 'PocketGPU アプリが入っていないのに、口 {0} の adb forward が残っています。' 'The PocketGPU app is not on the phone, but the adb forward for port(s) {0} remains.') -f ($left -join ' '))
        }
    } else {
        $appInfo = $(if ($app.Version) { $app.Version } else { M '版を読めませんでした' 'version unknown' })
        $present = @($specs | Where-Object { $fw.ContainsKey([string]$_.Port) })
        if ($present.Count -eq 0) {
            & $out 'info' ((M 'PocketGPU アプリ（{0}）：カメラとマイクの口は、まだ開いていません。使うときは android\android_app.cmd を実行してください。' 'PocketGPU app ({0}): the camera and mic ports are not open yet. To use them, run android\android_app.cmd.') -f $appInfo)
        } else {
            foreach ($sp in $specs) {
                $t = $fw[[string]$sp.Port]
                $want = $(if ($form -eq 'old') { $sp.Old } else { $sp.New })
                $owners = @(Get-TcpOwners $sp.Port 'Listen')
                $foreign = @($owners | Where-Object { $_.Name -notmatch '^adb' })
                if ($foreign.Count -gt 0) {
                    $pr.Add('appportconflict'); $r.AppFix += $sp
                    $r.AppConflict += @($foreign | ForEach-Object { [pscustomobject]@{ Name = $_.Name; Pid = $_.Pid; Exe = $_.Exe; Port = $sp.Port } })
                    & $out 'ng' ((M 'PC の口 {0} を、adb ではないプログラムが使っています：' 'Port {0} on this PC is used by a program that is not adb: ') -f $sp.Port)
                    foreach ($o in $foreign) { & $out 'info' (Get-ProcLabel $o.Name $o.Pid $o.Exe) }
                } elseif (-not $t) {
                    $pr.Add('appfwdhalf'); $r.AppFix += $sp
                    & $out 'ng' ((M '{0} の口 {1} の adb forward がありません。カメラとマイクの組のもう片方はつながっています。' 'There is no adb forward for the {0} port {1}, but the other one of the camera and mic pair is connected.') -f $sp.Name, $sp.Port)
                } elseif ($t -ne $sp.New -and $t -ne $sp.Old) {
                    $pr.Add('appfwdwrong'); $r.AppFix += $sp
                    & $out 'ng' ((M '{0} の口 {1} の adb forward が、別の物 {2} を指しています。' 'The adb forward for the {0} port {1} points at something else, {2}.') -f $sp.Name, $sp.Port, $t)
                } elseif ($form -ne 'unknown' -and $t -ne $want) {
                    $pr.Add('appfwdwrong'); $r.AppFix += $sp
                    if ($form -eq 'new') { & $out 'ng' ((M '{0} の口 {1} が古い向け方（{2}）です。アプリ {3} は、設定で「古い PC のツールも受け付ける」をオンにしたときしか、古い向け方を受けません。' 'The {0} port {1} uses the old form ({2}). App {3} takes the old form only when "Also accept old PC tools" is on in its settings.') -f $sp.Name, $sp.Port, $t, $appInfo) }
                    else { & $out 'ng' ((M '{0} の口 {1} が新しい向け方（{2}）です。アプリ {3} は、この向け方を受けません。' 'The {0} port {1} uses the new form ({2}). App {3} does not take this form.') -f $sp.Name, $sp.Port, $t, $appInfo) }
                } elseif ($owners.Count -eq 0) {
                    $pr.Add('appfwdstale'); $r.AppFix += $sp
                    & $out 'ng' ((M 'adb forward は残っていますが、PC の口 {0} が開いていません（adb が再起動したあと）。' 'The adb forward is listed but port {0} on this PC is not open (after an adb restart).') -f $sp.Port)
                } else {
                    & $out 'ok' ((M '{0} の口 {1}：adb forward {2}（アプリ {3}）' 'The {0} port {1}: adb forward {2} (app {3})') -f $sp.Name, $sp.Port, $t, $appInfo)
                }
            }
        }
    }
    $r.Problems = @($pr)
    $r.Note = $(if ($hello -and $hello.Ok) { $hello.Name } elseif ($appRecv) { 'app receiver' } else { '' })
    return $r
}

# ---- ウイルス対策の気配 ----
function Get-AvSigns {
    $signs = @()
    foreach ($rel in 'iphone_icd.dll', 'x86\iphone_icd.dll') {
        $full = Join-Path $script:KitDir $rel
        $json = Join-Path $script:KitDir ($rel -replace 'dll$', 'json')
        if (-not (Test-Path -LiteralPath $full) -and (Test-Path -LiteralPath $json)) { $signs += ((M '{0} だけが無い（json はある）。隔離された可能性' '{0} is gone but its json is here. It may have been quarantined') -f $rel) }
        elseif ((Test-Path -LiteralPath $full) -and (Get-Item -LiteralPath $full).Length -eq 0) { $signs += ((M '{0} の中身が空。駆除された可能性' '{0} is empty. It may have been cleaned') -f $rel) }
    }
    if ($env:PGPU_TEST_AV_EVENT -eq '1') { $signs += (M 'Windows セキュリティの記録に iphone_icd.dll の検出がある（試験）' 'Windows Security has a detection for iphone_icd.dll (test)') }
    else {
        try {
            $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-Windows Defender/Operational'; Id = 1116, 1117, 1118, 1119; StartTime = (Get-Date).AddDays(-30) } -MaxEvents 100 -ErrorAction Stop |
                Where-Object { $_.Message -match 'iphone_icd|PocketGPU' })
            if ($ev.Count -gt 0) { $signs += (M 'Windows セキュリティの記録に iphone_icd.dll の検出がある' 'Windows Security has a detection for iphone_icd.dll') }
        } catch { }
    }
    return $signs
}
function Get-AvNames {
    try { return @(Get-CimInstance -Namespace 'root\SecurityCenter2' -ClassName AntiVirusProduct -ErrorAction Stop | ForEach-Object { $_.displayName }) } catch { return @() }
}
# 読み込めない原因になりやすい、足りない DLL
function Get-MissingImports([string]$path, [string]$sysDir) {
    $pe = Get-PeInfo $path
    if (-not $pe) { return @() }
    $miss = @()
    foreach ($n in $pe.Imports) {
        if ($n -match '^(api|ext)-ms-') { continue }
        if (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $path) $n)) { continue }
        if (Test-Path -LiteralPath (Join-Path $script:KitDir $n)) { continue }
        if (Test-Path -LiteralPath (Join-Path $sysDir $n)) { continue }
        $miss += $n
    }
    return $miss
}

# =====================================================================
# 最初に、直すキットのフォルダを決める
if ($Kit) {
    if (-not (Test-Path -LiteralPath $Kit -PathType Container)) {
        Out-Line (M 'そのフォルダがありません。' 'That folder does not exist.')
        exit 2
    }
    $script:KitDir = (Resolve-Path -LiteralPath $Kit).Path.TrimEnd('\')
} elseif (-not (Test-LooksLikeKit $script:KitDir)) {
    # このファイルがキットの外（GitHub から取ったフォルダなど）にあるとき：近くのキットを探す
    $found = New-Object System.Collections.Generic.List[string]
    $roots = @((Split-Path -Parent $script:KitDir), (Join-Path $env:USERPROFILE 'Downloads'), (Join-Path $env:USERPROFILE 'Desktop'), [Environment]::GetFolderPath('MyDocuments'))
    foreach ($r in $roots) {
        if (-not $r -or -not (Test-Path -LiteralPath $r)) { continue }
        foreach ($d in @(Get-ChildItem -LiteralPath $r -Directory -Filter 'PocketGPU*' -ErrorAction SilentlyContinue)) {
            if (Test-LooksLikeKit $d.FullName) { $found.Add($d.FullName) }
            foreach ($e in @(Get-ChildItem -LiteralPath $d.FullName -Directory -Filter 'PocketGPU*' -ErrorAction SilentlyContinue)) {
                if (Test-LooksLikeKit $e.FullName) { $found.Add($e.FullName) }
            }
        }
    }
    $cands = @($found | Sort-Object -Unique)
    $pick = $null
    if ($cands.Count -eq 1) {
        Out-Line (M 'キットのフォルダが見つかりました。' 'Found a kit folder.')
        if (Ask-YesNo ((M 'このフォルダのキットを直しますか？ {0}' 'Fix the kit in this folder? {0}') -f $cands[0])) { $pick = $cands[0] }
    } elseif ($cands.Count -gt 1) {
        Out-Line (M 'キットのフォルダが複数見つかりました。直すフォルダを -KitDir で指定してください。' 'Found more than one kit folder. Choose one with -KitDir.')
        foreach ($c in $cands) { Say-Info $c }
    }
    if (-not $pick) {
        Out-Line (M 'キットのフォルダが見つかりません。Fix-PocketGPU.cmd -KitDir "キットのフォルダ" の形で、もう一度実行してください。' 'The kit folder was not found. Run this again as: Fix-PocketGPU.cmd -KitDir "path to the kit folder"')
        exit 2
    }
    $script:KitDir = (Resolve-Path -LiteralPath $pick).Path.TrimEnd('\')
}
Read-KitIni

Out-Line (M 'PocketGPU を直します' 'Fix PocketGPU')
Out-Line (M 'Start-PocketGPU.cmd と同じ所をチェックして、直せる物は 1 つずつ聞いてから直します。何も消しません。' 'This checks the same things as Start-PocketGPU.cmd and fixes what it can, asking before each fix. Nothing is deleted.')
if ($script:CheckOnly) { Out-Line (M '-CheckOnly：チェックだけです。何も変更しません。' '-CheckOnly: checking only. Nothing is changed.') 'Cyan' }

# ==== 1. この PC ====
if (Want 'pc') {
    Say-Step 1 $script:Total (M 'この PC' 'This PC')
    $os = [Environment]::OSVersion.Version
    $t = M 'この PC' 'This PC'
    if (-not [Environment]::Is64BitOperatingSystem -or $os.Major -lt 10) {
        Say-NG (M '64 ビット版の Windows 10 または 11 が必要です。' 'A 64-bit Windows 10 or 11 is needed.')
        Add-Result 'pc' 'unfixed' $t (M '64 ビット版の Windows 10・11 ではない' 'not 64-bit Windows 10/11')
    } else {
        $winName = $(if ($os.Build -ge 22000) { 'Windows 11' } else { 'Windows 10' })
        Say-OK ((M '{0}・ビルド {1}・64 ビット' '{0}, build {1}, 64-bit') -f $winName, $os.Build)
        $notes = @()
        $state = 'ok'
        if ($script:Elevated) {
            Say-NG (M 'このウィンドウは管理者権限で実行されています。管理者権限で実行されるアプリには、Windows が iPhone の GPU の設定を渡しません。' 'This window runs as administrator. Windows does not pass the iPhone GPU settings to programs that run as administrator.')
            Say-Info (M 'このウィンドウを閉じて、「管理者として実行」を使わずに Fix-PocketGPU.cmd をダブルクリックし直してください。' 'Close this window and double-click Fix-PocketGPU.cmd again, without "Run as administrator".')
            $notes += (M '管理者権限で実行していた' 'ran as administrator'); $state = 'unfixed'
        }
        if (Test-UacOff) {
            Say-Warn (M 'ユーザーアカウント制御が無効のため、すべてのアプリが管理者権限で実行されます。この状態では iPhone の GPU を認識できません。' 'User Account Control is off, so every program runs as administrator. The iPhone GPU cannot be seen this way.')
            Say-Info (M 'Windows のセキュリティの設定なので、このツールでは変更しません。「ユーザー アカウント制御設定の変更」で自分で有効にして、PC を再起動してください。' 'This is a Windows security setting, so this tool does not change it. Turn it on yourself in "Change User Account Control settings" and restart the PC.')
            $notes += (M 'ユーザーアカウント制御が無効' 'UAC is off'); if ($state -eq 'ok') { $state = 'warn' }
        }
        if (-not (Test-AnsiPath $script:KitDir)) {
            Say-Warn (M 'このフォルダの場所に、Windows の言語設定で表示できない文字が含まれています。ドライバを読み込めないことがあります。' 'The path of this folder has characters that the Windows language setting cannot show. The driver may fail to load.')
            Say-Info (M 'フォルダごと C:\PocketGPU に移動してから、もう一度起動してください（このツールは移動しません）。' 'Move the whole folder to C:\PocketGPU and open it again. This tool does not move it.')
            $notes += (M 'フォルダの場所に ANSI で表せない文字' 'folder path has non-ANSI characters'); if ($state -eq 'ok') { $state = 'warn' }
        }
        Add-Result 'pc' $state $t (Format-List2 $notes)
    }
}

# ==== 2. キットのファイル ====
if (Want 'files') {
    Say-Step 2 $script:Total (M 'キットのファイル' 'Kit files')
    $t = M 'キットのファイル' 'Kit files'
    $bad = @(Test-KitFiles)
    if ($bad.Count -eq 0) {
        Say-OK (M '必要なファイルがそろっていて、壊れていません。' 'All needed files are here and not damaged.')
        Add-Result 'files' 'ok' $t
    } else {
        Say-NG ((M '欠けている・壊れているファイルが {0} 個あります。' '{0} file(s) are missing or damaged.') -f $bad.Count)
        foreach ($b in $bad) { Say-Info ('{0}  ->  {1}' -f $b.P, (Get-WhyText $b.Why)) }
        $names = Format-List2 ($bad | ForEach-Object { $_.P })
        if ($bad | Where-Object { $_.P -like '*iphone_icd.dll' -and ($_.Why -eq 'missing' -or $_.Why -eq 'empty') }) {
            Say-Info (M 'iphone_icd.dll だけが無い・空のときは、ウイルス対策ソフトが隔離した可能性があります（手順 8 を見てください）。' 'If only iphone_icd.dll is missing or empty, antivirus software may have quarantined it (see step 8).')
        }
        $hasIni = Test-Path -LiteralPath (Join-Path $script:KitDir 'tools\update.ini')
        if (-not $hasIni) {
            Say-Info (M 'このキットには更新用の情報（tools\update.ini）が無いので、自動では取り直せません。' 'This kit has no update information (tools\update.ini), so it cannot be re-downloaded automatically.')
            Say-Info (M '配られた zip からキットを展開し直すか、Discord の #get-kit で /kit と入力して手順を見てください。' 'Extract the kit from the zip again, or type /kit in #get-kit on Discord and follow the steps.')
            Add-Result 'files' 'unfixed' $t ((M '欠け・壊れ：' 'missing/damaged: ') + $names + (M '（update.ini が無く取り直せない）' ' (no update.ini, cannot re-download)'))
        } else {
            Say-Info (M '直し方：Update-PocketGPU.cmd と同じ仕組みで、違うファイルだけを取り直します。ダウンロードしたファイルは SHA-256 で確かめます。古い物は update-backup\ に残ります。' 'Fix: re-downloads only the different files, the same way as Update-PocketGPU.cmd. Each download is checked with SHA-256. Old files stay in update-backup\.')
            if (Offer (M 'キットのファイルを取り直しますか？' 'Re-download the kit files?')) {
                $u = Invoke-KitUpdate
                if (-not $u.Ran) {
                    Say-NG (M '更新の道具（tools\update.ps1）が使えません。キットを展開し直してください。' 'The updater (tools\update.ps1) cannot be used. Extract the kit again.')
                }
                $bad2 = @(Test-KitFiles)
                if ($bad2.Count -eq 0) {
                    Say-OK (M '直りました。' 'Fixed.')
                    Add-Result 'files' 'fixed' $t ((M '取り直した：' 're-downloaded: ') + $names)
                } else {
                    Say-NG (M 'まだ直っていないファイルがあります：' 'Still not fixed: ')
                    foreach ($b in $bad2) { Say-Info ('{0}  ->  {1}' -f $b.P, (Get-WhyText $b.Why)) }
                    $why = $(if ($u.Ran) { 'update.ps1 exit ' + $u.Exit } else { 'update.ps1 unusable' })
                    Add-Result 'files' 'unfixed' $t ((M '取り直しても直らない：' 'not fixed after re-download: ') + (Format-List2 ($bad2 | ForEach-Object { $_.P })) + ' (' + $why + ')')
                }
            } else {
                Add-Result 'files' 'unfixed' $t ((M '欠け・壊れ（取り直しを選ばなかった）：' 'missing/damaged (re-download not chosen): ') + $names)
            }
        }
    }
}

# ==== 3. Vulkan ローダーと起動の入口 ====
if (Want 'loader') {
    Say-Step 3 $script:Total (M 'Vulkan ローダーと起動の入口' 'Vulkan loader and launchers')
    $t = M 'Vulkan ローダーと起動の入口' 'Vulkan loader and launchers'
    $lv = Get-VulkanLoaderVersion
    $old = $false
    $driverNote = ''
    if (-not $lv) {
        Say-NG (M 'Vulkan ランタイムの vulkan-1.dll がありません。' 'The Vulkan runtime vulkan-1.dll is missing.')
        Say-Info (M 'GPU のドライバを更新してください。ドライバに含まれています。このツールは、ドライバを入れません。' 'Update the GPU driver. The runtime comes with it. This tool does not install drivers.')
        Offer-Page (Get-DriverPage) (M 'GPU のドライバのページを開きますか？' 'Open the GPU driver page?')
        $driverNote = M 'vulkan-1.dll が無い（GPU ドライバの更新が必要）' 'vulkan-1.dll missing (update the GPU driver)'
    } else {
        $old = $lv -lt $script:LoaderMin
        if ($old) { Say-Warn ((M 'Vulkan ローダーが古いです：{0}。1.3.234 より前のローダーは、VK_ADD_DRIVER_FILES と VK_LOADER_DRIVERS_SELECT を読みません。起動の入口が VK_ICD_FILENAMES も付ける必要があります。' 'The Vulkan loader is old: {0}. Loaders older than 1.3.234 ignore VK_ADD_DRIVER_FILES and VK_LOADER_DRIVERS_SELECT. The launchers must also set VK_ICD_FILENAMES.') -f $lv) }
        else { Say-OK ((M 'Vulkan ローダー {0}' 'Vulkan loader {0}') -f $lv) }
    }
    $stale = @()
    $unknown = @()
    foreach ($n in (Get-LauncherNames)) {
        $st = Test-Launcher $n
        $script:Log.Add('      launcher ' + $n + ' = ' + $st)
        switch ($st) {
            'ok' { Say-OK ((M '{0}：VK_ICD_FILENAMES も渡します。' '{0}: also passes VK_ICD_FILENAMES.') -f $n) }
            'missing' { Say-NG ((M '{0}：ありません。' '{0}: not found.') -f $n); $stale += $n }
            'stale' {
                if ($old) { Say-NG ((M '{0}：VK_ICD_FILENAMES を渡さない古い入口です。このローダーでは iPhone の GPU が見えません。' '{0}: an old launcher that does not set VK_ICD_FILENAMES. This loader cannot see the iPhone GPU with it.') -f $n); $stale += $n }
                else { Say-Warn ((M '{0}：VK_ICD_FILENAMES を渡さない古い入口です。今のローダーでは動きますが、古いローダーの PC では動きません。' '{0}: an old launcher that does not set VK_ICD_FILENAMES. It works with this loader but not with an old one.') -f $n); $unknown += $n }
            }
            'target-missing' { Say-NG ((M '{0}：指しているファイルがありません。手順 2 を見てください。' '{0}: points to a file that is missing. See step 2.') -f $n) }
            default { Say-Warn ((M '{0}：動かして確かめることができませんでした。' '{0}: could not be run to check.') -f $n) }
        }
    }
    $fixedNames = @()
    $failed = @()
    if ($stale.Count -gt 0) {
        $names = Format-List2 $stale
        Say-Info (M '直し方：このツールに入っている新しい入口に入れ替えます。前の物は fix-backup\ に残します。' 'Fix: replace them with the new launchers that come with this tool. The old ones are kept in fix-backup\.')
        foreach ($n in $stale) {
            if (-not (Get-Template $n)) { Say-NG ((M '{0} の新しい物（tools\fix-data\）がありません。GitHub から取ったフォルダの kit\ の中身を、キットに重ねてください。' 'The new {0} (tools\fix-data\) is missing. Copy the contents of the kit\ folder from GitHub over your kit.') -f $n); $failed += $n; continue }
            if (Offer ((M '{0} を新しい物に入れ替えますか？' 'Replace {0} with the new one?') -f $n)) {
                $r = Replace-Launcher $n
                if ($r.Ok) {
                    $after = Test-Launcher $n
                    if ($after -eq 'ok') { Say-OK ((M '入れ替えました。確かめ直して、VK_ICD_FILENAMES が渡ることを確認しました。' 'Replaced. Checked again: VK_ICD_FILENAMES is passed now.') -f $n); $fixedNames += $n }
                    else { Say-NG ((M '入れ替えましたが、まだ確認できません（{0}）。' 'Replaced, but still not confirmed ({0}).') -f $after); $failed += $n }
                    if ($r.Backup) { Say-Info ((M '前の物：{0}' 'Old file: {0}') -f $r.Backup) }
                } else { Say-NG ((M '入れ替えられませんでした：{0}' 'Could not replace it: {0}') -f $r.Why); $failed += $n }
            } else { $failed += $n }
        }
    }
    if ($old -and $lv) {
        Say-Info (M 'ローダーそのものは、GPU のドライバを更新すると新しくなります（このツールは入れません）。' 'The loader itself becomes new when you update the GPU driver. This tool does not install it.')
        Offer-Page (Get-DriverPage) (M 'GPU のドライバのページを開きますか？' 'Open the GPU driver page?')
    }
    if (-not $lv) { Add-Result 'loader' 'unfixed' $t $driverNote }
    elseif ($failed.Count -gt 0) { Add-Result 'loader' 'unfixed' $t ((M '古い入口が残っている：' 'old launchers remain: ') + (Format-List2 $failed) + $(if ($old) { ' (loader ' + $lv + ')' } else { '' })) }
    elseif ($fixedNames.Count -gt 0) { Add-Result 'loader' 'fixed' $t ((M '入れ替えた：' 'replaced: ') + (Format-List2 $fixedNames) + $(if ($old) { ' (loader ' + $lv + ')' } else { '' })) }
    elseif ($old) { Add-Result 'loader' 'warn' $t ((M 'ローダーが古い {0}。入口は対応済み' 'old loader {0}; launchers already handle it') -f $lv) }
    elseif ($unknown.Count -gt 0) { Add-Result 'loader' 'warn' $t ((M '古い入口：' 'old launchers: ') + (Format-List2 $unknown)) }
    else { Add-Result 'loader' 'ok' $t ('loader ' + $lv) }
}

# ==== 4. Apple のソフトとサービス ====
$apple = $null
if (Want 'apple') {
    Say-Step 4 $script:Total (M 'Apple のソフトとサービス' 'Apple software and service')
    $t = M 'Apple のサービス' 'Apple service'
    $apple = Get-AppleSoftware
    $kind = $(if ($apple.AppleDevices) { 'Apple Devices' } elseif ($apple.Service) { 'iTunes / Apple Mobile Device Support' } elseif ($apple.StoreITunes) { 'iTunes (Store)' } else { '-' })
    $script:Ctx.AppleKind = $kind
    if (Test-MuxAnswers) {
        Say-OK (M 'Apple のサービスが実行されています（Apple Mobile Device Service）。' 'The Apple service is running (Apple Mobile Device Service).')
        $script:Ctx.MuxOk = $true
        Add-Result 'apple' 'ok' $t $kind
    } else {
        Say-NG $(if (Test-MuxListening) { M 'Apple のサービスの口は開いていますが、Apple のサービスとして答えません。' 'The port is open but does not answer as the Apple service.' } else { M 'Apple のサービスが実行されていません。' 'The Apple service is not running.' })
        $done = $false
        $note = ''
        if ($apple.Service) {
            $svc = $apple.Service
            $status = [string]$svc.Status
            $startType = $(if ($svc.PSObject.Properties['StartType']) { [string]$svc.StartType } else { '' })
            Say-Info ((M 'Apple Mobile Device Service の状態：{0}' 'Apple Mobile Device Service state: {0}') -f $status)
            if ($startType -eq 'Disabled') {
                Say-Info (M 'スタートアップの種類が「無効」です。Windows の設定なので、このツールでは変えません。「サービス」（services.msc）で「手動」か「自動」にしてください。' 'The startup type is Disabled. This is a Windows setting, so this tool does not change it. Set it to Manual or Automatic in Services (services.msc).')
                $note = M 'サービスが無効' 'service disabled'
            } else {
                $q = $(if ($status -eq 'Running') { M 'Apple Mobile Device Service は動いていますが、応答しません。再起動しますか？' 'Apple Mobile Device Service is running but does not answer. Restart it?' } else { M 'Apple Mobile Device Service を開始しますか？' 'Start Apple Mobile Device Service?' })
                if (Offer $q) {
                    $ok = $false
                    $denied = $false
                    if ($env:PGPU_TEST_SERVICE_DENIED -eq '1') { $denied = $true }
                    elseif ($env:PGPU_TEST_SERVICE_START) { [void](Invoke-Launch 'service' 'Apple Mobile Device Service'); $ok = $true }
                    elseif ($env:PGPU_TEST_APPLE_SW) { $denied = $true }   # 試験で偽のサービスのとき、本物のサービスには触らない
                    else {
                        try {
                            if ($status -eq 'Running') { Restart-Service -Name 'Apple Mobile Device Service' -ErrorAction Stop } else { Start-Service -Name 'Apple Mobile Device Service' -ErrorAction Stop }
                            $ok = $true
                        } catch {
                            $m = $_.Exception.Message
                            $inner = $_.Exception.InnerException
                            if ($m -match 'Cannot open|Access is denied|アクセスが拒否' -or ($inner -and $inner.PSObject.Properties['NativeErrorCode'] -and $inner.NativeErrorCode -eq 5)) { $denied = $true }
                            else { Say-NG ((M '開始できませんでした：' 'Could not start it: ') + $m) }
                        }
                    }
                    if ($denied) {
                        Say-NG (M 'サービスの開始には、管理者の権限が必要です。このウィンドウは通常の権限なので、開始できませんでした。' 'Starting a service needs administrator rights. This window is a normal one, so it could not start it.')
                        Say-Info (M 'スタートメニューで「Windows PowerShell」を右クリックして「管理者として実行」を選び、次の 1 行を実行してください。そのあと、このツールを通常のウィンドウでもう一度実行してください。' 'Right-click Windows PowerShell in the Start menu, choose Run as administrator, and run the next line. Then run this tool again in a normal window.')
                        Say-Info "    Start-Service -Name 'Apple Mobile Device Service'"
                        $note = M 'サービスの開始に管理者権限が必要（案内を出した）' 'starting the service needs administrator rights (instructions shown)'
                        if ($apple.AppleDevices -or $apple.StoreITunes) {
                            $o2 = Try-OpenAppleApp $apple
                            if ($o2.Done) { $done = $true }
                        }
                    } elseif ($ok) {
                        Say-Info (M '開始を頼みました。最大 20 秒待ちます' 'Asked it to start. Waiting up to 20 seconds')
                        if (Wait-Mux 20) { $done = $true }
                    }
                } else { $note = M '開始を選ばなかった' 'start not chosen' }
            }
        } elseif ($apple.AppleDevices -or $apple.StoreITunes) {
            $o2 = Try-OpenAppleApp $apple
            if ($o2.Done) { $done = $true } else { $note = $o2.Note }
        } else {
            Say-Info (M 'Apple Devices も iTunes も入っていません。Windows が USB で iPhone と通信するために、どちらかが必要です。このツールは入れません。' 'Neither Apple Devices nor iTunes is installed. Windows needs one of them to talk to the iPhone over USB. This tool does not install them.')
            Offer-Page 'ms-windows-store://pdp/?ProductId=9NP83LWLPZ9K' (M 'Microsoft Store で Apple Devices のページを開きますか？' 'Open the Apple Devices page in the Microsoft Store?')
            $note = M 'Apple Devices も iTunes も入っていない' 'neither Apple Devices nor iTunes is installed'
        }
        # 口を別のプログラムが使っていないか（Apple のサービスが動かない原因になる）
        if (-not $done) {
            $allOwners = @(Get-TcpOwners $script:MuxPort 'Listen')
            $own = @($allOwners | Where-Object { $_.Name -notmatch '^(AppleMobileDeviceService|AppleDevices|iTunes|usbmuxd)' })
            if ($allOwners.Count -gt 0 -and $own.Count -eq 0) { Say-Info (M 'Apple のサービスが動いていますが、答えません。PC を再起動するか、Apple Devices（または iTunes）を入れ直してください。' 'The Apple service is running but does not answer. Restart the PC, or reinstall Apple Devices (or iTunes).'); $note = ($note + ' ' + (M 'Apple のサービスが答えない' 'the Apple service does not answer')).Trim() }
            if ($own.Count -gt 0) {
                Say-Warn ((M '口 {0} を、Apple のサービスではないプログラムが使っています：' 'Port {0} is used by a program that is not the Apple service: ') -f $script:MuxPort)
                foreach ($o in $own) { Say-Info (Get-ProcLabel $o.Name $o.Pid $o.Exe) }
                Say-Info (M 'そのプログラムを終了すると、Apple のサービスが口を使えるようになります。ここで止めるか、自分で閉じてください。' 'Closing it lets the Apple service use the port. Stop it here, or close it yourself.')
                $stoppedOwners = 0
                foreach ($o in $own) {
                    if (Offer ((M '{0} (PID {1}) を止めますか？' 'Stop {0} (PID {1})?') -f $o.Name, $o.Pid)) { if (Stop-ByPid $o.Pid) { $stoppedOwners++ } }
                }
                if ($stoppedOwners -gt 0) {
                    Say-Info (M '口が空くと Apple のサービスが始まります。最大 20 秒待ちます' 'The Apple service starts once the port is free. Waiting up to 20 seconds')
                    if (Wait-Mux 20) { $done = $true }
                }
                if (-not $done) {
                    if (@(Get-TcpOwners $script:MuxPort 'Listen').Count -gt 0) { $note = ($note + ' ' + ((M '口 {0} を別のプログラムが使っている' 'port {0} is used by another program') -f $script:MuxPort)).Trim() }
                    else { $note = ($note + ' ' + ((M '口 {0} は空いたが、Apple のサービスが始まらない' 'port {0} is free now, but the Apple service did not start') -f $script:MuxPort)).Trim() }
                }
            }
        }
        if ($done -or (Test-MuxAnswers)) {
            Say-OK (M 'Apple のサービスが実行されるようになりました。' 'The Apple service is running now.')
            $script:Ctx.MuxOk = $true
            Add-Result 'apple' 'fixed' $t ((M '開始した（' 'started (') + $kind + ')')
        } else {
            Add-Result 'apple' 'unfixed' $t ($note.Trim())
        }
    }
}
if ($script:Ctx.MuxOk -eq $false -and -not (Want 'apple')) { $script:Ctx.MuxOk = Test-MuxAnswers }

# ==== 5. 動いている PocketGPU のツール ====
if (Want 'tools') {
    Say-Step 5 $script:Total (M '動いている PocketGPU のツール' 'Running PocketGPU tools')
    $t = M '動いているツール' 'Running tools'
    $procs = @(Get-KitProcs)
    if ($procs.Count -eq 0) {
        Say-OK (M 'このキットのツールは動いていません。' 'No tool of this kit is running.')
        Add-Result 'tools' 'ok' $t (M '動いていない' 'none running')
    } else {
        foreach ($g in ($procs | Group-Object { $_.Name.ToLowerInvariant() })) { Say-Info ('{0} x {1}' -f $g.Group[0].Name, $g.Count) }
        $dups = @($procs | Group-Object { $_.Name.ToLowerInvariant() } | Where-Object { $_.Count -gt 1 })
        if ($dups.Count -eq 0) {
            Say-OK (M '二重に動いているツールはありません。' 'No tool is running twice.')
            Add-Result 'tools' 'ok' $t (M '二重なし' 'no duplicates')
        } else {
            $stopped = @()
            $left = @()
            foreach ($g in $dups) {
                Say-NG ((M '{0} が {1} 個、二重に動いています。同じ iPhone を取り合って、つながらなくなります。' '{0} is running {1} times. They fight over the same iPhone and the connection fails.') -f $g.Group[0].Name, $g.Count)
                # 一番新しい 1 つを残して、古い物を止める
                $sorted = @($g.Group | Sort-Object CreationDate -Descending)
                Say-Info ((M '直し方：一番新しい PID {0} を残して、古い物を止めます。' 'Fix: keep the newest, PID {0}, and stop the older ones.') -f $sorted[0].ProcessId)
                foreach ($p in ($sorted | Select-Object -Skip 1)) {
                    if (Offer ((M '古い {0} (PID {1}) を止めますか？' 'Stop the older {0} (PID {1})?') -f $p.Name, $p.ProcessId)) {
                        if (Stop-ByPid ([int]$p.ProcessId)) { Say-OK ((M '止めました：PID {0}' 'Stopped: PID {0}') -f $p.ProcessId); $stopped += ('{0} PID {1}' -f $p.Name, $p.ProcessId) }
                        else { $left += ('{0} PID {1}' -f $p.Name, $p.ProcessId) }
                    } else { $left += ('{0} PID {1}' -f $p.Name, $p.ProcessId) }
                }
            }
            $still = @(Get-KitProcs | Group-Object { $_.Name.ToLowerInvariant() } | Where-Object { $_.Count -gt 1 })
            if ($still.Count -eq 0) { Add-Result 'tools' 'fixed' $t ((M '二重のツールを止めた：' 'stopped duplicates: ') + (Format-List2 $stopped)) }
            else { Add-Result 'tools' 'unfixed' $t ((M '二重のまま：' 'still duplicated: ') + (Format-List2 $left)) }
        }
    }
}

# ==== 6. iPhone の USB とアプリ ====
if (Want 'iphone') {
    Say-Step 6 $script:Total (M 'iPhone の USB とアプリ' 'iPhone USB and app')
    $t = M 'iPhone' 'iPhone'
    if (-not $script:Ctx.MuxOk -and -not (Test-MuxAnswers)) {
        Say-Warn (M 'Apple のサービスが動いていないので、確かめられません。手順 4 を先に直してください。' 'The Apple service is not running, so this cannot be checked. Fix step 4 first.')
        Add-Result 'iphone' 'skip' $t (M 'Apple のサービスが動いていない' 'Apple service not running')
    } else {
        $devs = @(Get-MuxDevices)
        $usb = @($devs | Where-Object { $_.Type -eq 'USB' })
        if ($usb.Count -eq 0) {
            Say-NG (M 'iPhone が USB で認識されていません。' 'The iPhone is not seen over USB.')
            if (@($devs | Where-Object { $_.Type -eq 'Network' }).Count -gt 0) { Say-Info (M 'Wi-Fi 経由でだけ認識されています。USB ケーブルでつないでください。' 'It is only seen over Wi-Fi. Connect it with a USB cable.') }
            $pnp = Get-UsbApplePhones
            if ($pnp.Count -gt 0) {
                if (@($pnp | Where-Object { $_.ConfigManagerErrorCode -ne 0 }).Count -gt 0) { Say-Info (M 'Windows は iPhone を認識していますが、Apple の USB ドライバが動いていません。Apple Devices か iTunes を入れ直してください。' 'Windows sees the iPhone, but the Apple USB driver is not working. Reinstall Apple Devices or iTunes.') }
                else {
                    Say-Info (M 'Windows は iPhone を認識していますが、Apple のサービスはまだ認識していません。iPhone のロックを解除して、「このコンピュータを信頼しますか？」と出たら「信頼」をタップしてください。' 'Windows sees the iPhone, but the Apple service does not yet. Unlock the iPhone and tap Trust if it asks "Trust This Computer?".')
                }
            } else { Say-Info (M 'iPhone を USB ケーブルでつないで、ロックを解除してください。充電専用のケーブルではつながりません。' 'Plug the iPhone in with USB and unlock it. A charge-only cable does not work.') }
            Add-Result 'iphone' 'unfixed' $t (M 'USB で認識されていない（案内を出した）' 'not seen over USB (instructions shown)')
        } else {
            $dev = $usb[0]
            $script:Ctx.Dev = $dev
            if ($usb.Count -gt 1) { Say-Warn (M 'Apple 製のデバイスが 2 台以上つながっています。最初の 1 台を使います。' 'More than one Apple device is connected. The first one is used.') }
            Say-OK (M 'iPhone が USB で認識されています。' 'The iPhone is connected over USB.')
            $h = Test-Receiver $dev.Id 3000
            if ($h.Ok) {
                Say-OK ((M 'iPhone のアプリが応答しました：' 'The iPhone app answered: ') + $h.Name)
                Add-Result 'iphone' 'ok' $t $h.Name
            } else {
                switch ($h.Why) {
                    'timeout' {
                        Say-NG (M 'iPhone のアプリは起動していますが、返事がありません。ほかのプログラムが iPhone の GPU を使っている（受け手は一度に 1 人しか相手をしません）か、前の接続が残っています。' 'The iPhone app is open but does not answer. Another program is using the iPhone GPU (the receiver serves one program at a time), or an old connection remains.')
                        $holders = @(Get-DriverHolders)
                        if ($holders.Count -eq 0) {
                            Say-Info (M 'iPhone の GPU を使っている PC のプログラムは見つかりませんでした。iPhone のアプリを一度閉じて（上にスワイプして終了）、開き直してください。前の接続が iPhone 側に残っていることがあります。' 'No program on this PC is using the iPhone GPU. Close the iPhone app (swipe it away) and open it again. The iPhone may still hold an old connection.')
                            Add-Result 'iphone' 'unfixed' $t (M '受け手が応答しない（PC に握っているプログラムは無い。iPhone のアプリを開き直す案内を出した）' 'receiver does not answer (no program on the PC holds it; told to reopen the iPhone app)')
                        } else {
                            Say-Info (M 'iPhone のドライバ（iphone_icd.dll）を読み込んでいるプログラム：' 'Programs that loaded the iPhone driver (iphone_icd.dll):')
                            foreach ($x in $holders) { Say-Info (Get-ProcLabel $x.Name $x.Pid $x.Exe) }
                            Say-Info (M '直し方：使っていない物を止めると、iPhone の GPU が空きます。ゲームなどを使っている最中なら、止めずに自分で閉じてください。' 'Fix: stopping the ones you no longer use frees the iPhone GPU. If you are in a game, close it yourself instead.')
                            $stoppedAny = @()
                            foreach ($x in $holders) {
                                if (Offer ((M '{0} (PID {1}) を止めますか？ 保存していない内容は失われます。' 'Stop {0} (PID {1})? Unsaved work is lost.') -f $x.Name, $x.Pid)) {
                                    if (Stop-ByPid $x.Pid) { Say-OK ((M '止めました：PID {0}' 'Stopped: PID {0}') -f $x.Pid); $stoppedAny += ('{0} PID {1}' -f $x.Name, $x.Pid) }
                                }
                            }
                            $ok2 = $null
                            if ($stoppedAny.Count -gt 0) {
                                for ($i = 0; $i -lt 8; $i++) { $ok2 = Test-Receiver $dev.Id 2000; if ($ok2.Ok) { break }; Start-Sleep -Seconds 1 }
                            }
                            if ($ok2 -and $ok2.Ok) {
                                Say-OK ((M 'iPhone のアプリが応答しました：' 'The iPhone app answered: ') + $ok2.Name)
                                Add-Result 'iphone' 'fixed' $t ((M '握っていたプログラムを止めた：' 'stopped the holder: ') + (Format-List2 $stoppedAny))
                            } else {
                                Add-Result 'iphone' 'unfixed' $t $(if ($stoppedAny.Count -gt 0) { M '止めても応答しない。iPhone のアプリを開き直す必要がある' 'still no answer after stopping; reopen the iPhone app' } else { M '握っているプログラムを止めなかった' 'the holder was not stopped' })
                            }
                        }
                    }
                    'result' {
                        if ($h.Result -eq -9) {
                            Say-NG (M 'iPhone のアプリとこのキットの版が合いません（受け手が古い・または新しい）。' 'The iPhone app and this kit are on different versions.')
                            $ipa = $(if ($script:AppVersion) { 'RpcServer-' + $script:AppVersion + '.ipa' } else { 'RpcServer-*.ipa' })
                            Say-Info ((M 'このキットの {0} を AltStore か Sideloadly で入れ直してください。このツールは iPhone にアプリを入れません。' 'Install {0} from this kit again with AltStore or Sideloadly. This tool does not install apps on the iPhone.') -f $ipa)
                            Say-Info (M 'キットが古いときは、先に Update-PocketGPU.cmd で更新してください。' 'If the kit is old, run Update-PocketGPU.cmd first.')
                            Add-Result 'iphone' 'unfixed' $t ((M 'アプリとキットの版が違う（キット {0}）' 'app and kit versions differ (kit {0})') -f $(if ($script:AppVersion) { $script:AppVersion } else { '?' }))
                        } else {
                            Say-NG ((M 'iPhone のアプリが拒否しました。コード {0}' 'The iPhone app refused. Code {0}') -f $h.Result)
                            Add-Result 'iphone' 'unfixed' $t ((M 'アプリが拒否（コード {0}）' 'app refused (code {0})') -f $h.Result)
                        }
                    }
                    default {
                        Say-NG (M 'iPhone のアプリが応答しません。' 'The iPhone app does not answer.')
                        Say-Info (M 'iPhone で PocketGPU のアプリを開いて、前面に表示したまま、画面をつけておいてください。バックグラウンドでは GPU を使えません。「開始」ボタンがあれば押してください。' 'Open the PocketGPU app on the iPhone and keep it in front with the screen on. It cannot use the GPU in the background. Tap Start if the app shows it.')
                        Say-Info (M '無料の Apple ID で入れたアプリは 7 日で開けなくなります。そのときは入れ直してください。' 'With a free Apple ID the app stops opening after 7 days. Install it again then.')
                        if ($apple -and $apple.AltServer -and -not $apple.AltServerRunning) { Say-Info (M 'AltServer が起動していません。AltStore で入れ直すときは、スタートメニューから AltServer を起動してください。' 'AltServer is not running. To reinstall with AltStore, start AltServer from the Start menu.') }
                        Add-Result 'iphone' 'unfixed' $t (M 'アプリが応答しない（開く・入れ直す案内を出した）' 'app does not answer (instructions shown)')
                    }
                }
            }
        }
    }
}

# ==== 7. Android の受け手とアプリの口 ====
if (Want 'android') {
    Say-Step 7 $script:Total (M 'Android の受け手' 'Android receiver')
    $t = M 'Android' 'Android'
    $port = $(if ($env:PGPU_PORT) { [int]$env:PGPU_PORT } else { 50300 })
    $adb = Find-Adb
    if (-not (Test-Path -LiteralPath (Join-Path $script:KitDir 'android\_common.cmd'))) {
        Say-Info (M 'このキットには Android 用のファイルがありません。Android を使わないなら問題ありません。' 'This kit has no Android files. That is fine if you do not use Android.')
        Add-Result 'android' 'skip' $t (M 'Android 用のファイルが無い' 'no Android files')
    } elseif (-not $adb) {
        Say-Info (M 'adb.exe が見つかりません。Android を使わないなら問題ありません。使うときは、Google の SDK Platform-Tools を展開して、android\README_android.txt の手順 2 を行ってください。' 'adb.exe was not found. That is fine if you do not use Android. If you do, unzip Google SDK Platform-Tools and do step 2 in android\README_android.txt.')
        Add-Result 'android' 'skip' $t (M 'adb が無い（Android 未使用）' 'no adb (Android not used)')
    } else {
        Say-Info ((M 'adb：{0}' 'adb: {0}') -f $adb)
        [void](Invoke-Adb $adb 'start-server')
        $dl = Invoke-Adb $adb 'devices'
        $devices = @()
        $others = @()
        foreach ($l in ($dl.Out -split "`r?`n" | Select-Object -Skip 1)) {
            if ($l -match '^(\S+)\s+(\S+)') { if ($Matches[2] -eq 'device') { $devices += $Matches[1] } else { $others += ($Matches[1] + ' ' + $Matches[2]) } }
        }
        $fwdAll = Get-AdbForwards $adb
        $hasFwd = $fwdAll.ContainsKey([string]$port)
        $leftApp = @(Get-AppLeftoverPorts $fwdAll)
        $leftAll = @()
        if ($hasFwd) { $leftAll += $port }
        $leftAll += $leftApp
        $serial = $(if ($env:ANDROID_SERIAL) { $env:ANDROID_SERIAL } elseif ($devices.Count -eq 1) { $devices[0] } else { '' })
        if ($devices.Count -eq 0) {
            if ($others | Where-Object { $_ -match 'unauthorized' }) {
                Say-NG (M 'スマートフォンはつながっていますが、許可されていません。ロックを解除して、「USB デバッグを許可しますか？」で「許可」をタップしてください。' 'The phone is connected but not allowed. Unlock it and tap Allow on "Allow USB debugging?".')
                Add-Result 'android' 'unfixed' $t (M '許可されていない（案内を出した）' 'not allowed (instructions shown)')
            } elseif ($leftAll.Count -gt 0) {
                Say-NG ((M 'スマートフォンはつながっていないのに、口 {0} の adb forward が残っています。' 'No phone is connected, but the adb forward for port(s) {0} remains.') -f ($leftAll -join ' '))
                Say-Info (M '直し方：残っている adb forward を外します（adb forward --remove）。' 'Fix: remove the leftover adb forward (adb forward --remove).')
                if (Offer (M '残っている adb forward を外しますか？' 'Remove the leftover adb forward?')) {
                    foreach ($lp in $leftAll) { [void](Invoke-Adb $adb ('forward --remove tcp:' + $lp)) }
                    $f2 = Get-AdbForwards $adb
                    $still = @($leftAll | Where-Object { $f2.ContainsKey([string]$_) })
                    if ($still.Count -eq 0) { Say-OK (M '外しました。' 'Removed.'); Add-Result 'android' 'fixed' $t (M '残っていた adb forward を外した' 'removed a leftover adb forward') }
                    else { Add-Result 'android' 'unfixed' $t (M 'adb forward を外せなかった' 'could not remove the adb forward') }
                } else { Add-Result 'android' 'unfixed' $t (M '古い adb forward が残っている' 'a leftover adb forward remains') }
            } else {
                Say-Info (M 'Android のスマートフォンはつながっていません。Android を使わないなら問題ありません。' 'No Android phone is connected. That is fine if you do not use Android.')
                Add-Result 'android' 'skip' $t (M '端末なし（Android 未使用）' 'no phone (Android not used)')
            }
        } elseif (-not $serial) {
            Say-NG (M '2 台以上つながっています。使う 1 台を ANDROID_SERIAL で指定してから、もう一度実行してください（adb devices で番号が分かります）。' 'More than one device is connected. Set ANDROID_SERIAL to the one you use and run this again (adb devices shows the numbers).')
            Add-Result 'android' 'unfixed' $t (M '端末が複数。ANDROID_SERIAL の指定が要る' 'several devices; ANDROID_SERIAL is needed')
        } else {
            $sa = '-s ' + $serial + ' '
            $local = Join-Path $script:KitDir 'android\server_aarch64'
            $localHash = $(if (Test-Path -LiteralPath $local) { (Get-FileHash -LiteralPath $local -Algorithm SHA256).Hash.ToLowerInvariant() } else { '' })
            $a1 = Get-AndroidProblems $adb $serial $port $localHash $false
            $problems = @($a1.Problems)
            $conflict = @($a1.Conflict)
            $hello = $a1.Hello
            if ($problems.Count -eq 0) { Add-Result 'android' 'ok' $t $(if ($a1.Note) { $a1.Note } else { 'ok' }) }
            else {
                $acts = @()
                $okAll = $true
                # 1) 口を使っているプログラムをどける
                if ($problems -contains 'portconflict') {
                    foreach ($o in $conflict) {
                        if (Offer ((M '{0} (PID {1}) を止めますか？ 口 {2} を adb が使えるようになります。' 'Stop {0} (PID {1})? Then adb can use port {2}.') -f $o.Name, $o.Pid, $port)) {
                            if (Stop-ByPid $o.Pid) { $acts += ('stopped ' + $o.Name + ' PID ' + $o.Pid); Say-OK ((M '止めました：PID {0}' 'Stopped: PID {0}') -f $o.Pid) }
                        }
                    }
                }
                # 2) 受け手を入れ直す（古い受け手・二重・止まっている）
                $needRestart = ($problems -contains 'norecv') -or ($problems -contains 'dup') -or ($problems -contains 'oldrecv') -or ($problems -contains 'nohello')
                if ($needRestart) {
                    Say-Info (M '直し方：android\start_android.cmd と同じ手順です。スマートフォンの古い受け手を止めて、キットの受け手をコピーして起動し、adb forward をつなぎ直します（スマートフォンには何もインストールしません）。' 'Fix: the same steps as android\start_android.cmd. It stops the old receiver on the phone, copies the kit receiver, starts it and connects adb forward again. Nothing is installed on the phone.')
                    if (Offer (M '受け手を入れ直して、つなぎ直しますか？' 'Restart the receiver and reconnect?')) {
                        $st = Join-Path $script:KitDir 'android\start_android.cmd'
                        $r = Invoke-Native $env:ComSpec ('/c ""' + $st + '""') 90000 @{ ADB = $adb; ANDROID_SERIAL = $serial; PGPU_PORT = [string]$port; PGPU_NOPAUSE = '1' }
                        foreach ($l in @($r.Out -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -Last 4)) { Say-Info $l }
                        if ($r.Exit -ne 0) { Say-NG (M 'start_android.cmd が失敗しました。上の表示を見てください。' 'start_android.cmd failed. See the lines above.'); $okAll = $false }
                        else { $acts += 'restarted the receiver' }
                    } else { $okAll = $false }
                } elseif ($problems | Where-Object { $_ -in 'fwdmissing', 'fwdwrong', 'fwdstale', 'busy', 'portconflict', 'appgpufwd' }) {
                    # 3) adb forward だけやり直す。アプリの中の受け手なら、アプリの版に合う向け方（版が読めなければ今の向け方）で
                    $gpuDest = 'tcp:' + $port
                    if ($a1.AppRecv) {
                        $gpuDest = $(if ($a1.App.GpuForm -ne 'unknown') { $a1.App.GpuDest } else { (Get-AdbForwards $adb)[[string]$port] })
                        if (-not $gpuDest) { $gpuDest = $script:AppGpuTcp }
                    }
                    Say-Info ((M '直し方：adb forward を外して、{0} へつなぎ直します。つながっていた接続は一度切れます。' 'Fix: remove the adb forward and connect it again to {0}. Existing connections are dropped once.') -f $gpuDest)
                    if ($problems -contains 'busy') {
                        $holdersA = @(Get-TcpOwners $port 'Established' -Remote | Where-Object { $_.Name -notmatch '^adb' })
                        foreach ($o in $holdersA) {
                            Say-Info (Get-ProcLabel $o.Name $o.Pid $o.Exe)
                            if (Offer ((M '受け手を握っている {0} (PID {1}) を止めますか？' 'Stop {0} (PID {1}), which holds the receiver?') -f $o.Name, $o.Pid)) { if (Stop-ByPid $o.Pid) { $acts += ('stopped ' + $o.Name + ' PID ' + $o.Pid) } }
                        }
                    }
                    if (Offer (M 'adb forward をやり直しますか？' 'Redo the adb forward?')) {
                        [void](Invoke-Adb $adb ($sa + 'forward --remove tcp:' + $port))
                        $r = Invoke-Adb $adb ($sa + 'forward tcp:' + $port + ' ' + $gpuDest)
                        # 古い adb は localabstract: を断る。そのときは端末の 50060 へ（android_app.cmd と同じ）
                        if ($r.Exit -ne 0 -and $gpuDest -eq $script:AppGpuUsb) {
                            Say-Info (M 'adb が USB 用の口の向け方を断ったので、端末の 50060 へつなぎます。' 'adb refused the USB-only port form, so the GPU port goes to port 50060 on the phone.')
                            $r = Invoke-Adb $adb ($sa + 'forward tcp:' + $port + ' ' + $script:AppGpuTcp)
                        }
                        if ($r.Exit -ne 0) { Say-NG ((M 'adb forward に失敗しました：{0}' 'adb forward failed: {0}') -f $r.Err.Trim()); $okAll = $false } else { $acts += 'redid adb forward' }
                    } else { $okAll = $false }
                }
                # 4) PocketGPU アプリのカメラとマイクの口：アプリの版に合う向け方でつなぎ直す。アプリが無いのに残っている物は外す
                if ($problems -contains 'appleft') {
                    Say-Info (M '直し方：残っている adb forward を外します（adb forward --remove）。' 'Fix: remove the leftover adb forward (adb forward --remove).')
                    if (Offer (M 'PocketGPU アプリの口の、残っている adb forward を外しますか？' 'Remove the leftover adb forward for the PocketGPU app ports?')) {
                        foreach ($lp in @($a1.AppLeft)) { [void](Invoke-Adb $adb ($sa + 'forward --remove tcp:' + $lp)) }
                        $acts += 'removed leftover app forwards'
                    } else { $okAll = $false }
                }
                if ($problems | Where-Object { $_ -in 'appfwdwrong', 'appfwdhalf', 'appfwdstale', 'appportconflict' }) {
                    foreach ($o in @($a1.AppConflict)) {
                        if (Offer ((M '{0} (PID {1}) を止めますか？ 口 {2} を adb が使えるようになります。' 'Stop {0} (PID {1})? Then adb can use port {2}.') -f $o.Name, $o.Pid, $o.Port)) {
                            if (Stop-ByPid $o.Pid) { $acts += ('stopped ' + $o.Name + ' PID ' + $o.Pid); Say-OK ((M '止めました：PID {0}' 'Stopped: PID {0}') -f $o.Pid) }
                        }
                    }
                    $formWord = $(if ($a1.App.Form -eq 'old') { M '古い向け方（アプリ 0.1.1 以前）' 'the old form (app 0.1.1 and older)' } else { M '新しい向け方（アプリ 0.1.2 以上。localabstract）' 'the new form (app 0.1.2 and newer, localabstract)' })
                    Say-Info ((M '直し方：カメラとマイクの adb forward を、{0}でつなぎ直します。アプリは操作しません。' 'Fix: connect the camera and mic adb forward again in {0}. The app itself is not touched.') -f $formWord)
                    if (Offer (M 'カメラとマイクの adb forward をやり直しますか？' 'Redo the camera and mic adb forward?')) {
                        foreach ($sp in @($a1.AppFix)) {
                            $dest = $(if ($a1.App.Form -eq 'old') { $sp.Old } else { $sp.New })
                            [void](Invoke-Adb $adb ($sa + 'forward --remove tcp:' + $sp.Port))
                            $r = Invoke-Adb $adb ($sa + 'forward tcp:' + $sp.Port + ' ' + $dest)
                            if ($r.Exit -ne 0) { Say-NG ((M 'adb forward に失敗しました：{0}' 'adb forward failed: {0}') -f $r.Err.Trim()); $okAll = $false } else { $acts += ('redid ' + $sp.Name + ' forward ' + $dest) }
                        }
                    } else { $okAll = $false }
                }
                # 直ったかを確かめ直す
                $a2 = Get-AndroidProblems $adb $serial $port $localHash $true
                if ($a2.Problems.Count -eq 0) {
                    Say-OK ((M '直りました。' 'Fixed. ') + $(if ($a2.Hello -and $a2.Hello.Ok) { (M '受け手が応答しました：' 'The receiver answered: ') + $a2.Hello.Name } else { '' }))
                    Add-Result 'android' 'fixed' $t ((M '直した：' 'fixed: ') + (Format-List2 $problems) + ' (' + (Format-List2 $acts) + ')')
                } else {
                    Say-NG (M 'まだ直っていません。' 'Not fixed yet.')
                    Say-Info (M 'スマートフォンのロックを解除して画面をつけたまま、android\stop_android.cmd のあとに android\start_android.cmd を実行してみてください。アプリを使っているなら、android\android_app.cmd stop のあとに android\android_app.cmd を実行してください。それでも直らないときは、受け手のログ（android\README_android.txt の「報告」）を送ってください。' 'With the phone unlocked and the screen on, run android\stop_android.cmd and then android\start_android.cmd. If you use the app, run android\android_app.cmd stop and then android\android_app.cmd. If it still fails, send the receiver log (see "Reporting" in android\README_android.txt).')
                    Add-Result 'android' 'unfixed' $t ((M '直らなかった：' 'not fixed: ') + (Format-List2 $a2.Problems))
                }
            }
        }
    }
}

# ==== 8. ドライバ経由で iPhone の GPU が見えるか ====
if (Want 'gpu') {
    Say-Step 8 $script:Total (M 'ドライバ経由で iPhone の GPU が見えるか' 'iPhone GPU through the driver')
    $t = M 'iPhone の GPU' 'iPhone GPU'
    if ($script:Elevated) {
        Say-NG (M '管理者権限で実行されているため、この項目はチェックできません。手順 1 を見てください。' 'Cannot check this while running as administrator. See step 1.')
        Add-Result 'gpu' 'skip' $t (M '管理者権限で実行していた' 'ran as administrator')
    } else {
        $vk = Invoke-VkList
        if ($vk.Ok) {
            foreach ($g in $vk.Gpus) { Say-OK ((M 'ドライバ経由で見えます：' 'Seen through the driver: ') + ($g -replace '^GPU\d+: ', '' -replace '\s+\(vendor .*$', '')) }
            Add-Result 'gpu' 'ok' $t (@($vk.Gpus | ForEach-Object { $_ -replace '^GPU\d+: ', '' -replace '\s+\(vendor .*$', '' }) -join '; ')
        } else {
            Say-NG (M 'ドライバ経由で iPhone の GPU が見えません（PC の GPU しか見えません）。' 'The driver does not show the iPhone GPU (only the PC GPUs are seen).')
            $reasons = @()
            if ($vk.Why -eq 'missing') { $reasons += (M 'vklist.exe が無い（手順 2）' 'vklist.exe is missing (step 2)'); Say-Info (M 'vklist.exe がありません。手順 2 で取り直してください。' 'vklist.exe is missing. Re-download it in step 2.') }
            elseif ($vk.Why -eq 'timeout') { $reasons += (M '30 秒以内に終わらない' 'did not finish in 30 seconds'); Say-Info (M '30 秒以内に終わりませんでした。iPhone のアプリを閉じて、開き直してください。' 'It did not finish in 30 seconds. Close and reopen the iPhone app.') }
            else {
                foreach ($l in @($vk.Output -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -Last 4)) { Say-Info $l }
            }
            # 原因の候補を、ここまでの結果と見比べる
            foreach ($r0 in $script:Results) {
                if ($r0.State -eq 'unfixed' -and $r0.Id -in 'files', 'loader', 'apple', 'iphone') {
                    $reasons += ((M '手順の項目が直っていない：' 'an earlier item is not fixed: ') + $r0.Title)
                    Say-Info ((M 'この項目が直っていません：{0}。先にそれを直すと、見えるようになります。' 'This item is not fixed: {0}. Fixing it first may make the GPU visible.') -f $r0.Title)
                }
            }
            $lv = Get-VulkanLoaderVersion
            if ($lv -and $lv -lt $script:LoaderMin) { Say-Info ((M 'Vulkan ローダーが古い（{0}）。ゲームは pocketgpu.cmd か Run-on-iPhone-GPU.cmd から起動してください。' 'The Vulkan loader is old ({0}). Start games from pocketgpu.cmd or Run-on-iPhone-GPU.cmd.') -f $lv) }
            # ドライバの DLL が読めない原因
            $depNotes = @()
            foreach ($pair in @(@('iphone_icd.dll', (Join-Path $env:WINDIR 'System32')), @('x86\iphone_icd.dll', (Join-Path $env:WINDIR 'SysWOW64')))) {
                $full = Join-Path $script:KitDir $pair[0]
                if (Test-Path -LiteralPath $full) {
                    $mi = @(Get-MissingImports $full $pair[1])
                    if ($mi.Count -gt 0) { $depNotes += ('{0}: {1}' -f $pair[0], ($mi -join ' ')) }
                }
            }
            if ($depNotes.Count -gt 0) {
                Say-NG (M 'ドライバの DLL が必要とするファイルが Windows に無いようです：' 'Windows seems to lack files that the driver DLL needs:')
                foreach ($d in $depNotes) { Say-Info $d }
                $reasons += (M 'ドライバが必要とする DLL が無い' 'a DLL the driver needs is missing')
            }
            # ウイルス対策ソフトの気配
            $signs = @(Get-AvSigns)
            if ($signs.Count -gt 0) {
                Say-Warn (M 'ウイルス対策ソフトが iphone_icd.dll を止めている気配があります：' 'Antivirus software may be blocking iphone_icd.dll:')
                foreach ($sg in $signs) { Say-Info ('- ' + $sg) }
                Say-Info (M '見分け方：Windows セキュリティ > ウイルスと脅威の防止 > 保護の履歴 に iphone_icd.dll があるか。ほかのウイルス対策ソフトを使っているときは、その履歴や隔離の一覧を見てください。' 'How to tell: open Windows Security > Virus and threat protection > Protection history and look for iphone_icd.dll. With another antivirus, look at its history or quarantine list.')
                Say-Info (M '案内：このツールは、除外の追加も、隔離した物の復元もしません（セキュリティの設定だからです）。キットを配った人に相談してから、自分の判断でキットのフォルダを除外するか、隔離した物を復元してください。そのあと、手順 2 で足りないファイルを取り直せます。' 'Note: this tool does not add exclusions or restore quarantined files, because they are security settings. Ask the person who gave you the kit, then decide yourself whether to exclude the kit folder or restore the file. After that, step 2 can re-download missing files.')
                $reasons += (M 'ウイルス対策ソフトが止めている気配' 'signs of antivirus blocking')
                Offer-Page 'windowsdefender://threat/' (M 'Windows セキュリティの「保護の履歴」を開きますか？' 'Open Windows Security protection history?')
            } else {
                $av = @(Get-AvNames)
                if ($av.Count -gt 0) { $script:Log.Add('      antivirus: ' + ($av -join ', ')) }
            }
            if ($reasons.Count -eq 0) {
                Say-Info (M '原因が分かりませんでした。Start-PocketGPU.cmd の結果と pocketgpu-fix-log.txt を Discord で送ってください。' 'The cause is unknown. Send the result of Start-PocketGPU.cmd and pocketgpu-fix-log.txt to us on Discord.')
                $reasons += (M '原因不明' 'cause unknown')
            }
            Add-Result 'gpu' 'unfixed' $t (Format-List2 $reasons)
        }
    }
}

# ==== まとめ ====
$names = @{ ok = (M '問題なし' 'OK'); fixed = (M '直った' 'fixed'); unfixed = (M '直らなかった' 'not fixed'); warn = (M '気をつけたい' 'note'); skip = (M '対象外' 'skipped') }
$cnt = @{ ok = 0; fixed = 0; unfixed = 0; warn = 0; skip = 0 }
foreach ($r in $script:Results) { $cnt[$r.State]++ }

function Get-PcInfo {
    $os = [Environment]::OSVersion.Version
    $win = $(if ($os.Build -ge 22000) { 'Windows 11' } else { 'Windows 10' })
    $gpus = ''
    try { $gpus = (@(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object { $_.Name.Trim() }) -join ', ') } catch { }
    $lv = Get-VulkanLoaderVersion
    $apple = $(if ($script:Ctx.AppleKind) { $script:Ctx.AppleKind } else { '-' })
    return ('{0} (build {1}) 64-bit / GPU: {2} / Vulkan loader: {3} / Apple: {4}' -f $win, $os.Build, $(if ($gpus) { $gpus } else { '?' }), $(if ($lv) { $lv } else { '-' }), $apple)
}
$sum = New-Object System.Collections.Generic.List[string]
$sum.Add('PocketGPU Fix ' + (Get-Date -Format 'yyyy-MM-dd') + ' (fix v' + $script:FixVersion + $(if ($script:CheckOnly) { ', check only' } else { '' }) + ')')
$sum.Add('PC: ' + (Get-PcInfo))
$sum.Add(('Kit: app {0} / proto {1}' -f $(if ($script:AppVersion) { $script:AppVersion } else { '?' }), $script:ProtoVersion))
$sum.Add(((M '結果：直った {0} / 直らなかった {1} / 気をつけたい {2} / 問題なし {3} / 対象外 {4}' 'Result: fixed {0} / not fixed {1} / note {2} / OK {3} / skipped {4}') -f $cnt.fixed, $cnt.unfixed, $cnt.warn, $cnt.ok, $cnt.skip))
foreach ($r in $script:Results) {
    if ($r.State -eq 'ok' -or $r.State -eq 'skip') { continue }
    $sum.Add(('[{0}] {1}{2}' -f $names[$r.State], $r.Title, $(if ($r.Note) { ' - ' + $r.Note } else { '' })))
}
if ($cnt.ok + $cnt.skip -gt 0) {
    $sum.Add((M '問題なし・対象外：' 'OK / skipped: ') + (Format-List2 ($script:Results | Where-Object { $_.State -eq 'ok' -or $_.State -eq 'skip' } | ForEach-Object { $_.Title })))
}
# 載せてはいけない物を最後にもう一度伏せる：ユーザー名・PC 名・キットのフォルダの場所
$summaryText = ($sum -join "`r`n")
if ($env:USERNAME) { $summaryText = $summaryText -replace [regex]::Escape($env:USERNAME), '(user)' }
if ($env:COMPUTERNAME) { $summaryText = $summaryText -replace [regex]::Escape($env:COMPUTERNAME), '(pc)' }
$summaryText = $summaryText.Replace($script:KitDir, '<kit>')

if (-not $NoSummary) {
    Out-Line ''
    Out-Line (M '==== まとめ（Discord に貼れます）====' '==== Summary (you can paste this on Discord) ====') 'Cyan'
    foreach ($l in ($summaryText -split "`r?`n")) { Out-Line $l }
    Out-Line (M '=======================================' '==============================================') 'Cyan'
    Out-Line (M 'この文には、合言葉・ユーザー名・PC 名・フォルダの場所は入っていません。' 'This text has no passphrase, user name, PC name, or folder path.')
    try {
        $sp = Join-Path $script:KitDir 'pocketgpu-fix-summary.txt'
        [IO.File]::WriteAllText($sp, $summaryText + "`r`n", (New-Object Text.UTF8Encoding($true)))
        Out-Line ((M 'ファイルにも書きました：' 'Also saved to: ') + 'pocketgpu-fix-summary.txt')
    } catch { }
    if (Ask-YesNo (M 'この文をクリップボードにコピーしますか？' 'Copy this text to the clipboard?')) {
        if ($env:PGPU_TEST_LAUNCH_LOG) { Add-Content -LiteralPath $env:PGPU_TEST_LAUNCH_LOG -Value 'clipboard' }
        else { try { Set-Clipboard -Value $summaryText; Say-OK (M 'コピーしました。' 'Copied.') } catch { Say-Warn (M 'コピーできませんでした。' 'Could not copy.') } }
    }
}
for ($i = 0; $i -lt $script:Log.Count; $i++) { $script:Log[$i] = $script:Log[$i].Replace($script:KitDir, '<kit>') }
$logPath = Save-Log 'pocketgpu-fix-log.txt'
Out-Line ''
Out-Line ((M 'ログ：' 'Log: ') + $logPath)
if (-not $script:NoPrompt -and $env:PGPU_NOPAUSE -ne '1') { [void](Read-Host (M 'Enter キーを押すと閉じます' 'Press Enter to close')) }
if ($cnt.unfixed -gt 0) { exit 1 }
exit 0
