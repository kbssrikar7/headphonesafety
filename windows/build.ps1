# Builds windows/ (the APO DLL + tray app) using VS Build Tools' bundled CMake + Ninja.
#
# Why this script exists: this project's dev toolchain (cl.exe, link.exe, cmake.exe, ninja.exe)
# only lands on PATH inside a "Developer" shell, and that environment does not persist across
# separate terminal invocations - every build must re-enter it. This script does that via
# Enter-VsDevShell (the modern, documented replacement for parsing vcvarsall.bat's output) so
# later sessions can just run `.\windows\build.ps1` instead of re-deriving this by hand.
#
# Usage:
#   .\windows\build.ps1                     # configure (if needed) + build, Debug
#   .\windows\build.ps1 -BuildConfig Release
#   .\windows\build.ps1 -Clean              # wipe windows/build and reconfigure from scratch
param(
    [string]$BuildConfig = "Debug",
    [switch]$Clean
)
# Note: intentionally NOT named $Config - Enter-VsDevShell (imported below) sets its own $Config-
# shaped state in this scope, which silently clobbered a same-named script parameter here and
# leaked the literal string "$Config" into CMAKE_BUILD_TYPE, corrupting the generated Ninja file
# (rule names containing an unescaped "$" broke ninja's parser). Confirmed live, not theoretical -
# keep this variable named something that can't collide with DevShell internals.

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$srcDir = Join-Path $repoRoot "windows"
$buildDir = Join-Path $srcDir "build"

if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found - is Visual Studio 2022 Build Tools installed? See Phase 0 in the Windows port plan."
}

$vsInstallPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsInstallPath) {
    throw "No VS install found with the VC.Tools.x86.x64 component. Re-run the Phase 0 winget install."
}

$devShellDll = Join-Path $vsInstallPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Import-Module $devShellDll
# -ErrorAction SilentlyContinue: Enter-VsDevShell internally shells out to a bare "vswhere.exe"
# (relying on PATH, unlike our own full-path lookup above) and that non-terminating error is
# purely cosmetic - the dev shell environment is still set up correctly regardless.
Enter-VsDevShell -VsInstallPath $vsInstallPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" -ErrorAction SilentlyContinue

# cl.exe / link.exe / cmake.exe / ninja.exe are on PATH for the rest of this script now.
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake.exe not on PATH after Enter-VsDevShell - is the VC.CMake.Project component installed?"
}

if (-not (Test-Path $buildDir)) {
    cmake -S $srcDir -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$BuildConfig"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit $LASTEXITCODE)" }
}

cmake --build $buildDir --config $BuildConfig
if ($LASTEXITCODE -ne 0) { throw "cmake build failed (exit $LASTEXITCODE)" }

Write-Host "`nBuild OK -> $buildDir" -ForegroundColor Green

