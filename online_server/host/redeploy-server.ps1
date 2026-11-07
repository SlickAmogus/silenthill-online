# Silent Hill Online - deploy or update the server from Windows, in one click.
# Reads server.txt next to this script, sends the host files in this folder to
# the Linux server over ONE ssh connection (you type its password once) and
# runs install-server.sh there, which builds or installs sh_master, sets up the
# sh1pc service (auto-start, restart after a crash) and restarts it. With
# players online it lists them and asks first: continue warns them and
# restarts after 30 seconds, stop leaves the server exactly as it was.
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

# Runs the remote script over one ssh connection. When players are online the
# script stops at a question: it prints who is on, then a line "@@ASK <n>",
# and reads the answer from its standard input (the same stream the script
# arrives on; bash -s reads that a byte at a time, so the answer lands right
# after the line that asked). With nobody on it prints "@@GO" instead. So the
# script goes in in two parts, the output is shown as it arrives, and the
# answer is written in between. Returns the remote exit code (3 = stopped).
function Invoke-Remote([string]$exe, [string]$exeArgs, [string]$part1, [string]$part2) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = $exeArgs
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $p = [System.Diagnostics.Process]::Start($psi)
    $in = $p.StandardInput.BaseStream
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $send = {
        param([string]$text)
        $b = $utf8.GetBytes(($text -replace "`r`n", "`n"))
        $in.Write($b, 0, $b.Length)
        $in.Flush()
    }
    & $send $part1

    $out  = $p.StandardOutput
    $line = New-Object System.Text.StringBuilder
    $hold = $false
    $done = $false
    while (($c = $out.Read()) -ge 0) {
        $ch = [char]$c
        if ($ch -ne "`n") {
            if ($line.Length -eq 0) { $hold = ($ch -eq '@') }
            [void]$line.Append($ch)
            # A line starting with @ may be a marker: held until it ends.
            # Everything else (a password prompt has no newline) shows at once.
            if (-not $hold) { [Console]::Out.Write($ch) }
            continue
        }
        $l = $line.ToString().TrimEnd("`r")
        $held = $hold
        $hold = $false
        $line.Length = 0
        if (-not $done -and $l -match '^@@ASK (\d+)') {
            Write-Host ""
            Write-Host "$($matches[1]) player(s) online (listed above)." -ForegroundColor Yellow
            Write-Host "  C = continue: warn them in game, wait 30 seconds, restart with the new version"
            Write-Host "  S = stop: change nothing; the server keeps running the version it has"
            do { $ans = (Read-Host "Continue or stop? (C/S)").Trim().ToLower() } while ($ans -ne 'c' -and $ans -ne 's')
            if ($ans -eq 'c') { & $send ("c`n" + $part2) } else { & $send "s`n" }
            $in.Close()
            $done = $true
            continue
        }
        if (-not $done -and $l -eq '@@GO') {
            & $send $part2
            $in.Close()
            $done = $true
            continue
        }
        if ($held) { [Console]::Out.Write($l) }
        [Console]::Out.Write("`n")
    }
    if (-not $done) { try { $in.Close() } catch {} }
    $p.WaitForExit()
    return $p.ExitCode
}

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

Write-Host ""
Write-Host "Connecting to $target - enter its password when prompted." -ForegroundColor Cyan
# install-server.sh asks its question (if anyone is on) in the middle; the
# whole script is part 1 and the answer is all that follows it.
$code = Invoke-Remote 'ssh' "-o StrictHostKeyChecking=accept-new -o ServerAliveInterval=20 $target `"bash -s`"" $remote ''

Write-Host ""
if ($code -eq 0) {
    Write-Host "Done - the server on $target is up to date and running." -ForegroundColor Green
} elseif ($code -eq 3) {
    Write-Host "Stopped - nothing was changed; the server keeps running the version it had." -ForegroundColor Yellow
    $code = 0
} else {
    Write-Host "Deploy FAILED (exit $code). Read the output above for the cause." -ForegroundColor Red
}
exit $code
