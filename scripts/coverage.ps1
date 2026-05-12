# Test coverage report for bro (Windows / MSVC only).
#
# Runs every tests/**/test_*.js through build/Debug/bro-headless.exe under
# OpenCppCoverage and emits an HTML report at build/coverage/index.html.
#
# Requirements:
#   - OpenCppCoverage installed (winget install OpenCppCoverage.OpenCppCoverage)
#   - Debug build present at build/Debug/bro-headless.exe (PDBs needed)
#
# Usage:
#   pwsh scripts/coverage.ps1
#   pwsh scripts/coverage.ps1 -Filter dom        # only tests whose path contains "dom"
#   pwsh scripts/coverage.ps1 -Output build/cov  # custom output dir

[CmdletBinding()]
param(
    [string]$Filter = '',
    [string]$Output = 'build/coverage'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$occ = "C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe"
if (-not (Test-Path $occ)) {
    throw "OpenCppCoverage not found at $occ. Install: winget install OpenCppCoverage.OpenCppCoverage"
}

$bro = Join-Path $root 'build\Debug\bro-headless.exe'
if (-not (Test-Path $bro)) {
    throw "$bro not found. Build first: cmake --build build --config Debug"
}

$outAbs = if ([System.IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path $root $Output }
if (Test-Path $outAbs) { Remove-Item -Recurse -Force $outAbs }

$env:BRO_COVERAGE_FILTER = $Filter

& $occ `
    --sources "$root\src" `
    --modules bro-headless.exe `
    --cover_children `
    --export_type "html:$outAbs" `
    --working_dir $root `
    --quiet `
    -- powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot '_coverage_run_all.ps1')

Write-Host ""
Write-Host "Coverage report: $outAbs\index.html"
