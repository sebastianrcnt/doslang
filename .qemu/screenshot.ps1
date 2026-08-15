param(
    [string]$Name = 'qemu-screen',
    [switch]$KeepPpm
)

$ErrorActionPreference = 'Stop'
$ffmpeg = Get-Command ffmpeg.exe -ErrorAction Stop

$target = if ([IO.Path]::IsPathRooted($Name)) { $Name } else { Join-Path $PSScriptRoot $Name }
$extension = [IO.Path]::GetExtension($target)
if ($extension -in @('.png', '.ppm')) {
    $target = [IO.Path]::Combine([IO.Path]::GetDirectoryName($target), [IO.Path]::GetFileNameWithoutExtension($target))
}
$directory = [IO.Path]::GetDirectoryName($target)
if ($directory) { New-Item -ItemType Directory -Force -Path $directory | Out-Null }

$ppm = "$target.ppm"
$png = "$target.png"
$qemuPath = $ppm.Replace('\', '/')

& (Join-Path $PSScriptRoot 'monitor.ps1') "screendump $qemuPath" | Out-Null
if (-not (Test-Path -LiteralPath $ppm)) { throw 'QEMU did not create the requested screenshot.' }
& $ffmpeg.Source -y -loglevel error -i $ppm $png
if ($LASTEXITCODE -ne 0) { throw "ffmpeg conversion failed with exit code $LASTEXITCODE." }
if (-not $KeepPpm) { Remove-Item -LiteralPath $ppm -Force }
Get-Item -LiteralPath $png
