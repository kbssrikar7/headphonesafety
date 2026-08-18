# install.ps1
#
# Installs Headphone Safety: checks that VB-Audio Virtual Cable is present (required for the
# Real-Time Limiter, not for Volume Cap), copies the built tray binary to a stable per-user
# location, sets up the tray app to start automatically at login, and starts it immediately.
#
# Does NOT require elevation. The shipped architecture (VB-Cable + WASAPI loopback, not a
# driver-level APO) has no TrustedInstaller-owned registry keys to touch and Task Scheduler's
# -RunLevel Limited at-logon trigger works fine unelevated.
#
# Finds HeadphoneSafetyTray.exe in either of two layouts: right next to this script (a
# standalone release zip - see the GitHub Releases page) or under ..\build\tray (a dev checkout
# built via windows\build.ps1). Checked in that order.
#
# Usage (from a normal PowerShell, from the repo root, an unzipped release, or anywhere):
#   .\windows\packaging\install.ps1
#   .\windows\packaging\install.ps1 -SkipVBCableCheck   # if you know it's installed under a
#                                                        # different friendly name
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "HeadphoneSafety"),
    [switch]$SkipVBCableCheck
)

$ErrorActionPreference = "Stop"

$releaseLayoutExe = Join-Path $PSScriptRoot "HeadphoneSafetyTray.exe"
$devLayoutExe = Join-Path $PSScriptRoot "..\build\tray\HeadphoneSafetyTray.exe"
$trayExe = if (Test-Path $releaseLayoutExe) { $releaseLayoutExe } elseif (Test-Path $devLayoutExe) { $devLayoutExe } else { $null }
if (-not $trayExe) {
    throw "HeadphoneSafetyTray.exe not found next to this script or under ..\build\tray - " +
          "if building from source, run .\windows\build.ps1 first."
}

Write-Host "[1/4] Checking for VB-Audio Virtual Cable..."
# Friendly-name match, same heuristic windows/tray/VBCableDetector.cpp uses at runtime - checked
# here too so install doesn't silently succeed into a tray that can never start the Real-Time
# Limiter. Not fatal (Volume Cap works fine without it) - just a clear warning.
$vbCablePresent = Get-PnpDevice -Class AudioEndpoint -Status OK -ErrorAction SilentlyContinue |
    Where-Object { $_.FriendlyName -match 'CABLE Input.*VB-Audio Virtual Cable' } |
    Select-Object -First 1
if ($SkipVBCableCheck) {
    Write-Host "  Skipped (-SkipVBCableCheck)."
} elseif ($vbCablePresent) {
    Write-Host "  Found: $($vbCablePresent.FriendlyName)"
} else {
    Write-Warning "  VB-Audio Virtual Cable not found. Volume Cap will still work, but the"
    Write-Warning "  Real-Time Limiter cannot start until it's installed."
    Write-Warning "  Download it (free) from https://vb-audio.com/Cable/ , then re-run this"
    Write-Warning "  script or just start the tray - it re-checks automatically."
}

Write-Host "[2/4] Installing to $InstallDir ..."
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$installedTrayExe = Join-Path $InstallDir "HeadphoneSafetyTray.exe"
Copy-Item -Path $trayExe -Destination $installedTrayExe -Force
Write-Host "  Copied HeadphoneSafetyTray.exe"

Write-Host "[3/4] Setting up autostart at login..."
$taskName = "HeadphoneSafetyTray"
$action = New-ScheduledTaskAction -Execute $installedTrayExe
$trigger = New-ScheduledTaskTrigger -AtLogOn
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal `
    -Settings $settings -Force | Out-Null
Write-Host "  Scheduled task '$taskName' created (runs at your next login)."

Write-Host "[4/4] Starting the tray now..."
Start-Process -FilePath $installedTrayExe
Write-Host "  Started."

Write-Host ""
Write-Host "Done. Headphone Safety is installed and running."
Write-Host "Volume Cap works immediately on any wired or Bluetooth headphone-classified device."
if ($vbCablePresent -or $SkipVBCableCheck) {
    Write-Host "Turn on the Real-Time Limiter from the tray icon's menu."
} else {
    Write-Host "Install VB-Audio Virtual Cable (see warning above) before the Real-Time Limiter"
    Write-Host "will start."
}
