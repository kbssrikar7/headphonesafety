# install.ps1
#
# Installs Headphone Safety: copies the built binaries to a stable per-user location, registers
# the Real-Time Limiter APO against every currently-present headphone-like output device (see
# docs/windows-port.md's "Known blocker" section - registration is confirmed correct, but whether
# it actually takes audible effect depends on your specific audio driver, not something this
# script can fix), and sets up the tray app to start automatically at login.
#
# Must run elevated (writes under HKEY_LOCAL_MACHINE via register-apo.ps1, and Task Scheduler task
# creation for reliable at-logon startup). Run windows/build.ps1 first if you have not already -
# this script installs from windows/build/, it does not build anything itself.
#
# Usage (from an elevated PowerShell, from the repo root or anywhere):
#   .\windows\packaging\install.ps1
[CmdletBinding()]
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "HeadphoneSafety")
)

$ErrorActionPreference = "Stop"

function Test-IsElevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
}

if (-not (Test-IsElevated)) {
    throw "install.ps1 must run elevated. Re-run from an admin PowerShell."
}

$apoDll = Join-Path $BuildDir "apo\HeadphoneSafetyApo.dll"
$trayExe = Join-Path $BuildDir "tray\HeadphoneSafetyTray.exe"
if (-not (Test-Path $apoDll) -or -not (Test-Path $trayExe)) {
    throw "Built binaries not found under $BuildDir - run .\windows\build.ps1 first."
}

Write-Host "[1/4] Installing to $InstallDir ..."
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$installedApoDll = Join-Path $InstallDir "HeadphoneSafetyApo.dll"
$installedTrayExe = Join-Path $InstallDir "HeadphoneSafetyTray.exe"
Copy-Item -Path $apoDll -Destination $installedApoDll -Force
Copy-Item -Path $trayExe -Destination $installedTrayExe -Force
Write-Host "  Copied HeadphoneSafetyApo.dll and HeadphoneSafetyTray.exe"

Write-Host "[2/4] Registering the Real-Time Limiter on headphone-like output devices..."
# Simple friendly-name heuristic, not the full PKEY_AudioEndpoint_FormFactor + Bluetooth-name
# check windows/tray/VolumeCap.cpp does via COM - good enough for "which devices to register the
# APO against at install time," since registering against a non-headphone device is harmless (it
# only takes effect if the user later also toggles the Real-Time Limiter on in the tray, which
# they would only do intentionally). A newly-paired Bluetooth device after this point still needs
# one more elevated registration pass - see docs/windows-port.md and the plan's Open Risks for why
# this is a real, accepted limitation of the classic (non-HSA/UWP) APO model, not an oversight.
$registerScript = Join-Path $PSScriptRoot "..\apo\register\register-apo.ps1"
$headphoneEndpoints = Get-PnpDevice -Class AudioEndpoint -Status OK -ErrorAction SilentlyContinue |
    Where-Object {
        $_.InstanceId -match '\{[^}]+\}\.\{([0-9a-fA-F-]{36})\}$' -and
        $_.FriendlyName -match 'Headphone|Headset|Hands-Free|Hands Free|Bluetooth'
    } |
    ForEach-Object {
        [PSCustomObject]@{ Name = $_.FriendlyName; Guid = $Matches[1] }
    }

if ($headphoneEndpoints.Count -eq 0) {
    Write-Host "  No headphone-like output devices currently present - nothing to register yet."
    Write-Host "  Connect headphones, then re-run this script (or register-apo.ps1 directly) to"
    Write-Host "  protect that specific device."
} else {
    foreach ($endpoint in $headphoneEndpoints) {
        Write-Host "  Registering against: $($endpoint.Name)"
        try {
            & $registerScript -EndpointGuid $endpoint.Guid -DllPath $installedApoDll
        } catch {
            Write-Warning "  Failed to register against $($endpoint.Name): $($_.Exception.Message)"
        }
    }
}

Write-Host "[3/4] Setting up autostart at login..."
# A Task Scheduler "at logon" trigger, not just a HKCU Run key - Task Scheduler triggers can run
# slightly earlier and more reliably (per docs/windows-port.md's guidance on the shared-memory
# IPC's known ordering limitation: the earlier the tray starts, the less likely audiodg.exe builds
# its stream graph before the tray's shared-memory mapping exists).
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
Write-Host "The Real-Time Limiter is registered on the device(s) listed above, but whether it has"
Write-Host "an audible effect depends on your audio driver - see docs/windows-port.md's 'Known"
Write-Host "blocker' section if you plan to rely on it. Toggle it from the tray icon's menu."
