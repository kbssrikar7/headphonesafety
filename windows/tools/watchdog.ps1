# watchdog.ps1
#
# Hard-won lesson #6 (docs/windows-port.md, from the macOS build): "an independent, out-of-process
# safety net during development." While iterating on and testing the Real-Time Limiter live, it is
# valuable to have a SEPARATE process (not part of the app under test) that force-reverts the
# system default audio output away from VB-Cable after a timeout regardless of what the tray's own
# recovery logic is doing - this catches cases where the app's own recovery logic is broken (e.g.
# the tray process is hung), without leaving a developer stuck listening to nothing for an
# extended period during testing.
#
# Approach B's limiter runs in-process inside HeadphoneSafetyTray.exe (unlike Approach A's
# APO, which ran inside audiodg.exe and was controlled via a shared-memory flag). So this
# watchdog no longer writes limiterEnabled=0 into shared memory (shared_state_write.exe) - the
# tray process being hung would mean it isn't reading that flag either. Instead it calls
# force_revert_default.exe, which makes its own direct IPolicyConfig call and has zero dependency
# on the tray process being alive or responsive.
#
# NOT part of the shipped app - a dev-time tool only, matching windows/tools/loopback_capture.cpp
# and shared_state_dump.cpp's throwaway-diagnostic status.
#
# Usage: run this in a SEPARATE terminal BEFORE (or immediately after) starting a live limiter
# test - e.g. before running HeadphoneSafetyTray.exe with the limiter enabled and playing test
# audio. It waits -TimeoutSeconds (default 20) and then unconditionally runs
# force_revert_default.exe, no matter what the tray is doing at that point. Safe to run
# repeatedly; if VB-Cable isn't the current default (nothing to revert), force_revert_default.exe
# says so and exits cleanly.
#
#   .\windows\tools\watchdog.ps1                    # 20 second default
#   .\windows\tools\watchdog.ps1 -TimeoutSeconds 45
[CmdletBinding()]
param(
    [int]$TimeoutSeconds = 20,
    [string]$ExePath = (Join-Path $PSScriptRoot "..\build\tools\force_revert_default.exe")
)

$resolvedExe = $null
try { $resolvedExe = (Resolve-Path $ExePath -ErrorAction Stop).Path } catch {}
if (-not $resolvedExe) {
    Write-Warning "Could not find force_revert_default.exe at $ExePath - build windows/ first (.\windows\build.ps1)."
    exit 1
}

Write-Host "watchdog: will force-revert the default output away from VB-Cable in $TimeoutSeconds seconds unless stopped first (Ctrl+C to cancel)."
Start-Sleep -Seconds $TimeoutSeconds
Write-Host "watchdog: timeout reached, force-reverting now."
& $resolvedExe
