param(
    [int] $ReadyTimeoutSeconds = 45
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

function Stop-Listener([int] $Port) {
    $listeners = Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue
    foreach ($listener in $listeners) {
        Stop-Process -Id $listener.OwningProcess -Force -ErrorAction SilentlyContinue
    }
}

# Quit through QEMU's monitor so qcow2/FAT writes are flushed. Forced VM
# termination is deliberately forbidden because it previously lost files.
$monitor = Get-NetTCPConnection -State Listen -LocalPort 4444 -ErrorAction SilentlyContinue
if ($monitor) {
    & (Join-Path $root 'monitor.ps1') 'quit' | Out-Null
    $deadline = (Get-Date).AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 250
        $monitor = Get-NetTCPConnection -State Listen -LocalPort 4444 -ErrorAction SilentlyContinue
    } while ($monitor -and (Get-Date) -lt $deadline)
    if ($monitor) { throw 'QEMU did not quit cleanly; refusing a forced reset.' }
}
Stop-Listener 5555
Start-Sleep -Milliseconds 500

$runner = Join-Path $root 'run.ps1'
Start-Process powershell -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$runner) -WorkingDirectory $root
$deadline = (Get-Date).AddSeconds(15)
do {
    Start-Sleep -Milliseconds 250
    $monitor = Get-NetTCPConnection -State Listen -LocalPort 4444 -ErrorAction SilentlyContinue
} while (-not $monitor -and (Get-Date) -lt $deadline)
if (-not $monitor) { throw 'QEMU monitor did not start.' }

# Select the default boot entry. TCPAGENT is started by FDAUTO.BAT after the
# packet driver, so readiness is the agent connection/PONG rather than sleep.
Start-Sleep -Seconds 2
& (Join-Path $root 'monitor.ps1') 'sendkey ret' | Out-Null
$deadline = (Get-Date).AddSeconds($ReadyTimeoutSeconds)
$ready = $false
do {
    Start-Sleep -Milliseconds 500
    $connection = Get-NetTCPConnection -State Established -LocalPort 5558 -ErrorAction SilentlyContinue
    if ($connection) {
        try {
            $client = [Net.Sockets.TcpClient]::new('127.0.0.1',5555)
            $stream = $client.GetStream()
            $stream.ReadTimeout = 1500
            $bytes = [Text.Encoding]::ASCII.GetBytes("PING`n")
            $stream.Write($bytes,0,$bytes.Length)
            $buffer = New-Object byte[] 128
            $reply = ''
            while ($reply -notmatch "`n") {
                $count = $stream.Read($buffer,0,$buffer.Length)
                if ($count -le 0) { break }
                $reply += [Text.Encoding]::ASCII.GetString($buffer,0,$count)
            }
            $client.Dispose()
            $ready = $reply -match '^OK 504F4E47'
        } catch { $ready = $false }
    }
} while (-not $ready -and (Get-Date) -lt $deadline)
if (-not $ready) { throw 'TCPAGENT did not become ready.' }
Write-Host 'QEMU reset complete; TCPAGENT answered PONG.'
