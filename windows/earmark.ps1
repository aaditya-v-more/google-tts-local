<#
.SYNOPSIS
Adds google-tts-local's voices to Earmark, the read-aloud book reader.

.DESCRIPTION
Registers a plugin file in Earmark's plugins folder (%APPDATA%\Earmark\Plugins), following
Earmark's plugin contract: one JSON file per plugin. The voices then appear under Google in
Earmark's voice menu. Earmark starts the background server (gtts service install) itself the
first time one of the voices is used; run gtts fetch first.

.EXAMPLE
.\earmark.ps1
.\earmark.ps1 -Uninstall
.\earmark.ps1 -Status
#>
param(
    [string]$App = 'Earmark',  # a development build is "Earmark Dev"
    [int]$Port = 5591,
    [string]$Gtts,             # gtts.exe; default: next to this script, else the installed one
    [switch]$Uninstall,        # remove the voices from Earmark; gtts stays installed
    [switch]$Status
)
$ErrorActionPreference = 'Stop'
if ($App -eq '' -or $App -match '[\\/]') { throw 'The app name cannot be empty or contain \ or /.' }
if ($Port -lt 1 -or $Port -gt 65535) { throw 'Port must be from 1 to 65535.' }
$folder = Join-Path $env:APPDATA "$App\Plugins"
$file = Join-Path $folder 'google-tts-local.json'
$url = "http://127.0.0.1:$Port"

if ($Status) {
    if (Test-Path $file) { Write-Host "Registered with ${App}: $file" } else { Write-Host "Not registered with $App." }
    try { Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 "$url/health" | Out-Null; Write-Host "The server is running at $url." }
    catch { Write-Host "The server is not running at $url." }
    return
}
if ($Uninstall) {
    if (Test-Path $file) { Remove-Item -Force $file }
    if ((Test-Path $folder) -and -not (Get-ChildItem $folder -Force)) { Remove-Item $folder }
    Write-Host "Removed the voices from $App. gtts and its voices are still installed."
    return
}

if (-not $Gtts) {
    foreach ($candidate in (Join-Path $PSScriptRoot 'gtts.exe'), (Join-Path $env:LOCALAPPDATA 'Programs\google-tts-local\gtts.exe')) {
        if (Test-Path $candidate) { $Gtts = $candidate; break }
    }
}
$plugin = [ordered]@{ name = 'Google'; url = "$url/speak"; voices = "$url/voices"; speed = 'engine' }
if ($Gtts -and (Test-Path $Gtts)) {
    $plugin.start = @((Resolve-Path $Gtts).Path, 'service', 'install', '--port', "$Port")
}
New-Item -ItemType Directory -Force $folder | Out-Null
# Written whole and then renamed, so Earmark never reads half of it.
$temp = Join-Path $folder ".google-tts-local.$PID.tmp"
[IO.File]::WriteAllText($temp, ($plugin | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
Move-Item -Force $temp $file
Write-Host "Registered the voices with ${App}: $file"
Write-Host "They appear under Google in its voice menu, for the book's language, when $App next comes to the front."
if (-not $plugin.Contains('start')) { Write-Host 'gtts.exe was not found: run gtts service install so they can speak.' }
