# uninstall.ps1
#
# Reverses install.ps1: stops the running tray process, removes the autostart scheduled task, and
# removes the installed binaries. Settings under HKCU\Software\HeadphoneSafety are left in place
# unless -RemoveSettings is passed, so a reinstall later picks up the same headroom/enabled
# preferences.
#
# Does NOT require elevation - matches install.ps1 (no APO/registry-level registration to undo
# for the shipped VB-Cable + WASAPI architecture). If you separately, manually registered the
# parked apo/ approach via apo\register\register-apo.ps1, unregister it yourself via
# apo\register\unregister-apo.ps1 (elevated) - this script does not touch that.
#
# Usage (from a normal PowerShell):
#   .\windows\packaging\uninstall.ps1
#   .\windows\packaging\uninstall.ps1 -RemoveSettings
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "HeadphoneSafety"),
    [switch]$RemoveSettings
)

$ErrorActionPreference = "Stop"

Write-Host "[1/3] Stopping the running tray process (if any)..."
Get-Process -Name "HeadphoneSafetyTray" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
Write-Host "  Done."

Write-Host "[2/3] Removing the autostart scheduled task..."
$taskName = "HeadphoneSafetyTray"
if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    Write-Host "  Removed."
} else {
    Write-Host "  No scheduled task found - nothing to remove."
}

Write-Host "[3/3] Removing installed binaries..."
if (Test-Path $InstallDir) {
    Remove-Item -Path $InstallDir -Recurse -Force
    Write-Host "  Removed $InstallDir"
} else {
    Write-Host "  $InstallDir does not exist - nothing to remove."
}

if ($RemoveSettings) {
    Write-Host ""
    Write-Host "Removing settings (HKCU\Software\HeadphoneSafety)..."
    Remove-Item -Path "HKCU:\Software\HeadphoneSafety" -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "Done."
Write-Host "Note: if the Real-Time Limiter was active, the OS default output device may still be"
Write-Host "set to VB-Cable (the tray reverts this on a clean quit, but Stop-Process above is a"
Write-Host "force-kill). If your audio seems silent, open Sound settings and pick your real device"
Write-Host "as the default output."
