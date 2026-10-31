<#
.SYNOPSIS
Installs gtts for the current user.

.DESCRIPTION
Copies gtts.exe and gttsw.exe to %LOCALAPPDATA%\Programs\google-tts-local (or
-Prefix) and adds that folder to your user PATH. Nothing is installed for other
users and no administrator rights are needed.

.EXAMPLE
.\install.ps1 -Fetch -StartAtLogin
.\install.ps1 -Uninstall
#>
param(
    [string]$Prefix = (Join-Path $env:LOCALAPPDATA 'Programs\google-tts-local'),
    [switch]$Fetch,          # download the engine and voices after installing
    [switch]$StartAtLogin,   # run gtts serve in the background when you sign in
    [switch]$Uninstall       # remove the program; downloaded voices are kept
)
$ErrorActionPreference = 'Stop'
$source = $PSScriptRoot

function Get-UserPath { [Environment]::GetEnvironmentVariable('Path', 'User') }
function Set-UserPath([string]$value) { [Environment]::SetEnvironmentVariable('Path', $value, 'User') }
function Split-PathList([string]$list) { if ($list) { $list -split ';' | Where-Object { $_ } } else { @() } }

if ($Uninstall) {
    $gtts = Join-Path $Prefix 'gtts.exe'
    if (Test-Path $gtts) { & $gtts service uninstall }
    $kept = Split-PathList (Get-UserPath) | Where-Object { $_.TrimEnd('\') -ne $Prefix.TrimEnd('\') }
    Set-UserPath ($kept -join ';')
    if (Test-Path $Prefix) { Remove-Item -Recurse -Force $Prefix }
    Write-Host "Removed $Prefix. Downloaded data is kept in $env:LOCALAPPDATA\google-tts-local."
    return
}

foreach ($file in 'gtts.exe', 'gttsw.exe') {
    if (-not (Test-Path (Join-Path $source $file))) { throw "$file is missing next to install.ps1." }
}
# A running background server holds gttsw.exe open; stop it before replacing it.
$running = Get-Process gttsw -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $Prefix 'gttsw.exe') }
$wasRunning = [bool]$running
if ($running) { $running | Stop-Process -Force; Start-Sleep -Milliseconds 300 }

New-Item -ItemType Directory -Force $Prefix | Out-Null
foreach ($file in 'gtts.exe', 'gttsw.exe', 'LICENSE', 'NOTICE.md') {
    $from = Join-Path $source $file
    if (Test-Path $from) { Copy-Item -Force $from $Prefix }
}
$licenses = Join-Path $source 'licenses'
if (Test-Path $licenses) { Copy-Item -Recurse -Force $licenses $Prefix }

$paths = Split-PathList (Get-UserPath)
if ($paths -notcontains $Prefix) {
    Set-UserPath ((@($paths) + $Prefix) -join ';')
    Write-Host "Added $Prefix to your PATH. Open a new terminal to use gtts."
}
$env:Path = "$Prefix;$env:Path"
$gtts = Join-Path $Prefix 'gtts.exe'
Write-Host "Installed $(& $gtts --version) to $Prefix"

if ($Fetch) { & $gtts fetch; if ($LASTEXITCODE) { exit $LASTEXITCODE } }
if ($StartAtLogin -or $wasRunning) { & $gtts service install; if ($LASTEXITCODE) { exit $LASTEXITCODE } }
if (-not $Fetch) { Write-Host 'Next: gtts fetch' }
