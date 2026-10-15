# PocketGPU：この PC の様子を読んで、Discord に貼れる txt を 1 つ作る。Check-PC.cmd から呼ぶ。Windows PowerShell 5.1 だけで動く（Python は要らない）。
# 日本語を含むので UTF-8（BOM 付き）で保存する。BOM が無いと 5.1 は Shift_JIS として読んで壊れる。
# 画面の文は仮。M '日本語' 'English' の組で並べてあるので、言い回しはここで直す。ファイルの中は英語だけ。
# 画面に出す文は、ラベルと事実だけ。説明の文章は 3 か所だけ（Smart App Control がオン・キットのファイルの記録がある・主なファイルが無い）で、【仮文】の印を付けた。鶴田さんが書き直す。
#
# 読むだけ。何も変えない（設定・レジストリ・ファイル・サービス・電源プランを書き換えない。Unblock-File もしない）。管理者の権限は要らない。
# 読めない所は、ファイルに unreadable と書く（画面は「読めず」）。ログが無いときは not found。
#
# 集める物
#   Windows の版・Smart App Control（HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy の VerifiedAndReputablePolicyState。0 オフ・1 オン・2 評価）・実行ポリシー
#   キットのフォルダの主な dll・exe・json があるか。大きさ・Zone.Identifier（ダウンロードの印。ZoneId の数字だけ読む。HostUrl は読まない）・署名の有無
#   キットのフォルダ全体で、Zone.Identifier の付いたファイルの数（dll・exe・ps1・cmd・json・その他）
#   直近 7 日のイベントのうち、キットのファイル名かキットのフォルダを含む物だけ（時刻・番号・ファイル名・脅威名・状態の数字）
#     Microsoft-Windows-CodeIntegrity/Operational  3076・3077・3089（3089 は同じ ActivityID の 3076・3077 に付く署名の数だけ）
#     Microsoft-Windows-Windows Defender/Operational  1006・1116・1117・1009・1010・1011
#   Defender の AMSI（スクリプトの検査）の記録の数だけ（記録にスクリプトの名前が残らないため、キットの名前では選べない。脅威名は集めない）
#   Vulkan のドライバの登録（HKLM\SOFTWARE\Khronos\Vulkan\Drivers と WOW6432Node）にキットの json があるか。Vulkan のローダーの版
#   GPU の名前の一覧（名前だけ）
#   Windows のグラフィックの設定（HKCU\Software\Microsoft\DirectX\UserGpuPreferences）の、キットのフォルダの中の物だけ。ほかのアプリは数だけ
#   Apple Mobile Device Service の状態・Apple Devices と iTunes（ストア版）があるか・iPhone が USB で見える台数（名前と UDID は出さない）
#   USB の選択的サスペンド・USB 3 のリンク電源管理・PCIe の ASPM（powercfg /query SCHEME_CURRENT の該当の項目だけ）
# 集めない物：ユーザー名・PC 名・シリアル番号・メールアドレス・iPhone の名前と UDID・ダウンロード元の URL・キット以外のイベント・ほかのアプリの GPU の設定の中身
# 伏せる物：PC のユーザー名・PC 名・プロフィールのフォルダ名・メールアドレス。android-log.ps1 と同じ「(hidden)」「(email)」に直す（画面にもファイルにも、最後にまとめて）
#
# 終わりのコード：0 txt を作った・2 txt を作れなかった
#
# 試す人は使わない、試験用の差し替え（pgpu_common.ps1 の物に加えて）：
#   PGPU_CHECK_OUT=フォルダ        txt をキットのフォルダの代わりにここへ作る
#   PGPU_TEST_CLIP=パス            クリップボードの代わりに、コピーするはずだった物をこのファイルに書く
#   PGPU_TEST_REG_ROOT=Software\x  レジストリの読み先を HKCU\Software\x\sac・vk64・vk32・gpupref に替える（本物の HKLM は読まない）
#   PGPU_TEST_EVENTS=パス          イベントログの代わりにこのファイルを読む。1 行 = タブ区切りで 種類(ci か def)・番号・何分前・ActivityID・名前=値 …。
#                                  「ci」「denied」の行は読めない、「def」「notfound」の行はログが無い
#   PGPU_TEST_KIT_REGEX=正規表現   キットの名前の見分け方の代わり（本物のイベントログを通して読み取りを試すため）
#   PGPU_POWERCFG=パス             powercfg.exe の代わり（.cmd でもよい）
#   PGPU_TEST_GPUS=名前;名前       GPU の一覧の代わり
#   PGPU_TEST_IPHONES=0,28         USB で見える iPhone の問題コード（台数 = 個数）。none は 0 台
#   PGPU_TEST_AMDS=running|stopped|none  Apple Mobile Device Service の状態の代わり
. (Join-Path $PSScriptRoot 'pgpu_common.ps1')

$script:Days = 7
$script:EventCap = 5000
$script:MaxHitLines = 8
$script:RegRoot = $env:PGPU_TEST_REG_ROOT
# 思わぬ誤りで止まるときも、短い 1 行を出して終わりのコード 2 で終わる
trap { Write-Host ('ERROR: ' + $_.Exception.Message) -ForegroundColor Red; exit 2 }

# ---- 伏せる（画面にもファイルにも）----
# android-log.ps1 の Hide-Private と同じ（シリアル番号は集めないので無い）。3 字以上の名前は前後が英数字でなければどこでも。短い名前は、Users\ の次の場所だけ
function Get-HideNames {
    $names = New-Object System.Collections.Generic.List[string]
    $cand = @($env:USERNAME, $env:COMPUTERNAME)
    if ($env:USERPROFILE) { $cand += (Split-Path -Leaf $env:USERPROFILE) }
    foreach ($x in $cand) { if ($x -and $x.Trim() -and -not $names.Contains($x)) { $names.Add($x) } }
    return @($names)
}
function Hide-Private([string]$text) {
    if (-not $text) { return '' }
    foreach ($n in (Get-HideNames)) {
        $e = [regex]::Escape($n)
        $text = [regex]::Replace($text, '(?i)(?<=[\\/]Users[\\/])' + $e + '(?![A-Za-z0-9])', '(hidden)')
        if ($n.Length -ge 3) { $text = [regex]::Replace($text, '(?i)(?<![A-Za-z0-9])' + $e + '(?![A-Za-z0-9])', '(hidden)') }
    }
    $text = [regex]::Replace($text, '[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}(?![A-Za-z0-9])', '(email)')
    return $text
}
function Say-I([string]$t) { Say-Info (Hide-Private $t) }

