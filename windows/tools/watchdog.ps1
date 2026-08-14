# watchdog.ps1
#
# Hard-won lesson #6 (docs/windows-port.md, from the macOS build): "an independent, out-of-process
# safety net during development." While iterating on and testing the Real-Time Limiter live, it is
# valuable to have a SEPARATE process (not part of the app under test) that force-disables the
# limiter after a timeout regardless of what the tray/APO's own logic is doing - this catches
# cases where the app's own recovery logic is broken, without leaving a developer stuck with a
# stuck-on limiter for an extended period during testing.
#
# NOT part of the shipped app - a dev-time tool only, matching windows/tools/loopback_capture.cpp
# and shared_state_dump.cpp's throwaway-diagnostic status.
#
# Usage: run this in a SEPARATE terminal BEFORE (or immediately after) starting a live limiter
# test - e.g. before running HeadphoneSafetyTray.exe --enable-limiter and playing test audio. It
# waits -TimeoutSeconds (default 20) and then unconditionally writes limiterEnabled=0 into the
# shared-memory mapping via shared_state_write.exe, no matter what the tray/APO are doing at that
# point. Safe to run repeatedly; if the mapping does not exist yet (the tray hasn't started), it
# reports that clearly and exits with a non-zero code rather than silently doing nothing.
#
#   .\windows\tools\watchdog.ps1                    # 20 second default
#   .\windows\tools\watchdog.ps1 -TimeoutSeconds 45
[CmdletBinding()]
param(
    [int]$TimeoutSeconds = 20,
    [string]$ExePath = (Join-Path $PSScriptRoot "..\build\tools\shared_state_write.exe")
)

$resolvedExe = $null
try { $resolvedExe = (Resolve-Path $ExePath -ErrorAction Stop).Path } catch {}
if (-not $resolvedExe) {
    Write-Warning "Could not find shared_state_write.exe at $ExePath - build windows/ first (.\windows\build.ps1)."
    exit 1
}

Write-Host "watchdog: will force limiterEnabled=0 in $TimeoutSeconds seconds unless stopped first (Ctrl+C to cancel)."
Start-Sleep -Seconds $TimeoutSeconds
Write-Host "watchdog: timeout reached, force-disabling the limiter now."
& $resolvedExe 0
