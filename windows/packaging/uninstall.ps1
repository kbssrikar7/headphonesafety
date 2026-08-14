# uninstall.ps1
#
# Reverses install.ps1: stops the running tray process, removes the autostart scheduled task,
# unregisters the Real-Time Limiter APO from every endpoint install.ps1 (or a manual
# register-apo.ps1 run) registered it against, and removes the installed binaries. Settings under
# HKCU\Software\HeadphoneSafety are left in place unless -RemoveSettings is passed, so a
# reinstall later picks up the same headroom/enabled preferences.
#
# Must run elevated (writes under HKEY_LOCAL_MACHINE via unregister-apo.ps1, and removes the
# Task Scheduler task).
#
# Usage (from an elevated PowerShell):
#   .\windows\packaging\uninstall.ps1
#   .\windows\packaging\uninstall.ps1 -RemoveSettings
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "HeadphoneSafety"),
    [switch]$RemoveSettings
)

$ErrorActionPreference = "Stop"

function Test-IsElevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
}

if (-not (Test-IsElevated)) {
    throw "uninstall.ps1 must run elevated. Re-run from an admin PowerShell."
}

Write-Host "[1/4] Stopping the running tray process (if any)..."
Get-Process -Name "HeadphoneSafetyTray" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
Write-Host "  Done."

Write-Host "[2/4] Removing the autostart scheduled task..."
$taskName = "HeadphoneSafetyTray"
if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    Write-Host "  Removed."
} else {
    Write-Host "  No scheduled task found - nothing to remove."
}

Write-Host "[3/4] Unregistering the Real-Time Limiter from every endpoint it was attached to..."
$unregisterScript = Join-Path $PSScriptRoot "..\apo\register\unregister-apo.ps1"
$installedApoDll = Join-Path $InstallDir "HeadphoneSafetyApo.dll"
$trackingPath = "HKCU:\Software\HeadphoneSafety\RegisteredEndpoints"
if (Test-Path $trackingPath) {
    $tracked = Get-ItemProperty -Path $trackingPath -ErrorAction SilentlyContinue
    $endpointGuids = if ($tracked) {
        $tracked.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object { $_.Name }
    } else { @() }

    if ($endpointGuids.Count -eq 0) {
        Write-Host "  No tracked endpoints - nothing to unregister."
    } else {
        foreach ($guid in $endpointGuids) {
            Write-Host "  Unregistering from endpoint {$guid}..."
            try {
                & $unregisterScript -EndpointGuid $guid -DllPath $installedApoDll
            } catch {
                Write-Warning "  Failed to unregister {$guid}: $($_.Exception.Message)"
            }
        }
    }
} else {
    Write-Host "  No tracking key found - nothing to unregister."
}

Write-Host "[4/4] Removing installed binaries..."
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
Write-Host "Done. Stop-Process -Name audiodg -Force to make the audio engine immediately drop the"
Write-Host "unregistered APO (it respawns automatically on next playback) - not required, but"
Write-Host "otherwise the currently-running audiodg.exe instance keeps whatever it already loaded"
Write-Host "until it next rebuilds its stream graph on its own."
