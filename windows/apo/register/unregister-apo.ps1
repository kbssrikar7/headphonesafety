# unregister-apo.ps1
#
# Reverses register-apo.ps1: removes our CLSID from the given endpoint's SFX+MFX FxProperties
# slots (restoring any pre-existing values that were backed up during registration), then
# unregisters the COM class. Deliberately leaves DisableProtectedAudioDG alone - a future
# re-install or some other unsigned APO may still depend on it, and there is no reliable way to
# know from here whether it is safe to revert.
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
$FxValueNames = @(
    "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5",  # SFX CLSID
    "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6",  # MFX CLSID
    "{d3993a3f-99c2-4402-b5ec-a92a0367664b},5",  # SFX supported processing modes
    "{d3993a3f-99c2-4402-b5ec-a92a0367664b},6"   # MFX supported processing modes
)
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
    foreach ($valueName in $FxValueNames) {
        Remove-ItemProperty -Path $fxPropsPath -Name $valueName -ErrorAction SilentlyContinue
        Write-Host "Removed '$valueName' from endpoint {$EndpointGuid}."

        if (Test-Path $backupPath) {
            $backup = Get-ItemProperty -Path $backupPath -Name $valueName -ErrorAction SilentlyContinue
            if ($null -ne $backup) {
                Set-ItemProperty -Path $fxPropsPath -Name $valueName -Value $backup.$valueName -Type MultiString
                Write-Host "  Restored previous value: $($backup.$valueName)"
                Remove-ItemProperty -Path $backupPath -Name $valueName -ErrorAction SilentlyContinue
            }
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
