# register-apo.ps1
#
# Registers the HeadphoneSafetyApo COM DLL as a system-wide APO class (regsvr32) and attaches it
# to one render endpoint's EFX (Endpoint Effect) FxProperties slot - the post-mix, closest-to-
# hardware processing location. That matches this project's requirement that the limiter see the
# final mixed signal, the same thing BlackHole captures on macOS and PipeWire's monitor source
# captures on Linux.
#
# NOTE ON THE FXPROPERTIES SCHEME: docs/windows-port.md assumed a single "GFX" value name,
# {d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2. Phase 2 research against a real, working third-party
# APO installer (PotatoAPO, github.com/Dybios/PotatoAPO) showed this was wrong - the actual
# scheme is SFX=,5 (per-stream, pre-mix), MFX=,6 (per-mode), EFX=,7 (post-mix, one instance per
# endpoint). This script uses EFX. The value TYPE is also REG_MULTI_SZ (a list of CLSIDs), not
# REG_SZ as the doc assumed.
#
# Must run elevated (writes under HKEY_LOCAL_MACHINE). Run with -ListDevices first to find the
# endpoint GUID to target.
[CmdletBinding()]
param(
    [string]$EndpointGuid,
    [string]$DllPath = (Join-Path $PSScriptRoot "..\..\build\apo\HeadphoneSafetyApo.dll"),
    [switch]$ListDevices
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

# HKLM\...\MMDevices\Audio and everything under it (including every endpoint's FxProperties key)
# is owned by TrustedInstaller, not Administrators - confirmed live: a fully elevated
# Administrator process gets "Access is denied" writing a FxProperties value via both
# Set-ItemProperty and raw reg.exe, despite the key's DACL nominally listing
# BUILTIN\Administrators: SetValue. This is documented, expected Windows behavior (see
# dechamps/APO's README and multiple Equalizer APO SourceForge support threads describing the
# identical failure), not a quirk of one machine.
#
# The fix, confirmed against Equalizer APO's own real installer source (RegistryHelper.cpp in
# its GitHub mirror): enable SeTakeOwnershipPrivilege, take ownership of the key, then grant
# Administrators a DACL entry. This is the registry equivalent of `takeown.exe` + `icacls` for
# the filesystem - standard, well-precedented administrative practice, not TrustedInstaller
# impersonation or any more exotic escalation. Scoped to exactly the one FxProperties key we
# need, not the whole MMDevices\Audio tree.
function Enable-TakeOwnershipPrivilege {
    if (-not ("HpsWin32.TokenPrivilege" -as [type])) {
        Add-Type -Namespace HpsWin32 -Name TokenPrivilege -MemberDefinition @'
[DllImport("advapi32.dll", SetLastError = true)]
public static extern bool OpenProcessToken(IntPtr ProcessHandle, uint DesiredAccess, out IntPtr TokenHandle);

[DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Auto)]
public static extern bool LookupPrivilegeValue(string lpSystemName, string lpName, out LUID lpLuid);

[DllImport("advapi32.dll", SetLastError = true)]
public static extern bool AdjustTokenPrivileges(IntPtr TokenHandle, bool DisableAllPrivileges, ref TOKEN_PRIVILEGES NewState, uint BufferLength, IntPtr PreviousState, IntPtr ReturnLength);

[StructLayout(LayoutKind.Sequential)]
public struct LUID { public uint LowPart; public int HighPart; }

[StructLayout(LayoutKind.Sequential)]
public struct LUID_AND_ATTRIBUTES { public LUID Luid; public uint Attributes; }

[StructLayout(LayoutKind.Sequential)]
public struct TOKEN_PRIVILEGES { public uint PrivilegeCount; public LUID Luid; public uint Attributes; }
'@ -ErrorAction Stop
    }

    $TOKEN_ADJUST_PRIVILEGES = 0x20
    $TOKEN_QUERY = 0x8
    $SE_PRIVILEGE_ENABLED = 0x2

    $tokenHandle = [IntPtr]::Zero
    $currentProcess = (Get-Process -Id $PID).Handle
    if (-not [HpsWin32.TokenPrivilege]::OpenProcessToken($currentProcess, $TOKEN_ADJUST_PRIVILEGES -bor $TOKEN_QUERY, [ref]$tokenHandle)) {
        throw "OpenProcessToken failed (Win32 error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
    }

    $luid = New-Object HpsWin32.TokenPrivilege+LUID
    if (-not [HpsWin32.TokenPrivilege]::LookupPrivilegeValue($null, "SeTakeOwnershipPrivilege", [ref]$luid)) {
        throw "LookupPrivilegeValue failed (Win32 error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
    }

    $tp = New-Object HpsWin32.TokenPrivilege+TOKEN_PRIVILEGES
    $tp.PrivilegeCount = 1
    $tp.Luid = $luid
    $tp.Attributes = $SE_PRIVILEGE_ENABLED

    if (-not [HpsWin32.TokenPrivilege]::AdjustTokenPrivileges($tokenHandle, $false, [ref]$tp, 0, [IntPtr]::Zero, [IntPtr]::Zero)) {
        throw "AdjustTokenPrivileges failed (Win32 error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
    }
    # AdjustTokenPrivileges can return true but still not actually grant the privilege (e.g. if
    # the token doesn't hold it at all) - GetLastWin32Error is ERROR_NOT_ALL_ASSIGNED (1300) in
    # that case even on a "successful" call. Verify by readback, not by trusting the return value
    # alone (this project's own hard-won lesson #1, applied here to a Win32 privilege call).
    $lastError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    if ($lastError -eq 1300) {
        throw "AdjustTokenPrivileges reported success but SeTakeOwnershipPrivilege was not assigned (ERROR_NOT_ALL_ASSIGNED) - is this process really running elevated?"
    }
}

