<#
Start / stop / inspect the ESP8266 lyrics daemon on Windows, and refuse to
start it wrong. The PowerShell twin of tools/lyrics.sh -- same three guards,
same reasons:

  1. A second daemon started on top of a running one dies on the bound port 8766
     while the old one keeps talking to the board.
  2. A daemon started without --insecure connects and never renders a frame:
     this board has no heap for the SEC2 record layer.
  3. An unreachable board looks exactly like a broken daemon from this side.

So: guard on the port, force --insecure, and say what the board itself thinks.

  powershell -ExecutionPolicy Bypass -File tools\lyrics.ps1 [start] [daemon args...]
  powershell -ExecutionPolicy Bypass -File tools\lyrics.ps1 start -f
  powershell -ExecutionPolicy Bypass -File tools\lyrics.ps1 status
  powershell -ExecutionPolicy Bypass -File tools\lyrics.ps1 stop

Windows PowerShell 5.1 syntax only (no ?? / && / ternaries): it is the one that
ships with Windows.
#>

$ErrorActionPreference = 'Stop'
$Daemon = (Resolve-Path (Join-Path $PSScriptRoot '..\daemon\lyrics_display_daemon.py') -ErrorAction SilentlyContinue).Path
function EnvOr([string]$name, [string]$default) {
    $value = [Environment]::GetEnvironmentVariable($name)
    if ([string]::IsNullOrEmpty($value)) { return $default }
    return $value
}
$BoardName = EnvOr 'G4PYS_LYRICS_BOARD_NAME' 'clawdmeter.local'
$BoardPort = [int](EnvOr 'G4PYS_LYRICS_BOARD_PORT' '8766')
$ExtPort = [int](EnvOr 'G4PYS_LYRICS_EXTENSION_PORT' '8765')
$Log = EnvOr 'G4PYS_LYRICS_LOG' (Join-Path $env:LOCALAPPDATA 'g4pys\lyrics-display.log')

# The py launcher is what python.org installs; fall back to python on PATH.
$PyExe = EnvOr 'G4PYS_LYRICS_PYTHON' ''
$PyArgs = @()
if (-not $PyExe) {
    if (Get-Command py -ErrorAction SilentlyContinue) { $PyExe = 'py'; $PyArgs = @('-3') }
    else { $PyExe = 'python' }
}

function Say([string]$text) { Write-Host $text }
function Head([string]$text) { Write-Host $text -ForegroundColor White }
function Good([string]$text) { Write-Host '  + ' -ForegroundColor Green -NoNewline; Write-Host $text }
function Bad([string]$text) { Write-Host '  ! ' -ForegroundColor Red -NoNewline; Write-Host $text }
function Warn([string]$text) { Write-Host '  ~ ' -ForegroundColor Yellow -NoNewline; Write-Host $text }
function Note([string]$text) { Write-Host "    $text" -ForegroundColor DarkGray }

# Keyed off the listening socket, not a process name: the bound port is what
# the board dials.
function Get-DaemonPid([int]$port) {
    $conn = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($conn) { return [int]$conn.OwningProcess }
    return $null
}

function Get-CommandLine([int]$processId) {
    $proc = Get-CimInstance Win32_Process -Filter "ProcessId=$processId" -ErrorAction SilentlyContinue
    if ($proc) { return [string]$proc.CommandLine }
    return ''
}

# IPv4 only: a .local name with no AAAA record otherwise waits out the IPv6
# mDNS timeout before returning the A record it already had.
function Resolve-Board {
    try {
        $addr = [System.Net.Dns]::GetHostAddresses($BoardName) |
            Where-Object { $_.AddressFamily -eq 'InterNetwork' } | Select-Object -First 1
        if ($addr) { return $addr.IPAddressToString }
    } catch { }
    return $null
}

# An open stream socket from the board is what tells "connected, idle" apart
# from "never learned this PC's address": both report lyr=0.
function Test-BoardLinked([string]$ip) {
    $conn = Get-NetTCPConnection -LocalPort $BoardPort -State Established -ErrorAction SilentlyContinue |
        Where-Object { $_.RemoteAddress -eq $ip -or $_.RemoteAddress -eq "::ffff:$ip" }
    return [bool]$conn
}

