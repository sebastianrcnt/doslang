param()

$ErrorActionPreference = 'Stop'
$qemu = Get-Command qemu-system-i386.exe -ErrorAction Stop
$node = Get-Command node.exe -ErrorAction Stop
$disk = Join-Path $PSScriptRoot 'freedos.qcow2'
$relay = Join-Path $PSScriptRoot 'tcp-agent-relay.mjs'

if (-not (Test-Path -LiteralPath $disk)) {
    throw "Missing $disk. Run .qemu\\setup.ps1, then .qemu\\install.ps1."
}

$relayListening = Get-NetTCPConnection -State Listen -LocalPort 5558 -ErrorAction SilentlyContinue
if (-not $relayListening) {
    Start-Process -FilePath $node.Source -ArgumentList @($relay) -WorkingDirectory $PSScriptRoot -WindowStyle Hidden
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        Start-Sleep -Milliseconds 100
        $relayListening = Get-NetTCPConnection -State Listen -LocalPort 5558 -ErrorAction SilentlyContinue
    } while (-not $relayListening -and [DateTime]::UtcNow -lt $deadline)
    if (-not $relayListening) { throw 'TCP agent relay did not start.' }
}

$monitorListening = Get-NetTCPConnection -State Listen -LocalPort 4444 -ErrorAction SilentlyContinue
if ($monitorListening) {
    throw 'QEMU monitor port 4444 is already in use. Stop the existing VM before starting another one.'
}

& $qemu.Source `
    -machine pc,accel=whpx,kernel-irqchip=off,usb=on `
    -smp 1 `
    -m 64 `
    -drive "file=$disk,format=qcow2,if=ide,index=0,media=disk" `
    -nic user,model=ne2k_isa `
    -monitor "tcp:127.0.0.1:4444,server=on,wait=off" `
    -boot order=c `
    -display default