# ---- 小さな道具 ----
function Get-WindowsText {
    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction SilentlyContinue
    if (-not $cv) { return 'not available' }
    $name = $cv.ProductName
    if ([int]$cv.CurrentBuild -ge 22000) { $name = $name -replace 'Windows 10', 'Windows 11' }
    $build = '' + $cv.CurrentBuild
    if ($cv.UBR) { $build = $build + '.' + $cv.UBR }
    return ($name + ' ' + $cv.DisplayVersion + ' (build ' + $build + ', ' + $env:PROCESSOR_ARCHITECTURE + ')').Replace('  ', ' ')
}
function Get-OutRoot {
    $cands = @()
    if ($env:PGPU_CHECK_OUT) { $cands += $env:PGPU_CHECK_OUT }
    $cands += $script:KitDir
    try { $cands += [Environment]::GetFolderPath('Desktop') } catch { }
    $cands += [IO.Path]::GetTempPath()
    foreach ($c in $cands) {
        if (-not $c) { continue }
        try {
            if (-not (Test-Path -LiteralPath $c)) { [void](New-Item -ItemType Directory -Path $c -Force) }
            $probe = Join-Path $c ('.pgpu-write-test-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
            [IO.File]::WriteAllText($probe, 'x')
            Remove-Item -LiteralPath $probe -Force
            return $c
        } catch { }
    }
    return $script:KitDir
}
# レジストリの 1 つの鍵の値を読む。64 ビットの見え方で読む（32 ビットの PowerShell でも HKLM\SOFTWARE が WOW6432Node に曲がらない）。
# State は ok・notfound（鍵が無い）・unreadable（読めない）
function Get-RegLoc([string]$what) {
    if ($script:RegRoot) { return @('HKCU', ($script:RegRoot.TrimEnd('\') + '\' + $what)) }
    switch ($what) {
        'sac' { return @('HKLM', 'SYSTEM\CurrentControlSet\Control\CI\Policy') }
        'vk64' { return @('HKLM', 'SOFTWARE\Khronos\Vulkan\Drivers') }
        'vk32' { return @('HKLM', 'SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers') }
        'gpupref' { return @('HKCU', 'Software\Microsoft\DirectX\UserGpuPreferences') }
    }
}
function Read-RegKey([string]$what) {
    $loc = Get-RegLoc $what
    $k = $null
    try {
        $hive = $(if ($loc[0] -eq 'HKLM') { [Microsoft.Win32.RegistryHive]::LocalMachine } else { [Microsoft.Win32.RegistryHive]::CurrentUser })
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, [Microsoft.Win32.RegistryView]::Registry64)
        $k = $base.OpenSubKey($loc[1], $false)
    } catch { return @{ State = 'unreadable'; Values = @{} } }
    if ($null -eq $k) { return @{ State = 'notfound'; Values = @{} } }
    try {
        $h = [ordered]@{}
        foreach ($n in $k.GetValueNames()) { $h[$n] = $k.GetValue($n) }
        return @{ State = 'ok'; Values = $h }
    } catch { return @{ State = 'unreadable'; Values = @{} } } finally { $k.Close() }
}
# キットのフォルダを、デバイスの形のパス（\Device\HarddiskVolume3\Users\…）の中からも探せるように、ドライブ文字を除いた形にする
function Get-KitTail {
    $k = $script:KitDir.TrimEnd([char]92, [char]47)
    if ($k -match '^[A-Za-z]:(\\.+)$') { return $Matches[1] }
    if ($k -match '^\\\\(.+)$') { return '\' + $Matches[1] }
    return ''
}
function New-KitRegex {
    # 試験用：本物のイベントログを通して読み取りを試すため、キットの名前の見分け方を差し替える
    if ($env:PGPU_TEST_KIT_REGEX) { return (New-Object Text.RegularExpressions.Regex($env:PGPU_TEST_KIT_REGEX, [Text.RegularExpressions.RegexOptions]::IgnoreCase)) }
    $parts = New-Object System.Collections.Generic.List[string]
    foreach ($n in 'iphone_icd', 'pocketgpu', 'vklist', 'webcam_rx', 'ipaudio', 'livecap', 'camera_setup', 'dshow_cam', 'pgpu_', 'miccam', 'iphone-gpu', 'mic-camera', 'pc-check', 'check-pc', 'android-log', 'android-speed', 'fix-pocketgpu') { $parts.Add([regex]::Escape($n)) }
    $tail = Get-KitTail
    if ($tail) { $parts.Add([regex]::Escape($tail) + '[\\/]') }
    return (New-Object Text.RegularExpressions.Regex(($parts -join '|'), [Text.RegularExpressions.RegexOptions]::IgnoreCase))
}
# 記録の中のファイルの印（file:_C:\… など）を外し、キットのフォルダの中なら中の相対パス、そうでなければファイル名だけにする
function Get-KitDisplay([string]$token) {
    $t = [regex]::Replace($token.Trim(), '^[A-Za-z]+:_', '')
    $tail = Get-KitTail
    if ($tail) {
        $i = $t.IndexOf($tail + '\', [StringComparison]::OrdinalIgnoreCase)
        if ($i -lt 0) { $i = $t.IndexOf($tail + '/', [StringComparison]::OrdinalIgnoreCase) }
        if ($i -ge 0) {
            $rest = $t.Substring($i + $tail.Length).TrimStart([char]92, [char]47)
            if ($rest) { return $rest }
        }
    }
    $leaf = @($t -split '[\\/]')[-1]
    if ($leaf) { return $leaf } else { return $t }
}

# ---- Smart App Control・実行ポリシー ----
function Get-SacInfo {
    $r = Read-RegKey 'sac'
    if ($r.State -eq 'unreadable') { return @{ Text = 'unreadable'; Brief = (M '読めず' 'unreadable'); Warn = $false } }
    if ($r.State -eq 'notfound') { return @{ Text = 'not found (no CI\Policy key)'; Brief = (M '見つかりません' 'not found'); Warn = $false } }
    $v = $r.Values['VerifiedAndReputablePolicyState']
    if ($null -eq $v) { return @{ Text = 'not found (no VerifiedAndReputablePolicyState value)'; Brief = (M '見つかりません' 'not found'); Warn = $false } }
    $n = 0
    if (-not [int]::TryParse([string]$v, [ref]$n)) { return @{ Text = 'unknown value ' + $v; Brief = ('? ' + $v); Warn = $false } }
    switch ($n) {
        0 { return @{ Text = 'Off (VerifiedAndReputablePolicyState = 0)'; Brief = (M 'オフ（0）' 'Off (0)'); Warn = $false } }
        1 { return @{ Text = 'On, enforce (VerifiedAndReputablePolicyState = 1)'; Brief = (M 'オン（1）' 'On (1)'); Warn = $true } }
        2 { return @{ Text = 'Evaluation (VerifiedAndReputablePolicyState = 2)'; Brief = (M '評価モード（2）' 'Evaluation (2)'); Warn = $true } }
        default { return @{ Text = 'unknown value ' + $n + ' (VerifiedAndReputablePolicyState)'; Brief = ('? ' + $n); Warn = $false } }
    }
}
function Get-ExecPolicyText {
    try {
        $l = @(Get-ExecutionPolicy -List | ForEach-Object { [string]$_.Scope + '=' + [string]$_.ExecutionPolicy })
        if ($l.Count -eq 0) { return 'unreadable' }
        return ($l -join ', ')
    } catch { return 'unreadable' }
}

# ---- キットのファイル ----
$script:MainFiles = @(
    'PocketGPU.exe', 'vklist.exe', 'iphone_icd.dll', 'iphone_icd.json', 'x86\iphone_icd.dll', 'x86\iphone_icd.json',
    'dxvk\x64\d3d11.dll', 'dxvk\x64\dxgi.dll', 'dxvk\x64\d3d10core.dll', 'dxvk\x64\d3d9.dll',
    'dxvk\x32\d3d11.dll', 'dxvk\x32\dxgi.dll', 'dxvk\x32\d3d10core.dll', 'dxvk\x32\d3d9.dll',
    'extras\webcam_rx.exe', 'extras\ipaudio.exe', 'extras\livecap.exe', 'extras\camera_setup.exe', 'extras\dshow_cam64.dll', 'extras\dshow_cam32.dll'
)
# ダウンロードの印（Zone.Identifier）。none・ZoneId=数字・present (no ZoneId)・unreadable。ZoneId の数字だけ読む（HostUrl・ReferrerUrl は読まない）
function Get-ZoneMark([string]$path) {
    try {
        $st = @(Get-Item -LiteralPath $path -Stream '*' -ErrorAction Stop | Where-Object { $_.Stream -eq 'Zone.Identifier' })
        if ($st.Count -eq 0) { return 'none' }
        $txt = Get-Content -LiteralPath $path -Stream 'Zone.Identifier' -Raw -ErrorAction Stop
        if ($txt -match '(?im)^\s*ZoneId\s*=\s*(\d+)') { return 'ZoneId=' + $Matches[1] }
        return 'present (no ZoneId)'
    } catch { return 'unreadable' }
}
function Get-SigText([string]$path) {
    try {
        $s = Get-AuthenticodeSignature -LiteralPath $path -ErrorAction Stop
        switch ([string]$s.Status) {
            'NotSigned' { return 'unsigned' }
            'Valid' { return 'signed (valid)' }
            default { return 'signature ' + [string]$s.Status }
        }
    } catch { return 'unreadable' }
}
function Get-KitFileInfo {
    $rows = New-Object System.Collections.Generic.List[string]
    $missing = New-Object System.Collections.Generic.List[string]
    $present = 0
    $markedMain = 0
    foreach ($rel in $script:MainFiles) {
        $p = Join-Path $script:KitDir $rel
        $name = ('{0,-26}' -f $rel)
        try {
            if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { $rows.Add($name + ' MISSING'); $missing.Add($rel); continue }
            $present++
            $size = ([IO.FileInfo]$p).Length
            $z = Get-ZoneMark $p
            if ($z -like 'ZoneId=*' -or $z -like 'present*') { $markedMain++ }
            $sig = $(if ($rel -match '\.(dll|exe)$') { Get-SigText $p } else { '-' })
            $rows.Add($name + ' ' + ('{0,9}' -f $size) + ' bytes  mark: ' + $z + '  ' + $sig)
        } catch { $rows.Add($name + ' unreadable') }
    }
    # キットのフォルダ全体の印の数
    $cats = [ordered]@{ dll = @(0, 0); exe = @(0, 0); ps1 = @(0, 0); cmd = @(0, 0); json = @(0, 0); other = @(0, 0) }
    $all = 0
    $marked = 0
    $capped = $false
    try {
        $files = @(Get-ChildItem -LiteralPath $script:KitDir -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 801)
        if ($files.Count -gt 800) { $capped = $true; $files = @($files | Select-Object -First 800) }
        foreach ($f in $files) {
            $ext = $f.Extension.TrimStart('.').ToLowerInvariant()
            $c = $(if ($cats.Contains($ext)) { $ext } else { 'other' })
            $z = Get-ZoneMark $f.FullName
            $all++
            $cats[$c][1]++
            if ($z -like 'ZoneId=*' -or $z -like 'present*') { $marked++; $cats[$c][0]++ }
        }
        $by = (@($cats.Keys | Where-Object { $cats[$_][1] -gt 0 } | ForEach-Object { $_ + ' ' + $cats[$_][0] + '/' + $cats[$_][1] }) -join ', ')
        $sum = 'Files with a mark in the whole kit folder: ' + $marked + ' of ' + $all + $(if ($capped) { '+ (first 800 only)' } else { '' }) + $(if ($by) { ' (' + $by + ')' } else { '' })
    } catch { $sum = 'Files with a mark in the whole kit folder: unreadable' }
    return @{ Rows = $rows; Missing = $missing; Present = $present; MarkedMain = $markedMain; Marked = $marked; All = $all; Summary = $sum }
}

# ---- イベントログ（直近 7 日・キットを含む物だけ）----
# 試験用：PGPU_TEST_EVENTS のファイルから、本物と同じ形の XML を作る（読み取りの部分を、本物と同じ道で通すため）
function Get-FakeEventXml([string]$tag) {
    $res = @{ State = 'ok'; Xml = (New-Object System.Collections.Generic.List[string]); Capped = $false }
    $f = $env:PGPU_TEST_EVENTS
    if (-not (Test-Path -LiteralPath $f)) { $res.State = 'unreadable'; return $res }
    foreach ($line in [IO.File]::ReadAllLines($f)) {
        if (-not $line.Trim() -or $line.StartsWith('#')) { continue }
        $c = $line -split "`t"
        if ($c[0] -ne $tag) { continue }
        if ($c[1] -eq 'denied') { $res.State = 'unreadable'; continue }
        if ($c[1] -eq 'notfound') { $res.State = 'notfound'; continue }
        $time = (Get-Date).ToUniversalTime().AddMinutes(-[double]$c[2]).ToString('o')
        $data = ''
        for ($i = 4; $i -lt $c.Count; $i++) {
            $eq = $c[$i].IndexOf('=')
            if ($eq -lt 1) { continue }
            $data += "<Data Name='" + [Security.SecurityElement]::Escape($c[$i].Substring(0, $eq)) + "'>" + [Security.SecurityElement]::Escape($c[$i].Substring($eq + 1)) + '</Data>'
        }
        $act = $(if ($c[3] -and $c[3] -ne '-') { $c[3] } else { '{00000000-0000-0000-0000-000000000000}' })
        $res.Xml.Add("<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><EventID>" + $c[1] + "</EventID><TimeCreated SystemTime='" + $time + "'/><Correlation ActivityID='" + $act + "'/></System><EventData>" + $data + '</EventData></Event>')
    }
    return $res
}
function Get-RawEvents([string]$log, [int[]]$ids, [string]$tag) {
    if ($env:PGPU_TEST_EVENTS) { return (Get-FakeEventXml $tag) }
    $res = @{ State = 'ok'; Xml = (New-Object System.Collections.Generic.List[string]); Capped = $false }
    try {
        $cond = (@($ids | ForEach-Object { 'EventID=' + $_ }) -join ' or ')
        $xp = '*[System[(' + $cond + ') and TimeCreated[timediff(@SystemTime) <= ' + ([int64]$script:Days * 86400000) + ']]]'
        $q = New-Object System.Diagnostics.Eventing.Reader.EventLogQuery($log, [System.Diagnostics.Eventing.Reader.PathType]::LogName, $xp)
        $q.ReverseDirection = $true
        $rd = New-Object System.Diagnostics.Eventing.Reader.EventLogReader($q)
        try {
            while ($true) {
                $e = $rd.ReadEvent()
                if ($null -eq $e) { break }
                try { $res.Xml.Add($e.ToXml()) } finally { $e.Dispose() }
                if ($res.Xml.Count -ge $script:EventCap) { $res.Capped = $true; break }
            }
        } finally { $rd.Dispose() }
    } catch [System.Diagnostics.Eventing.Reader.EventLogNotFoundException] { $res.State = 'notfound'
    } catch { if ($res.Xml.Count -eq 0) { $res.State = 'unreadable' } }
    return $res
}
function Read-KitEvents([string]$log, [int[]]$ids, [string]$tag, [string]$label, [Text.RegularExpressions.Regex]$kitRx) {
    $raw = Get-RawEvents $log $ids $tag
    $out = @{ State = $raw.State; Total = 0; Capped = $raw.Capped; Hits = (New-Object System.Collections.Generic.List[string]); HitCount = 0; Label = $label; Amsi = 0 }
    if ($raw.State -ne 'ok') { return $out }
    $cutoff = (Get-Date).AddDays(-$script:Days)
    $sig = @{}          # ActivityID -> 3089 の TotalSignatureCount の一番小さい数
    $hitEvents = New-Object System.Collections.Generic.List[object]
    foreach ($x in $raw.Xml) {
        if ($x -notmatch '<EventID[^>]*>(\d+)</EventID>') { continue }
        $id = [int]$Matches[1]
        $time = $null
        if ($x -match "SystemTime=['""]([^'""]+)['""]") {
            try { $time = [datetime]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToLocalTime() } catch { }
        }
        if ($null -eq $time -or $time -lt $cutoff) { continue }
        $out.Total++
        $act = ''
        if ($x -match "ActivityID=['""]([^'""]+)['""]") { $act = $Matches[1].ToLowerInvariant() }
        $ei = $x.IndexOf('<EventData')
        $ed = $(if ($ei -ge 0) { $x.Substring($ei) } else { '' })
        if ($id -eq 3089) {
            if ($act -and $ed -match "Name=['""]TotalSignatureCount['""]>(\d+)<") {
                $n = [int]$Matches[1]
                if (-not $sig.ContainsKey($act) -or $n -lt $sig[$act]) { $sig[$act] = $n }
            }
            continue
        }
        # AMSI（スクリプトの中身の検査）の検出は、記録にスクリプトの名前が残らず（Path は powershell.exe）、キットの名前では選べない。数だけ数える
        if ($ed -and ($ed.IndexOf('>AMSI<', [StringComparison]::OrdinalIgnoreCase) -ge 0 -or $ed.IndexOf('amsi:_', [StringComparison]::OrdinalIgnoreCase) -ge 0)) { $out.Amsi++ }
        if (-not $ed -or -not $kitRx.IsMatch($ed)) { continue }
        $hitEvents.Add(@{ Id = $id; Time = $time; Act = $act; Xml = $x })
    }
    $out.HitCount = $hitEvents.Count
    foreach ($h in $hitEvents) {
        if ($out.Hits.Count -ge $script:MaxHitLines) { break }
        $data = [ordered]@{}
        try {
            $doc = New-Object Xml.XmlDocument
            $doc.XmlResolver = $null
            $doc.LoadXml($h.Xml)
            foreach ($d in @($doc.Event.EventData.Data)) { if ($d -and $d.GetAttribute('Name')) { $data[$d.GetAttribute('Name')] = [string]$d.InnerText } }
        } catch { }
        $files = New-Object System.Collections.Generic.List[string]
        foreach ($v in $data.Values) {
            foreach ($tok in ([string]$v -split ';')) {
                if ($tok -and $kitRx.IsMatch($tok)) {
                    $d2 = Get-KitDisplay $tok
                    if ($d2 -and -not $files.Contains($d2)) { $files.Add($d2) }
                }
            }
        }
        $line = $h.Time.ToString('yyyy-MM-dd HH:mm') + '  ' + $label + ' ' + $h.Id + '  ' + $(if ($files.Count -gt 0) { (@($files | Select-Object -First 3) -join ', ') + $(if ($files.Count -gt 3) { ' +' + ($files.Count - 3) } else { '' }) } else { '(kit name in another field)' })
        if ($data.Contains('Threat Name') -and $data['Threat Name']) { $line += '  threat=' + $data['Threat Name'] }
        if ($data.Contains('Action ID') -and $data['Action ID'] -ne '') { $line += '  action=' + $data['Action ID'] }
        if ($data.Contains('Status') -and $data['Status']) { $line += '  status=' + $data['Status'] }
        if ($data.Contains('Validated Signing Level') -and $data['Validated Signing Level'] -ne '') { $line += '  signing-level=' + $data['Validated Signing Level'] }
        if ($h.Act -and $sig.ContainsKey($h.Act)) { $line += '  signatures=' + $sig[$h.Act] }
        if ($line.Length -gt 240) { $line = $line.Substring(0, 237) + '...' }
        $out.Hits.Add($line)
    }
    return $out
}
function Get-EventSummaryText($r) {
    switch ($r.State) {
        'unreadable' { return 'unreadable' }
        'notfound' { return 'log not found' }
        default { return [string]$r.Total + $(if ($r.Capped) { '+' } else { '' }) + ' events in total, ' + $r.HitCount + ' mention the kit' }
    }
}
function Get-EventBrief($r) {
    switch ($r.State) {
        'unreadable' { return (M '読めず' 'unreadable') }
        'notfound' { return (M 'ログがありません' 'log not found') }
        default { return (M ([string]$r.Total + $(if ($r.Capped) { '+' } else { '' }) + ' 件中 キット関連 ' + $r.HitCount + ' 件') ([string]$r.Total + $(if ($r.Capped) { '+' } else { '' }) + ' events, ' + $r.HitCount + ' about the kit')) }
    }
}

# ---- Vulkan・GPU の設定 ----
function Get-LoaderText([string]$dirName) {
    try {
        if ($dirName -eq 'System32' -and $env:PGPU_TEST_VK_LOADER) {
            $v = Get-VulkanLoaderVersion
            if ($null -eq $v) { return 'not found' } else { return [string]$v }
        }
        $dll = Join-Path (Join-Path $env:WINDIR $dirName) 'vulkan-1.dll'
        if (-not (Test-Path -LiteralPath $dll)) { return 'not found' }
        $vi = (Get-Item -LiteralPath $dll).VersionInfo
        return ([string]$vi.FileMajorPart + '.' + $vi.FileMinorPart + '.' + $vi.FileBuildPart + '.' + $vi.FilePrivatePart)
    } catch { return 'unreadable' }
}
function Get-VkRegistration([string]$what, [string]$label, [Text.RegularExpressions.Regex]$kitRx) {
    $r = Read-RegKey $what
    $lines = New-Object System.Collections.Generic.List[string]
    $kitFound = $false
    if ($r.State -eq 'unreadable') { $lines.Add($label + ': unreadable'); return @{ Lines = $lines; Kit = $false; Brief = (M '読めず' 'unreadable'); State = 'unreadable' } }
    if ($r.State -eq 'notfound') { $lines.Add($label + ': key not found (no driver registered here)'); return @{ Lines = $lines; Kit = $false; Brief = (M '登録なし' 'no entries'); State = 'notfound' } }
    $others = New-Object System.Collections.Generic.List[string]
    foreach ($n in $r.Values.Keys) {
        if ($kitRx.IsMatch([string]$n)) {
            $kitFound = $true
            $exists = $false
            try { $exists = Test-Path -LiteralPath ([string]$n) -PathType Leaf } catch { }
            $lines.Add('  kit json registered: ' + (Get-KitDisplay ([string]$n)) + ', value ' + $r.Values[$n] + ', file ' + $(if ($exists) { 'exists' } else { 'missing' }))
        } else { $others.Add(@(([string]$n) -split '[\\/]')[-1]) }
    }
    $head = $label + ': ' + $r.Values.Count + ' entries, kit json registered: ' + $(if ($kitFound) { 'yes' } else { 'no' })
    $lines.Insert(0, $head)
    if ($others.Count -gt 0) { $lines.Add('  other entries (file names): ' + (@($others | Select-Object -First 10) -join ', ') + $(if ($others.Count -gt 10) { ' +' + ($others.Count - 10) } else { '' })) }
    return @{ Lines = $lines; Kit = $kitFound; Brief = (M ([string]$r.Values.Count + ' 件・キット ' + $(if ($kitFound) { 'あり' } else { 'なし' })) ([string]$r.Values.Count + ' entries, kit ' + $(if ($kitFound) { 'yes' } else { 'no' }))); State = 'ok' }
}
function Get-GpuPrefInfo([Text.RegularExpressions.Regex]$kitRx) {
    $r = Read-RegKey 'gpupref'
    $lines = New-Object System.Collections.Generic.List[string]
    if ($r.State -eq 'unreadable') { $lines.Add('unreadable'); return @{ Lines = $lines; Kit = 0; Brief = (M '読めず' 'unreadable') } }
    if ($r.State -eq 'notfound') { $lines.Add('key not found (no per-app GPU choice saved)'); return @{ Lines = $lines; Kit = 0; Brief = (M '登録なし' 'no entries') } }
    $tail = Get-KitTail
    $kit = 0
    foreach ($n in $r.Values.Keys) {
        $nm = [string]$n
        $inKit = ($nm.StartsWith($script:KitDir.TrimEnd([char]92) + '\', [StringComparison]::OrdinalIgnoreCase)) -or ($tail -and $nm.IndexOf($tail + '\', [StringComparison]::OrdinalIgnoreCase) -ge 0)
        if ($inKit) { $kit++; $lines.Add(('{0}: {1}' -f (Get-KitDisplay $nm), $r.Values[$n])) }
    }
    $lines.Insert(0, 'Entries in total: ' + $r.Values.Count + ' (only entries inside the kit folder are listed). Entries inside the kit folder: ' + $kit)
    return @{ Lines = $lines; Kit = $kit; Brief = (M ('全部で ' + $r.Values.Count + ' 件・キットの中 ' + $kit + ' 件') ('' + $r.Values.Count + ' in total, ' + $kit + ' in the kit')) }
}
function Get-GpuNames {
    if ($env:PGPU_TEST_GPUS) { return @($env:PGPU_TEST_GPUS -split ';' | Where-Object { $_.Trim() } | ForEach-Object { $_.Trim() }) }
    try {
        $g = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object { [string]$_.Name } | Where-Object { $_ })
        return $g
    } catch { return $null }
}

# ---- Apple・iPhone ----
function Get-AppleServiceInfo {
    if ($env:PGPU_TEST_AMDS) {
        switch ($env:PGPU_TEST_AMDS) {
            'running' { return @{ Text = 'Running (startup Automatic)'; Brief = (M '動いています' 'Running'); Running = $true } }
            'stopped' { return @{ Text = 'Stopped (startup Automatic)'; Brief = (M '止まっています' 'Stopped'); Running = $false } }
            default { return @{ Text = 'not found'; Brief = (M '見つかりません' 'not found'); Running = $false } }
        }
    }
    try {
        $s = Get-Service -Name 'Apple Mobile Device Service' -ErrorAction SilentlyContinue
        if (-not $s) { return @{ Text = 'not found'; Brief = (M '見つかりません' 'not found'); Running = $false } }
        $run = ([string]$s.Status -eq 'Running')
        return @{ Text = [string]$s.Status + ' (startup ' + [string]$s.StartType + ')'; Brief = $(if ($run) { M '動いています' 'Running' } else { M ('止まっています（' + $s.Status + '）') ('Not running (' + $s.Status + ')') }); Running = $run }
    } catch { return @{ Text = 'unreadable'; Brief = (M '読めず' 'unreadable'); Running = $false } }
}
# Windows が USB で見ている iPhone・iPad（Apple の会社番号 05AC・型は 12xx）。1 台 = 親の機器 1 つ（&MI_ の子は数えない）。名前と UDID は出さない
function Get-UsbPhoneInfo {
    if ($env:PGPU_TEST_IPHONES) {
        if ($env:PGPU_TEST_IPHONES -eq 'none') { return @{ State = 'ok'; Codes = @() } }
        return @{ State = 'ok'; Codes = @($env:PGPU_TEST_IPHONES -split ',' | ForEach-Object { [int]$_.Trim() }) }
    }
    try {
        $d = @(Get-CimInstance Win32_PnPEntity -Filter "PNPDeviceID LIKE 'USB\\VID_05AC&PID_12%'" -ErrorAction Stop | Where-Object { $_.PNPDeviceID -notmatch '&MI_' })
        return @{ State = 'ok'; Codes = @($d | ForEach-Object { [int]$_.ConfigManagerErrorCode }) }
    } catch { return @{ State = 'unreadable'; Codes = @() } }
}

# ---- USB の電源設定（powercfg /query の該当の項目だけ）----
$script:PowerPlans = @{
    '381b4222-f694-41f0-9685-ff5bb260df2e' = 'Balanced'
    '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c' = 'High performance'
    'a1841308-3541-4fab-bc81-f71556f20b4a' = 'Power saver'
    'e9a42b02-d5df-448d-aa00-03f14749eb61' = 'Ultimate Performance'
}
$script:PowerItems = @(
    @{ Label = 'USB selective suspend'; LabelJa = 'USB のセレクティブ サスペンド'; Sub = '2a737441-1930-4402-8d77-b2bebba308a3'; Set = '48e6b7a6-50f5-4782-a5d4-53bb8f07e226'; Names = @{ 0 = 'disabled'; 1 = 'enabled' } },
    @{ Label = 'USB 3 link power management'; LabelJa = 'USB 3 のリンク電源管理'; Sub = '2a737441-1930-4402-8d77-b2bebba308a3'; Set = 'd4e98f31-5ffe-4ce1-be31-1b38b384c009'; Names = @{} },
    @{ Label = 'PCIe link state power management (ASPM)'; LabelJa = 'PCIe のリンク状態の電源管理（ASPM）'; Sub = '501a4d13-42af-4429-9fd1-a8218c268e20'; Set = 'ee12f906-d277-404b-b6da-e5fa1a576df5'; Names = @{ 0 = 'off'; 1 = 'moderate'; 2 = 'maximum' } }
)
function Invoke-Powercfg([string]$arguments) {
    $exe = $(if ($env:PGPU_POWERCFG) { $env:PGPU_POWERCFG } else { Join-Path $env:SystemRoot 'System32\powercfg.exe' })
    if (-not (Test-Path -LiteralPath $exe)) { return @{ Ok = $false; Exit = -1; Out = ''; Started = $false } }
    $psi = New-Object Diagnostics.ProcessStartInfo
    if ($exe -like '*.cmd') { $psi.FileName = $env:ComSpec; $psi.Arguments = '/c ""' + $exe + '" ' + $arguments + '"' } else { $psi.FileName = $exe; $psi.Arguments = $arguments }
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    # 画面の文字コード（日本語の Windows は Shift_JIS）で出る。数字と GUID だけ読むので、読み違えても困らない
    try { $psi.StandardOutputEncoding = [Text.Encoding]::GetEncoding([Globalization.CultureInfo]::CurrentCulture.TextInfo.OEMCodePage) } catch { }
    try { $p = [Diagnostics.Process]::Start($psi) } catch { return @{ Ok = $false; Exit = -1; Out = ''; Started = $false } }
    try { $p.StandardInput.Close() } catch { }
    $o = $p.StandardOutput.ReadToEndAsync()
    $er = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(20000)) { try { $p.Kill() } catch { }; return @{ Ok = $false; Exit = -1; Out = ''; Started = $true } }
    [void]$p.WaitForExit()
    return @{ Ok = ($p.ExitCode -eq 0); Exit = $p.ExitCode; Out = ($o.Result + $er.Result); Started = $true }
}
function Get-PowerPlanText {
    $r = Invoke-Powercfg '/getactivescheme'
    if (-not $r.Started) { return 'unreadable' }
    if ($r.Out -match '([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})') {
        $g = $Matches[1].ToLowerInvariant()
        $nm = $(if ($script:PowerPlans.ContainsKey($g)) { $script:PowerPlans[$g] } else { 'custom plan' })
        return $nm + ' (' + $g + ')'
    }
    return 'unreadable'
}
function Get-PowerItemText($item) {
    $r = Invoke-Powercfg ('/query SCHEME_CURRENT ' + $item.Sub + ' ' + $item.Set)
    if (-not $r.Started) { return @{ Text = 'unreadable'; Ac = $null; Dc = $null } }
    $hex = @([regex]::Matches($r.Out, '0x([0-9a-fA-F]{8})') | ForEach-Object { [Convert]::ToInt64($_.Groups[1].Value, 16) })
    if (-not $r.Ok -or $hex.Count -lt 1) { return @{ Text = 'not present on this PC'; Ac = $null; Dc = $null } }
    $ac = $hex[$hex.Count - 2]
    $dc = $hex[$hex.Count - 1]
    if ($hex.Count -eq 1) { $ac = $hex[0]; $dc = $null }
    $fmt = { param($v) if ($null -eq $v) { return '-' }; $s = [string]$v; if ($item.Names.ContainsKey([int]$v)) { $s += ' (' + $item.Names[[int]$v] + ')' }; return $s }
    return @{ Text = 'AC ' + (& $fmt $ac) + ', DC ' + (& $fmt $dc); Ac = $ac; Dc = $dc }
}

# =====================================================================
# ---- 流れ / Main ----
# =====================================================================
Out-Line ''
Out-Line (M 'PocketGPU この PC のチェック' 'PocketGPU PC check')
Say-Info (M '読むだけです。何も変更しません' 'Read only. Nothing is changed')
$kitRx = New-KitRegex
$total = 8
$S = @{}      # 章ごとの行（English）
$B = @{}      # 画面用の短い結果

# ---- 1. Windows ----
Say-Step 1 $total (M 'Windows とセキュリティ' 'Windows and security')
$winText = Get-WindowsText
$sac = Get-SacInfo
$epText = Get-ExecPolicyText
$elev = Test-Elevated
$S.win = @(
    ('Windows: ' + $winText),
    ('Smart App Control: ' + $sac.Text),
    ('Execution policy: ' + $epText),
    ('Run as administrator: ' + $(if ($elev) { 'yes' } else { 'no' }))
)
Say-I ('Windows: ' + $winText)
Say-I ('Smart App Control: ' + $sac.Brief)
Say-I ((M '実行ポリシー：' 'Execution policy: ') + $(if ($epText -eq 'unreadable') { M '読めず' 'unreadable' } else { $epText }))
if ($sac.Warn) { Say-Warn (M '【仮文】Smart App Control がオンか評価モードです。署名の無いファイルが止められる場合があります' 'DRAFT: Smart App Control is on or in evaluation mode. Unsigned files may be blocked') }

# ---- 2. キットのファイル ----
Say-Step 2 $total (M 'キットのファイル' 'Kit files')
$kf = Get-KitFileInfo
$kitIni = Join-Path $PSScriptRoot 'kit.ini'
$iniText = $(if (Test-Path -LiteralPath $kitIni) { 'app ' + $(if ($script:AppVersion) { $script:AppVersion } else { '-' }) + ', proto ' + $script:ProtoVersion } else { '(no tools\kit.ini)' })
$ansi = $true
try { $ansi = Test-AnsiPath $script:KitDir } catch { }
$S.files = @(
    ('Kit: ' + $iniText),
    ('Kit folder: ' + $script:KitDir),
    ('Kit folder path: ' + $script:KitDir.Length + ' characters, ' + $(if ($ansi) { 'all characters fit the Windows ANSI code page' } else { 'has characters outside the Windows ANSI code page' }))
) + @($kf.Rows) + @($kf.Summary)
Say-I ((M '主なファイル：' 'Main files: ') + $kf.Present + (M (' / ' + $script:MainFiles.Count + ' 個あり') (' of ' + $script:MainFiles.Count + ' present')))
Say-I ((M 'ダウンロードの印（Zone.Identifier）：主なファイル ' 'Download mark (Zone.Identifier): main files ') + $kf.MarkedMain + (M ' 個・キット全体 ' ', whole kit ') + $kf.Marked + ' / ' + $kf.All)
if ($kf.Missing.Count -gt 0) {
    Say-I ((M '無いファイル：' 'Missing: ') + (@($kf.Missing | Select-Object -First 6) -join ', ') + $(if ($kf.Missing.Count -gt 6) { ' +' + ($kf.Missing.Count - 6) } else { '' }))
    Say-Warn (M '【仮文】キットの主なファイルが足りません' 'DRAFT: Some main kit files are missing')
}
if (-not $ansi) { Say-I (M 'キットの場所：ANSI にない文字があります' 'Kit folder path: has characters outside the ANSI code page') }

# ---- 3. 記録（イベント）----
Say-Step 3 $total (M 'Windows の記録（直近 7 日・キット関連だけ）' 'Windows records (last 7 days, kit only)')
$ci = Read-KitEvents 'Microsoft-Windows-CodeIntegrity/Operational' @(3076, 3077, 3089) 'ci' 'CI' $kitRx
$df = Read-KitEvents 'Microsoft-Windows-Windows Defender/Operational' @(1006, 1116, 1117, 1009, 1010, 1011) 'def' 'Defender' $kitRx
$S.events = @(
    'CI = Microsoft-Windows-CodeIntegrity/Operational (3076, 3077, 3089)',
    'Defender = Microsoft-Windows-Windows Defender/Operational (1006, 1116, 1117, 1009, 1010, 1011)',
    ('CI: ' + (Get-EventSummaryText $ci)),
    ('Defender: ' + (Get-EventSummaryText $df))
)
if ($df.State -eq 'ok') { $S.events += ('Defender events from AMSI script scans (Windows does not record the script name, so they cannot be matched to kit files): ' + $df.Amsi) }
foreach ($r in @($ci, $df)) {
    foreach ($l in $r.Hits) { $S.events += $l }
    if ($r.HitCount -gt $r.Hits.Count) { $S.events += ('(' + ($r.HitCount - $r.Hits.Count) + ' more ' + $r.Label + ' events left out)') }
}
Say-I ('CodeIntegrity: ' + (Get-EventBrief $ci))
Say-I ('Defender: ' + (Get-EventBrief $df))
if ($df.State -eq 'ok') { Say-I ((M 'Defender の AMSI（スクリプトの検査）の記録：' 'Defender AMSI (script scan) events: ') + $df.Amsi) }
$kitEvents = $ci.HitCount + $df.HitCount
if ($kitEvents -gt 0) { Say-Warn (M '【仮文】キットのファイルに関する記録があります。このテキストを送ってください' 'DRAFT: There are records about kit files. Please send this text') }

# ---- 4. Vulkan・グラフィックの設定 ----
Say-Step 4 $total (M 'Vulkan とグラフィックの設定' 'Vulkan and graphics settings')
$l64 = Get-LoaderText 'System32'
$l32 = Get-LoaderText 'SysWOW64'
$vk64 = Get-VkRegistration 'vk64' 'HKLM\SOFTWARE\Khronos\Vulkan\Drivers' $kitRx
$vk32 = Get-VkRegistration 'vk32' 'HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers' $kitRx
$gp = Get-GpuPrefInfo $kitRx
$S.vk = @(('Vulkan loader, 64-bit (System32\vulkan-1.dll): ' + $l64), ('Vulkan loader, 32-bit (SysWOW64\vulkan-1.dll): ' + $l32)) + @($vk64.Lines) + @($vk32.Lines)
$S.pref = @($gp.Lines)
Say-I ((M 'Vulkan のローダー（64 ビット）：' 'Vulkan loader (64-bit): ') + $(if ($l64 -eq 'unreadable') { M '読めず' 'unreadable' } else { $l64 }))
Say-I ((M 'Vulkan のドライバ登録（64 ビット）：' 'Vulkan driver entries (64-bit): ') + $vk64.Brief)
Say-I ((M 'Vulkan のドライバ登録（32 ビット）：' 'Vulkan driver entries (32-bit): ') + $vk32.Brief)
Say-I ((M 'Windows のグラフィックの設定：' 'Windows graphics settings: ') + $gp.Brief)

# ---- 5. GPU ----
Say-Step 5 $total (M 'GPU' 'GPU')
$gpus = Get-GpuNames
if ($null -eq $gpus) {
    $S.gpu = @('GPU list: unreadable')
    Say-I (M 'GPU の一覧：読めず' 'GPU list: unreadable')
} else {
    $S.gpu = @($gpus | ForEach-Object { 'GPU: ' + $_ })
    if ($gpus.Count -eq 0) { $S.gpu = @('GPU: (none listed)') }
    foreach ($g in $gpus) { Say-I ('GPU: ' + $g) }
}

# ---- 6. Apple・iPhone ----
Say-Step 6 $total (M 'Apple と iPhone' 'Apple and iPhone')
$svc = Get-AppleServiceInfo
$sw = $null
try { $sw = Get-AppleSoftware } catch { }
$devApp = $(if ($null -eq $sw) { 'unreadable' } elseif ($sw.AppleDevices) { 'yes' } else { 'no' })
$itApp = $(if ($null -eq $sw) { 'unreadable' } elseif ($sw.StoreITunes) { 'yes' } else { 'no' })
$ph = Get-UsbPhoneInfo
$phText = $(if ($ph.State -ne 'ok') { 'unreadable' } else { [string]$ph.Codes.Count + $(if ($ph.Codes.Count -gt 0) { ' (problem code' + $(if ($ph.Codes.Count -gt 1) { 's' } else { '' }) + ': ' + (($ph.Codes | ForEach-Object { [string]$_ }) -join ', ') + '; 0 = no problem)' } else { '' }) })
$S.apple = @(
    ('Apple Mobile Device Service: ' + $svc.Text),
    ('Apple Devices app (Microsoft Store): ' + $devApp),
    ('iTunes (Microsoft Store): ' + $itApp),
    ('iPhone/iPad seen on USB: ' + $phText)
)
Say-I ((M 'Apple Mobile Device Service：' 'Apple Mobile Device Service: ') + $svc.Brief)
Say-I ((M 'Apple Devices（ストア）：' 'Apple Devices (Store): ') + $(if ($devApp -eq 'yes') { M 'あり' 'yes' } elseif ($devApp -eq 'no') { M 'なし' 'no' } else { M '読めず' 'unreadable' }))
Say-I ((M 'USB で見える iPhone：' 'iPhone on USB: ') + $(if ($ph.State -ne 'ok') { M '読めず' 'unreadable' } else { [string]$ph.Codes.Count + (M ' 台' '') }))

# ---- 7. USB の電源 ----
Say-Step 7 $total (M 'USB の電源設定' 'USB power settings')
$planText = Get-PowerPlanText
$S.power = @('Power plan: ' + $planText)
$powerBrief = @()
$ssText = ''
foreach ($it in $script:PowerItems) {
    $pi = Get-PowerItemText $it
    $S.power += ($it.Label + ': ' + $pi.Text)
    if ($it.Label -eq 'USB selective suspend') { $ssText = $pi.Text }
    Say-I ((M $it.LabelJa $it.Label) + ': ' + $(if ($pi.Text -eq 'unreadable') { M '読めず' 'unreadable' } elseif ($pi.Text -like 'not present*') { M 'この PC にありません' 'not present' } else { $pi.Text }))
}

# ---- 8. 書く ----
Say-Step 8 $total (M 'txt を書く' 'Write the text')
$sum = New-Object System.Collections.Generic.List[string]
$sum.Add('Smart App Control: ' + $sac.Text)
$sum.Add('Main kit files present: ' + $kf.Present + ' of ' + $script:MainFiles.Count + $(if ($kf.Missing.Count -gt 0) { ' (missing: ' + (@($kf.Missing | Select-Object -First 6) -join ', ') + $(if ($kf.Missing.Count -gt 6) { ' +' + ($kf.Missing.Count - 6) } else { '' }) + ')' } else { '' }))
$sum.Add('Files with a download mark: main ' + $kf.MarkedMain + ', whole kit ' + $kf.Marked + ' of ' + $kf.All)
$sum.Add('Events in the last ' + $script:Days + ' days that mention the kit: CI ' + $(if ($ci.State -eq 'ok') { $ci.HitCount } else { $ci.State }) + ', Defender ' + $(if ($df.State -eq 'ok') { $df.HitCount } else { $df.State }))
$sum.Add('Defender AMSI script-scan events in the last ' + $script:Days + ' days (file not named): ' + $(if ($df.State -eq 'ok') { [string]$df.Amsi } else { $df.State }))
$sum.Add('Kit json in the Vulkan driver registry: 64-bit ' + $(if ($vk64.State -eq 'unreadable') { 'unreadable' } elseif ($vk64.Kit) { 'yes' } else { 'no' }) + ', 32-bit ' + $(if ($vk32.State -eq 'unreadable') { 'unreadable' } elseif ($vk32.Kit) { 'yes' } else { 'no' }))
$sum.Add('Kit entries in the Windows graphics settings: ' + $gp.Kit)
$sum.Add('GPUs: ' + $(if ($null -eq $gpus) { 'unreadable' } else { [string]$gpus.Count }))
$sum.Add('iPhone/iPad on USB: ' + $(if ($ph.State -ne 'ok') { 'unreadable' } else { [string]$ph.Codes.Count }))
$sum.Add('Apple Mobile Device Service: ' + $svc.Text)
$sum.Add('USB selective suspend: ' + $(if ($ssText) { $ssText } else { 'unreadable' }))
$info = New-Object System.Collections.Generic.List[string]
$info.Add('PocketGPU PC check')
$info.Add('Created: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm'))
$info.Add('Read only: nothing on this PC was changed.')
$info.Add('')
$info.Add('[Summary]')
foreach ($x in $sum) { $info.Add($x) }
foreach ($sec in @(@('Windows', 'win'), @('Kit files', 'files'), @('Events, last 7 days, only those that mention kit files', 'events'), @('Vulkan', 'vk'), @('Windows graphics settings (HKCU DirectX UserGpuPreferences)', 'pref'), @('GPU', 'gpu'), @('Apple / iPhone', 'apple'), @('USB power (active power plan)', 'power'))) {
    $info.Add('')
    $info.Add('[' + $sec[0] + ']')
    foreach ($x in @($S[$sec[1]])) { $info.Add([string]$x) }
}
$info.Add('')
$info.Add('Not collected: user name, PC name, serial numbers, e-mail addresses, iPhone name and UDID, download URLs, events that do not mention the kit.')
$info.Add('Hidden: PC user name, PC name, e-mail addresses.')

$text = Hide-Private ($info -join "`n")
$root = Get-OutRoot
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$name = 'pocketgpu-pc-check-' + $stamp
$file = Join-Path $root ($name + '.txt')
$n = 1
while (Test-Path -LiteralPath $file) { $n++; $file = Join-Path $root ($name + '-' + $n + '.txt') }
$failed = $false
try {
    $body = ($text -replace "`r`n", "`n" -replace "`r", "`n") -replace "`n", "`r`n"
    if (-not $body.EndsWith("`r`n")) { $body += "`r`n" }
    [IO.File]::WriteAllText($file, $body, (New-Object Text.UTF8Encoding($true)))
} catch { $failed = $true; Say-NG ((M 'txt を作れませんでした：' 'Could not write the text file: ') + $_.Exception.Message) }
if (-not $failed -and -not (Test-Path -LiteralPath $file)) { $failed = $true; Say-NG (M 'txt を作れませんでした' 'Could not write the text file') }
if ($failed) { exit 2 }

Out-Line ''
Say-OK (Hide-Private $file)
Say-Info ((M '文字数：' 'Characters: ') + $text.Length)
Say-Info (M 'この txt を Discord に貼ってください' 'Paste this text on Discord')
Say-Info (M '含まない：ユーザー名・PC 名・シリアル番号・メールアドレス・iPhone の名前と UDID' 'Not included: user name, PC name, serial numbers, e-mail addresses, iPhone name and UDID')
if (Ask-YesNo (M 'クリップボードにコピーしますか？' 'Copy it to the clipboard?')) {
    try {
        if ($env:PGPU_TEST_CLIP) { [IO.File]::WriteAllText($env:PGPU_TEST_CLIP, $text, (New-Object Text.UTF8Encoding($false))) }
        else { Set-Clipboard -Value $text }
        Say-OK (M 'コピーしました' 'Copied')
    } catch { Say-Warn ((M 'コピーできませんでした：' 'Could not copy: ') + $_.Exception.Message) }
}
exit 0