function Grant-AdministratorsOwnership {
    param([string]$HklmRelativePath)  # e.g. "SOFTWARE\Microsoft\...\FxProperties", no HKLM: prefix

    Enable-TakeOwnershipPrivilege

    $adminsSid = New-Object Security.Principal.SecurityIdentifier(
        [Security.Principal.WellKnownSidType]::BuiltinAdministratorsSid, $null)

    # Take ownership first - SeTakeOwnershipPrivilege lets us open with TakeOwnership rights even
    # though the current DACL doesn't grant us that, then SetOwner. Owning the key implicitly
    # grants WRITE_DAC regardless of the DACL, which is what lets the second open below succeed.
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
        $HklmRelativePath,
        [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
        [Security.AccessControl.RegistryRights]::TakeOwnership)
    if (-not $key) { throw "Could not open HKLM:\$HklmRelativePath with TakeOwnership rights" }
    try {
        $acl = $key.GetAccessControl([Security.AccessControl.AccessControlSections]::None)
        $acl.SetOwner($adminsSid)
        $key.SetAccessControl($acl)
    } finally {
        $key.Close()
    }

    # Re-open now that Administrators owns it, and grant an explicit FullControl DACL entry so
    # ordinary (non-ownership-privileged) writes work from here on, including from future runs of
    # this script that don't re-take ownership every time.
    $key2 = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
        $HklmRelativePath,
        [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
        [Security.AccessControl.RegistryRights]::ChangePermissions)
    if (-not $key2) { throw "Took ownership but could not re-open HKLM:\$HklmRelativePath with ChangePermissions rights" }
    try {
        $acl2 = $key2.GetAccessControl()
        $rule = New-Object Security.AccessControl.RegistryAccessRule(
            $adminsSid,
            [Security.AccessControl.RegistryRights]::FullControl,
            [Security.AccessControl.InheritanceFlags]::None,
            [Security.AccessControl.PropagationFlags]::None,
            [Security.AccessControl.AccessControlType]::Allow)
        $acl2.AddAccessRule($rule)
        $key2.SetAccessControl($acl2)
    } finally {
        $key2.Close()
    }
}

function Get-RenderEndpoints {
    # Audio endpoint InstanceIds look like SWD\MMDEVAPI\{0.0.0.00000000}.{<endpoint-guid>} - the
    # first {...} is a short device-role token (e.g. "0.0.0.00000000"), NOT a GUID; the second
    # {...} is the actual endpoint GUID FxProperties keys off, and what -EndpointGuid expects.
    # (An earlier version of this regex wrongly required the first segment to also be a 36-char
    # GUID, which matched zero real devices - confirmed live against this machine's actual
    # InstanceId format rather than assumed.)
    Get-PnpDevice -Class AudioEndpoint -Status OK -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match '\{[^}]+\}\.\{([0-9a-fA-F-]{36})\}$' } |
        ForEach-Object {
            [PSCustomObject]@{
                Name = $_.FriendlyName
                Guid = $Matches[1]
            }
        }
}

if ($ListDevices) {
    Write-Host "Active audio endpoints (both render and capture show up here - pick the render/output one you want):"
    Get-RenderEndpoints | Format-Table Name, Guid -AutoSize
    exit 0
}

if (-not (Test-IsElevated)) {
    throw "register-apo.ps1 must run elevated (it writes under HKEY_LOCAL_MACHINE). Re-run from an admin PowerShell."
}

