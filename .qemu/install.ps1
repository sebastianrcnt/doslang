param()

$ErrorActionPreference = 'Stop'
$qemu = Get-Command qemu-system-i386.exe -ErrorAction Stop
$disk = Join-Path $PSScriptRoot 'freedos.qcow2'
$iso = Join-Path $PSScriptRoot 'FD14LIVE.iso'

if (-not (Test-Path -LiteralPath $disk)) {
    throw "Missing $disk. Create it first with: qemu-img create -f qcow2 .qemu\\freedos.qcow2 2G"
}
if (-not (Test-Path -LiteralPath $iso)) {
    throw "Missing $iso. Run .qemu\\setup.ps1 first."
}
if (Get-NetTCPConnection -State Listen -LocalPort 4444 -ErrorAction SilentlyContinue) {
    throw 'QEMU monitor port 4444 is already in use. Stop the existing VM first.'
}

& $qemu.Source `
    -machine pc,usb=on `
    -cpu pentium3 `
    -m 64 `
    -drive "file=$disk,format=qcow2,if=ide,index=0,media=disk" `
    -drive "file=$iso,media=cdrom,readonly=on" `
    -nic user,model=ne2k_isa `
    -monitor "tcp:127.0.0.1:4444,server=on,wait=off" `
    -serial null `
    -boot order=d `
    -display default
