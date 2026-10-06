# Windows installation script for bro runtime and desktop environment
[CmdletBinding()]
param (
    [string]$InstallDir = "$Env:ProgramFiles\bro",
    [string]$BuildDir = "..\build\Release",
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

if ($Uninstall) {
    Write-Host ">>> Uninstalling bro from $InstallDir..."
    if (Test-Path $InstallDir) {
        Remove-Item -Recurse -Force $InstallDir
    }
    Write-Host ">>> bro uninstallation complete."
    exit 0
}

Write-Host ">>> Installing bro desktop to $InstallDir..."

if (-not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
}

$SystemDir = Join-Path $InstallDir "system"
$AppsDir = Join-Path $InstallDir "apps"

if (-not (Test-Path $SystemDir)) {
    New-Item -ItemType Directory -Path $SystemDir -Force | Out-Null
}
if (-not (Test-Path $AppsDir)) {
    New-Item -ItemType Directory -Path $AppsDir -Force | Out-Null
}

# Copy binaries and runtime DLLs
Get-ChildItem -Path $BuildDir -Include @("bro.exe", "bro-headless.exe", "bro-server.exe", "*.dll") -Recurse | ForEach-Object {
    Copy-Item $_.FullName -Destination $InstallDir -Force
    Write-Host "  Installed $($_.Name)"
}

# Copy system UI panels and resources
$RepoRoot = Resolve-Path "$PSScriptRoot\..\.."
$SystemSrc = Join-Path $RepoRoot "system"
if (Test-Path $SystemSrc) {
    Copy-Item -Path "$SystemSrc\*" -Destination $SystemDir -Recurse -Force
    Write-Host "  Installed system UI resources to $SystemDir"
}

Write-Host ""
Write-Host ">>> bro desktop installation successfully finished."
Write-Host "    Trusted shell applications directory: $AppsDir"