if (-not $EndpointGuid) {
    throw "Pass -EndpointGuid <guid> (no braces). Run with -ListDevices first to see available endpoints and their GUIDs."
}
$EndpointGuid = $EndpointGuid.Trim('{', '}')

$resolvedDllPath = (Resolve-Path $DllPath -ErrorAction Stop).Path
Write-Host "DLL: $resolvedDllPath"
Write-Host "Endpoint: {$EndpointGuid}"

Write-Host ""
Write-Host "[1/3] Setting DisableProtectedAudioDG (allows loading an unsigned APO)..."
$audioKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio"
if (-not (Test-Path $audioKey)) { New-Item -Path $audioKey -Force | Out-Null }
Set-ItemProperty -Path $audioKey -Name "DisableProtectedAudioDG" -Value 1 -Type DWord

Write-Host "[2/3] Registering the COM class (regsvr32)..."
$regsvr32 = Join-Path $env:WINDIR "System32\regsvr32.exe"
$proc = Start-Process -FilePath $regsvr32 -ArgumentList "/s", "`"$resolvedDllPath`"" -Wait -PassThru
if ($proc.ExitCode -ne 0) {
    throw "regsvr32 failed (exit $($proc.ExitCode))"
}
# Verify by readback, not by trusting the exit code (hard-won lesson #1: a successful call is
# not proof it took effect) - confirm the CLSID's InprocServer32 key actually exists now.
$clsidKey = "HKLM:\SOFTWARE\Classes\CLSID\$ApoClsid\InprocServer32"
if (-not (Test-Path $clsidKey)) {
    throw "regsvr32 reported success but $clsidKey was not found - registration did not actually take effect."
}
Write-Host "  Verified: $clsidKey exists"

Write-Host "[3/3] Attaching to endpoint {$EndpointGuid}'s EFX FxProperties slot..."
$fxPropsPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{$EndpointGuid}\FxProperties"
if (-not (Test-Path $fxPropsPath)) {
    throw "FxProperties key not found at $fxPropsPath - is {$EndpointGuid} a valid RENDER (output) endpoint GUID? Run with -ListDevices to check."
}

# Back up any existing EFX value for this endpoint before overwriting, so unregister-apo.ps1 can
# restore it - an endpoint may already have a vendor EFX (e.g. a Realtek effects chain).
$existing = Get-ItemProperty -Path $fxPropsPath -Name $EfxValueName -ErrorAction SilentlyContinue
if ($null -ne $existing) {
    $backupPath = Join-Path $BackupRoot $EndpointGuid
    if (-not (Test-Path $backupPath)) { New-Item -Path $backupPath -Force | Out-Null }
    Set-ItemProperty -Path $backupPath -Name $EfxValueName -Value $existing.$EfxValueName -Type MultiString
    Write-Host "  Backed up existing EFX value to $backupPath"
} else {
    Write-Host "  No existing EFX value for this endpoint - nothing to back up."
}

# This key is TrustedInstaller-owned by default - a plain elevated Administrator cannot write to
# it even though its DACL nominally allows SetValue (see Enable-TakeOwnershipPrivilege's comment
# above). Take ownership first, every run - idempotent if we already own it.
$fxPropsRelativePath = $fxPropsPath -replace '^HKLM:\\', ''
Write-Host "  Taking ownership of the FxProperties key (was TrustedInstaller-owned)..."
Grant-AdministratorsOwnership -HklmRelativePath $fxPropsRelativePath

# NOTE ON VALUE TYPE: this writes REG_MULTI_SZ per Phase 2's PotatoAPO-based research, but a live
# reg.exe query against this test machine's existing (Realtek/Conexant vendor) EFX value showed
# it stored as REG_SZ instead - an unresolved contradiction (see docs/windows-port.md). If audio
# doesn't play correctly after registering (see the Restart-Service step below), that's the first
# thing to try changing: swap -Type MultiString for -Type String and @($ApoClsid) for $ApoClsid.
Set-ItemProperty -Path $fxPropsPath -Name $EfxValueName -Value @($ApoClsid) -Type MultiString

# Verify by readback.
$readback = (Get-ItemProperty -Path $fxPropsPath -Name $EfxValueName).$EfxValueName
if ($readback -notcontains $ApoClsid) {
    throw "Wrote the FxProperties value but readback does not contain our CLSID - something is wrong."
}
Write-Host "  Verified: $fxPropsPath -> $EfxValueName = $readback"

Write-Host ""
Write-Host "Done. Restart the Windows Audio service for the audio engine to pick up the new EFX chain:"
Write-Host "  Restart-Service audiosrv -Force"
Write-Host "This restarts ALL system audio momentarily - close anything sensitive first."
