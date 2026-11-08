# リポジトリに、秘密や個人の物が入っていないかを調べる。Windows PowerShell 5.1 で動く。見つかれば、場所を出して終わりのコード 1
#   powershell -ExecutionPolicy Bypass -File tests\Test-NoSecrets.ps1 [-Root フォルダ]
# 一般的な形だけを探す（合言葉の形・Cloudflare の名前・メールアドレス・ユーザーのフォルダ・長い 16 進数・鍵の形）。
# プロジェクトの公開の招待リンク（discord.gg/F3Mk8DBxqQ）は README に載せるので見逃す。ほかの招待は止める
# 試験用の偽の値の行には、行の最後に「# pgpu-allow-secret」と書くと、見逃す。
# 人の名前など、このリポジトリに書いてはいけない物は、ここにも書かない。公開する前に、自分の手元で別に検索する
param([string]$Root = '')
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$patterns = [ordered]@{
    'workers.dev の名前'                = '[A-Za-z0-9-]+\.[A-Za-z0-9-]+\.workers\.dev'
    'Cloudflare / pages の名前'          = '\.pages\.dev|cloudflare-ipfs|api\.cloudflare\.com/client'
    'メールアドレス'                      = '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.(com|jp|net|org|io|dev|co|me)\b'
    'ユーザーのフォルダ'                  = '(?i)[A-Z]:\\Users\\(?!<|%|\.\.\.|\(user\)|Public|Default|\$)[^\\\s"'']+'
    '私用のドライブ・フォルダ'            = '(?i)\b[D-Z]:\\(iphone-rpc|work|secrets|Users)\b'
    '値の入った合言葉・トークン'          = '(?i)\b(token|passphrase|secret|password|apikey|api_key)\s*[=:]\s*[''"]?[A-Za-z0-9_\-]{12,}'
    '32 文字以上の 16 進数（名前空間の番号など）' = '\b[0-9a-fA-F]{32,}\b'
    '鍵の形'                            = '\b(sk-[A-Za-z0-9]{20,}|ghp_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|AKIA[0-9A-Z]{16}|xox[baprs]-[A-Za-z0-9-]{10,})'
    'Discord の招待・Webhook'            = 'discord(app)?\.com/api/webhooks|discord\.gg/(?!F3Mk8DBxqQ\b)[A-Za-z0-9]+'
    '人ごとの印の値'                      = 'PGKIT-MARK:(?!#)[A-Za-z0-9_-]{8,}'
}
$files = Get-ChildItem -LiteralPath $Root -Recurse -File -Force | Where-Object {
    $_.FullName -notmatch '\\\.git\\' -and $_.Extension -notin '.exe', '.dll', '.zip', '.ipa', '.png', '.jpg', '.ico'
}
$hits = @()
foreach ($f in $files) {
    $lines = [IO.File]::ReadAllLines($f.FullName)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        foreach ($k in $patterns.Keys) {
            if ($lines[$i] -match $patterns[$k]) {
                if ($f.Name -eq 'Test-NoSecrets.ps1' -or $lines[$i] -match 'pgpu-allow-secret') { continue }
                $hits += ('{0}:{1}  [{2}]  {3}' -f $f.FullName.Substring($Root.Length).TrimStart('\'), ($i + 1), $k, $lines[$i].Trim())
            }
        }
    }
}
if ($hits.Count -gt 0) { Write-Host '次の場所に、秘密や個人の物かもしれない文字列があります：'; $hits | ForEach-Object { Write-Host $_ }; exit 1 }
Write-Host ('見つかりませんでした（{0} ファイルを調べた）' -f $files.Count)
exit 0