# Prints the board's own view of itself; returns @{ Ok; Lyr }.
function Get-BoardReport([string]$ip, [switch]$Quiet) {
    $linked = Test-BoardLinked $ip
    try {
        $d = Invoke-RestMethod -Uri "http://$ip/usage.json" -TimeoutSec 4
    } catch {
        if (-not $Quiet) {
            Bad "board at $ip did not answer /usage.json"
            Note 'powered off, on another network, or wedged -- power-cycle it'
        }
        return @{ Ok = $false; Lyr = $null }
    }
    $lyr = $d.lyr
    if ($lyr -eq 0) {
        if ($linked) { $state = 'connected, idle (nothing playing)' } else { $state = 'NOT connected (no PC address known)' }
    } elseif ($lyr -eq 1) { $state = 'connecting' }
    elseif ($lyr -eq 2) { $state = 'streaming' }
    else { $state = '?' }
    $up = [int]$d.up
    $line = 'wifi {0} ({1} dBm) - {2} - heap {3}k - up {4}h{5:00}m - boot {6}' -f `
        $d.ssid, $d.rssi, $state, [int]([int]$d.heap / 1024), [math]::Floor($up / 3600), [math]::Floor(($up % 3600) / 60), $d.rst
    if ([int]$d.wifidown) { $line += " - WIFI DOWN $([int]$d.wifidown)s (watchdog reconnecting)" }
    if ([int]$d.wifidrops) { $line += " - $([int]$d.wifidrops) wifi drop(s) since boot" }
    if ($null -ne $d.mdnsok -and $d.mdnsok -eq 0) { $line += ' - mDNS refresh FAILED (.local may be dark; use --announce-url http://<ip>)' }
    # Healthy is the link being up: streaming, or idle while connected.
    $ok = ($lyr -eq 2) -or ($lyr -eq 0 -and $linked)
    if (-not $Quiet) {
        if ($ok) { Good "board $line" } elseif ($lyr -eq 1) { Warn "board $line" } else { Bad "board $line" }
    }
    return @{ Ok = $ok; Lyr = $lyr }
}

function Invoke-Status {
    Head 'daemon'
    $daemonPid = Get-DaemonPid $BoardPort
    if ($daemonPid) {
        Good "listening on :$BoardPort (pid $daemonPid)"
        if ((Get-CommandLine $daemonPid) -notmatch '--insecure') {
            Bad "running WITHOUT --insecure -- the board's socket will connect and never render"
            Note 'stop it and start it with this script'
        }
        if (Get-DaemonPid $ExtPort) {
            $attached = Get-NetTCPConnection -LocalPort $ExtPort -State Established -ErrorAction SilentlyContinue
            if ($attached) { Good "browser extension attached on :$ExtPort" }
            else { Warn "nothing attached on :$ExtPort -- open music.youtube.com" }
        }
    } else {
        Bad "nothing listening on :$BoardPort -- the daemon is not running"
    }
    Say ''
    Head 'board'
    $ip = Resolve-Board
    if (-not $ip) {
        Bad "cannot resolve $BoardName"
        Note 'mDNS is quiet: the board may be off, or on a network that blocks multicast'
        Note 'set G4PYS_LYRICS_BOARD_NAME to the board IP to skip mDNS'
        return 1
    }
    Good "$BoardName resolves to $ip"
    $report = Get-BoardReport $ip
    if ($report.Ok) { return 0 }
    return 1
}

function Invoke-Stop {
    $daemonPid = Get-DaemonPid $BoardPort
    if (-not $daemonPid) { Say "nothing listening on :$BoardPort -- already stopped."; return 0 }
    # Never kill on the port alone: another board's daemon may own it.
    $cmd = Get-CommandLine $daemonPid
    if ($cmd -notmatch 'lyrics_display_daemon\.py') {
        Bad "pid $daemonPid holds :$BoardPort but is not a lyrics daemon -- refusing to kill it"
        Note $cmd
        return 1
    }
    # Windows has no SIGTERM to send a console-less process; Stop-Process is the
    # only lever. The daemon keeps no state that a hard stop loses (the lyrics
    # cache is SQLite and mDNS is off by default).
    Stop-Process -Id $daemonPid -Force
    for ($i = 0; $i -lt 20; $i++) {
        if (-not (Get-DaemonPid $BoardPort)) { Say "stopped (was pid $daemonPid)."; return 0 }
        Start-Sleep -Milliseconds 250
    }
    Bad "pid $daemonPid still holds :$BoardPort"
    return 1
}

# Windows PowerShell 5.1 turns any stderr from a native command into a
# terminating error under ErrorActionPreference=Stop, so relax it for the probe.
function Test-RendererPackages {
    $ErrorActionPreference = 'Continue'
    & $PyExe @PyArgs -c 'import PIL, uharfbuzz, freetype' 2>&1 | Out-Null
    return ($LASTEXITCODE -eq 0)
}

function Invoke-Start([string[]]$argv) {
    $foreground = $false
    $extra = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $argv.Count; $i++) {
        $a = $argv[$i]
        # A --board-port passed through moves the port this script guards on.
        if ($a -eq '-f' -or $a -eq '--foreground') { $foreground = $true; continue }
        if ($a -eq '--board-port' -and $i + 1 -lt $argv.Count) {
            $script:BoardPort = [int]$argv[$i + 1]
            $extra.Add($a); $extra.Add($argv[$i + 1]); $i++; continue
        }
        if ($a -like '--board-port=*') { $script:BoardPort = [int]$a.Substring(13) }
        $extra.Add($a)
    }

    $daemonPid = Get-DaemonPid $BoardPort
    if ($daemonPid) {
        Bad "already running: pid $daemonPid holds :$BoardPort"
        Note (Get-CommandLine $daemonPid)
        Note "use 'tools\lyrics.ps1 status', or 'stop' first"
        return 1
    }
    if (-not $Daemon) { Bad 'daemon not found next to this script (..\daemon\lyrics_display_daemon.py)'; return 1 }

    Head 'pre-flight'
    if (-not (Test-RendererPackages)) {
        Bad "the Pillow renderer's packages are missing -- the panel would show no text"
        $req = Join-Path (Split-Path $Daemon) 'requirements-windows.txt'
        Note "$PyExe $($PyArgs -join ' ') -m pip install -r `"$req`""
        return 1
    }
    Good 'renderer packages installed'
    $ip = Resolve-Board
    if ($ip) {
        Good "$BoardName resolves to $ip"
        [void](Get-BoardReport $ip)
    } else {
        Warn "cannot resolve $BoardName -- starting anyway, the announcer keeps retrying"
        Note 'if the board has a fixed address, pass --announce-url http://<ip>'
    }

    # --insecure is forced for this board; passing it twice is harmless.
    $daemonArgs = @($PyArgs) + @("$Daemon", 'serve', '--insecure') + @($extra)
    # Unbuffered, or the log reads empty for the whole session.
    $env:PYTHONUNBUFFERED = '1'
    $env:PYTHONIOENCODING = 'utf-8'
    Say ''
    Head 'starting'
    if ($foreground) {
        Note 'foreground: Ctrl+C stops the daemon and the board falls back within 8s'
        # Out-Host streams the output live; without it the function would hold
        # every line as its return value until the daemon exits.
        $ErrorActionPreference = 'Continue'
        & $PyExe @daemonArgs | Out-Host
        return $LASTEXITCODE
    }

    New-Item -ItemType Directory -Force -Path (Split-Path $Log) | Out-Null
    Add-Content -Path $Log -Value ("`r`n===== {0} : lyrics.ps1 start =====" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) -Encoding UTF8
    # cmd does the appending redirect; Start-Process can only truncate. /s strips
    # the outer quotes so every inner quoted path survives.
    $inner = (@($PyExe) + $daemonArgs | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -join ' '
    $cmdLine = '/d /s /c "' + $inner + ' >> "' + $Log + '" 2>&1"'
    $child = Start-Process -FilePath $env:ComSpec -ArgumentList $cmdLine -WindowStyle Hidden -PassThru

    # Did it survive its own startup?
    $up = $false
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 250
        if ($child.HasExited) {
            Bad 'daemon exited during startup'
            Note "last lines of ${Log}:"
            Get-Content $Log -Tail 12 -Encoding UTF8 | ForEach-Object { Note $_ }
            return 1
        }
        if (Get-DaemonPid $BoardPort) { $up = $true; break }
    }
    if (-not $up) {
        Bad "daemon is alive but never bound :$BoardPort"
        Get-Content $Log -Tail 12 -Encoding UTF8 | ForEach-Object { Note $_ }
        return 1
    }
    Good "daemon up (pid $(Get-DaemonPid $BoardPort)), logging to $Log"
    Note 'the first start may raise a Windows Firewall prompt: allow Private networks, or the board cannot dial in'

    if (-not $ip) { Warn 'board was unreachable at pre-flight; not waiting for it'; return 0 }
    Say ''
    Head 'waiting for the board'
    for ($i = 0; $i -lt 12; $i++) {
        Start-Sleep -Seconds 2
        $report = Get-BoardReport $ip -Quiet
        if ($report.Ok) {
            [void](Get-BoardReport $ip)
            Say ''
            if ($report.Lyr -eq 2) { Write-Host '  lyrics are on the panel.' -ForegroundColor Green }
            else { Write-Host '  board is connected. Play something and the panel follows.' -ForegroundColor Green }
            return 0
        }
    }
    [void](Get-BoardReport $ip)
    Say ''
    Warn 'board has not connected after ~24s'
    Note "check $Log for 'board announce reached' and 'board connected'"
    Note 'no "board connected" line usually means Windows Firewall is blocking inbound TCP 8766'
    return 1
}

$command = 'start'
$rest = @($args)
if ($rest.Count -gt 0 -and @('start', 'stop', 'status') -contains $rest[0]) {
    $command = $rest[0]
    $rest = @($rest | Select-Object -Skip 1)
}
switch ($command) {
    'status' { exit (Invoke-Status) }
    'stop' { exit (Invoke-Stop) }
    default { exit (Invoke-Start $rest) }
}
