param()

$ErrorActionPreference = 'Stop'
$qemuImg = Get-Command qemu-img.exe -ErrorAction Stop
$disk = Join-Path $PSScriptRoot 'freedos.qcow2'
$archive = Join-Path $PSScriptRoot 'FD14-LiveCD.zip'
$iso = Join-Path $PSScriptRoot 'FD14LIVE.iso'
$url = 'https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.4/FD14-LiveCD.zip'

if (-not (Test-Path -LiteralPath $disk)) {
    & $qemuImg.Source create -f qcow2 $disk 2G
    if ($LASTEXITCODE -ne 0) { throw "qemu-img failed with exit code $LASTEXITCODE." }
}

if (-not (Test-Path -LiteralPath $iso)) {
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -Uri $url -OutFile $archive
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $PSScriptRoot -Force
}

if (-not (Test-Path -LiteralPath $iso)) {
    throw 'FreeDOS ISO extraction failed.'
}

Get-Item -LiteralPath $disk,$iso | Select-Object Name,Length,LastWriteTime
