# Builds gtts with MSYS2's UCRT64 toolchain and packs the release zip into dist\.
param([string]$Msys = 'C:\msys64')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
# windows\VERSION, when present, lets a Windows-only fix ship without changing the Mac version.
$versionFile = Join-Path $PSScriptRoot 'VERSION'
if (-not (Test-Path $versionFile)) { $versionFile = Join-Path $root 'native\VERSION' }
$version = (Get-Content $versionFile).Trim()

$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
Push-Location $PSScriptRoot
try {
    & (Join-Path $Msys 'usr\bin\bash.exe') -lc 'make clean && make'
    if ($LASTEXITCODE) { throw 'Build failed.' }
} finally { Pop-Location }

$name = "google-tts-local-$version-windows-x64"
$stage = Join-Path $root "dist\$name"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
foreach ($file in 'gtts.exe', 'gttsw.exe', 'install.ps1', 'NOTICE.md') { Copy-Item (Join-Path $PSScriptRoot $file) $stage }
Copy-Item (Join-Path $root 'LICENSE') $stage
Copy-Item -Recurse (Join-Path $PSScriptRoot 'licenses') $stage
$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $stage -DestinationPath $zip
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
Write-Host "$zip`nSHA-256 $hash"
