# Runs a script with UTF-8 console output, so the tests can read Japanese text on an English Windows too.
#   utf8-run.ps1 <script.ps1> [args...]
$script = $args[0]
$rest = @($args | Select-Object -Skip 1)
[Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
& $script @rest
exit $LASTEXITCODE
