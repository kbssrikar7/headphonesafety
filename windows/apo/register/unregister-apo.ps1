# unregister-apo.ps1
#
# Reverses register-apo.ps1: removes our CLSID from the given endpoint's EFX FxProperties slot
# (restoring any pre-existing value that was backed up during registration), then unregisters
# the COM class. Deliberately leaves DisableProtectedAudioDG alone - a future re-install or some
# other unsigned APO may still depend on it, and there is no reliable way to know from here
# whether it is safe to revert.
#
# Must run elevated (writes under HKEY_LOCAL_MACHINE).
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EndpointGuid,
    [string]$DllPath = (Join-Path $PSScriptRoot "..\..\build\apo\HeadphoneSafetyApo.dll")
)

$ErrorActionPreference = "Stop"

$ApoClsid = "{AAF92DEA-FFE0-4E91-94A4-39385AD5ECFD}"
$EfxValueName = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7"
$BackupRoot = "HKCU:\Software\HeadphoneSafety\FxPropertiesBackup"

function Test-IsElevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
}

if (-not (Test-IsElevated)) {
    throw "unregister-apo.ps1 must run elevated. Re-run from an admin PowerShell."
}

$EndpointGuid = $EndpointGuid.Trim('{', '}')
$fxPropsPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{$EndpointGuid}\FxProperties"
$backupPath = Join-Path $BackupRoot $EndpointGuid

if (Test-Path $fxPropsPath) {
    Remove-ItemProperty -Path $fxPropsPath -Name $EfxValueName -ErrorAction SilentlyContinue
    Write-Host "Removed the EFX value from endpoint {$EndpointGuid}."

    if (Test-Path $backupPath) {
        $backup = Get-ItemProperty -Path $backupPath -Name $EfxValueName -ErrorAction SilentlyContinue
        if ($null -ne $backup) {
            Set-ItemProperty -Path $fxPropsPath -Name $EfxValueName -Value $backup.$EfxValueName -Type MultiString
            Write-Host "Restored the endpoint's previous EFX value: $($backup.$EfxValueName)"
            Remove-Item -Path $backupPath -Force
        }
    }
} else {
    Write-Host "No FxProperties key found for {$EndpointGuid} - nothing to remove there."
}

$resolvedDllPath = $null
try { $resolvedDllPath = (Resolve-Path $DllPath -ErrorAction Stop).Path } catch {}
if ($resolvedDllPath) {
    $regsvr32 = Join-Path $env:WINDIR "System32\regsvr32.exe"
    $proc = Start-Process -FilePath $regsvr32 -ArgumentList "/s", "/u", "`"$resolvedDllPath`"" -Wait -PassThru
    if ($proc.ExitCode -ne 0) {
        Write-Warning "regsvr32 /u exited with code $($proc.ExitCode))"
    }
} else {
    Write-Warning "Could not resolve $DllPath - skipping regsvr32 /u. The CLSID's registry entries may be left behind, but the FxProperties reference (what actually matters for audio behavior) has already been removed above."
}

Write-Host ""
Write-Host "Done. Restart-Service audiosrv -Force (or reboot) to make the audio engine drop this APO."
