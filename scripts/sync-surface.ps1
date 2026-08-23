<#
.SYNOPSIS
    Generates and synchronizes WebIDL bindings, docs, and TypeScript types from brosurface into bro.

.DESCRIPTION
    Invokes the brosurface code generator toolchain (..\brosurface) to emit:
      - QuickJS C++ bindings to src/js/
      - Bronze host C++ bindings to src/bronze_host/
      - Availability stubs to src/js/feature_stubs.cpp
      - JSDoc documentation to docs/
      - TypeScript definition files to types/index.d.ts and docs/bro.d.ts

.EXAMPLE
    pwsh scripts/sync-surface.ps1
    pwsh scripts/sync-surface.ps1 -DryRun
#>
[CmdletBinding()]
param(
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$BroRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ProjectsRoot = (Resolve-Path (Join-Path $BroRoot '..')).Path
$SurfaceRoot = Join-Path $ProjectsRoot 'brosurface'

if (-not (Test-Path $SurfaceRoot)) {
    Write-Error "brosurface repository not found at: $SurfaceRoot"
    exit 1
}

$SyncScript = Join-Path $SurfaceRoot 'tools\sync_to_bro.mjs'
if (-not (Test-Path $SyncScript)) {
    Write-Error "brosurface sync script not found at: $SyncScript"
    exit 1
}

Write-Host "== Generating and Synchronizing brosurface -> bro ==" -ForegroundColor Cyan

$nodeArgs = @($SyncScript)
if ($DryRun) {
    $nodeArgs += '--dry-run'
}

& node @nodeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "brosurface sync failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

Write-Host "✅ Surface synchronization completed successfully." -ForegroundColor Green
