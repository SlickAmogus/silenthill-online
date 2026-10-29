# Silent Hill Online - deploy or update the server from Windows, in one click.
# Reads server.txt next to this script, sends the host files in this folder to
# the Linux server over ONE ssh connection (you type its password once) and
# runs install-server.sh there, which builds or installs sh_master, sets up the
# sh1pc service (auto-start, restart after a crash) and restarts it.
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$cfgPath = Join-Path $here 'server.txt'

$cfg = @{}
if (Test-Path $cfgPath) {
    foreach ($line in Get-Content $cfgPath) {
        if ($line -match '^\s*([A-Za-z_]+)\s*=\s*(.*?)\s*$') { $cfg[$matches[1].ToLower()] = $matches[2] }
    }
}

$target = $cfg['host']
if (-not $target) {
    Write-Host "server.txt has no host yet." -ForegroundColor Yellow
    $target = Read-Host "Server to log in to (for example root@1.2.3.4)"
    if (-not $target) { exit 1 }
    if (Test-Path $cfgPath) {
        $text = Get-Content $cfgPath -Raw
        if ($text -match '(?m)^\s*host\s*=.*$') { $text = $text -replace '(?m)^\s*host\s*=.*$', "host=$target" }
        else { $text = $text.TrimEnd() + "`r`nhost=$target`r`n" }
        Set-Content -Path $cfgPath -Value $text -Encoding ascii -NoNewline
    } else {
        Set-Content -Path $cfgPath -Value "host=$target`r`n" -Encoding ascii -NoNewline
    }
    Write-Host "Saved to server.txt."
}
if ($target -notmatch '@') { $target = "root@$target" }
$serverArgs = $cfg['args']
$domain = $cfg['domain']

$required = @('install-server.sh', 'sh_master.c', 'sh_net_proto.h', 'sh_net_memo_text.h', 'sh_net_chat_filter.h')
$optional = @('sh_master-linux-x64', 'sh_master-linux-arm64')
foreach ($f in $required) {
    if (-not (Test-Path (Join-Path $here $f))) { Write-Host "ERROR: $f is missing next to this script" -ForegroundColor Red; exit 1 }
}
if (-not (Get-Command ssh -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: 'ssh' not found. Enable Windows OpenSSH Client in Settings > Optional Features." -ForegroundColor Red
    exit 1
}

function Sq($s) { "'" + ($s -replace "'", "'\''") + "'" }

Write-Host "Packaging host files ..."
$sb = New-Object System.Text.StringBuilder
[void]$sb.Append("set -e`nU=`$HOME/sh1pc-upload`nmkdir -p `"`$U`" && cd `"`$U`"`n")
$i = 0
foreach ($f in $required + $optional) {
    $p = Join-Path $here $f
    if (-not (Test-Path $p)) { continue }
    $b64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($p), 'InsertLineBreaks')
    [void]$sb.Append("base64 -d > $f <<'B64_$i'`n$b64`nB64_$i`n")
    $i++
}
[void]$sb.Append("if [ `"`$(id -u)`" = 0 ]; then SUDO=; else SUDO=sudo; fi`n")
[void]$sb.Append("`$SUDO bash `"`$U/install-server.sh`" --args $(Sq $serverArgs) --domain $(Sq $domain)`n")
$remote = $sb.ToString() -replace "`r`n", "`n"

$tmp = Join-Path $env:TEMP 'sh1pc_deploy.sh'
[IO.File]::WriteAllText($tmp, $remote, (New-Object System.Text.UTF8Encoding($false)))

Write-Host ""
Write-Host "Connecting to $target - enter its password when prompted." -ForegroundColor Cyan
cmd /c "ssh -o StrictHostKeyChecking=accept-new $target `"bash -s`" < `"$tmp`""
$code = $LASTEXITCODE
Remove-Item $tmp -ErrorAction SilentlyContinue

Write-Host ""
if ($code -eq 0) {
    Write-Host "Done - the server on $target is up to date and running." -ForegroundColor Green
} else {
    Write-Host "Deploy FAILED (exit $code). Read the output above for the cause." -ForegroundColor Red
}
exit $code
