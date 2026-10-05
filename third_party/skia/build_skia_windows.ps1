# Build Skia for Windows x64 from source.
#
# Prerequisites:
#   - Visual Studio 2022 with "Desktop development with C++" (MSVC + Windows SDK)
#   - git and Python 3 on PATH
#   - Run from a "Developer PowerShell for VS 2022" (x64), so lib.exe is on PATH
#
# Ganesh is built for Vulkan only (bro has no GL). The Vulkan headers come from
# Skia's own third_party tree, so no Vulkan SDK is needed to build the lib.
# Compiled with MSVC against the dynamic CRT (/MD), like bro itself. Skia's
# image codecs are off (bro decodes images itself and links no codec libs on
# Windows); expat, HarfBuzz and ICU come from the source bundle, which bro
# compiles (skia_modules.cmake).
#
# Usage:
#   cd third_party\skia
#   .\build_skia_windows.ps1            # builds Release
#   .\build_skia_windows.ps1 Debug      # builds Debug
#   .\build_skia_windows.ps1 all        # builds both
#
# $env:SKIA_SRC = 'C:\path\to\checkout' builds from a full Skia checkout
# elsewhere; third_party\skia\src normally holds the trimmed source bundle,
# which is not a buildable tree, so by default the checkout goes to
# third_party\skia\skia-checkout. Either way it is pinned to $SkiaCommit, the
# chrome/m147 commit the source bundle was cut from.

param([ValidateSet('Release', 'Debug', 'all')][string]$Config = 'Release')

$ErrorActionPreference = 'Stop'
$ScriptDir = $PSScriptRoot
$SkiaSrc = if ($env:SKIA_SRC) { $env:SKIA_SRC } else { Join-Path $ScriptDir 'skia-checkout' }
$SkiaCommit = 'abbe599fb3c0ef2fa82bfadbb0ddcd321f22faf0'   # chrome/m147

function Invoke-Checked([string]$what, [scriptblock]$cmd) {
    & $cmd
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

if (-not (Get-Command lib.exe -ErrorAction SilentlyContinue)) {
    throw 'lib.exe not found: run this from a Developer PowerShell for VS 2022 (x64).'
}

# Clone the pinned commit if there is no checkout yet.
if (-not (Test-Path (Join-Path $SkiaSrc 'BUILD.gn'))) {
    Write-Host '=== Cloning Skia source ==='
    New-Item -ItemType Directory -Force $SkiaSrc | Out-Null
    Push-Location $SkiaSrc
    if (-not (Test-Path .git)) {
        Invoke-Checked 'git init' { git init -q }
        Invoke-Checked 'git remote add' { git remote add origin https://skia.googlesource.com/skia.git }
    }
    Invoke-Checked 'git fetch' { git fetch --depth 1 origin $SkiaCommit }
    Invoke-Checked 'git checkout' { git checkout -f FETCH_HEAD }
    Pop-Location
}

Push-Location $SkiaSrc
try {
    $head = (git rev-parse HEAD).Trim()
    if ($head -ne $SkiaCommit) {
        throw "$SkiaSrc is at $head, expected $SkiaCommit (chrome/m147). The library must match the headers in the source bundle."
    }

    # git-sync-deps fires ~40 parallel fetches at googlesource.com, which
    # rate-limits; retry with backoff on transient failures.
    Write-Host '=== Syncing Skia dependencies ==='
    $delay = 10
    foreach ($attempt in 1..5) {
        python tools/git-sync-deps
        if ($LASTEXITCODE -eq 0) { break }
        Write-Host "=== git-sync-deps attempt $attempt failed; sleeping ${delay}s ==="
        Start-Sleep -Seconds $delay
        $delay *= 2
    }
    Invoke-Checked 'git-sync-deps' { python tools/git-sync-deps }
    Invoke-Checked 'fetch-ninja' { python bin/fetch-ninja }
    $ninja = Join-Path $SkiaSrc 'third_party\ninja\ninja.exe'

    function Build-Config([string]$cfg) {
        $isDebug = if ($cfg -eq 'Debug') { 'true' } else { 'false' }
        $isOfficial = if ($cfg -eq 'Debug') { 'false' } else { 'true' }
        $gnArgs = @(
            "is_official_build=$isOfficial"
            "is_debug=$isDebug"
            'skia_use_gl=false'
            'skia_use_vulkan=true'
            'skia_use_direct3d=false'
            'skia_enable_ganesh=true'
            'skia_enable_pdf=false'
            'skia_enable_svg=true'
            'skia_use_expat=true'
            'skia_use_dng_sdk=false'
            'skia_use_piex=false'
            'skia_use_wuffs=false'
            'skia_use_libpng_decode=false'
            'skia_use_libpng_encode=false'
            'skia_use_libjpeg_turbo_decode=false'
            'skia_use_libjpeg_turbo_encode=false'
            'skia_use_libwebp_decode=false'
            'skia_use_libwebp_encode=false'
            'skia_use_system_expat=false'
            'skia_use_system_harfbuzz=false'
            'skia_use_system_icu=false'
            'skia_use_system_zlib=false'
            'extra_cflags_cc=["/GR"]'
        )

        # Write args.gn rather than pass --args: Windows PowerShell mangles the
        # embedded double quotes when handing a string to a native command.
        Write-Host "=== Building Skia ($cfg) ==="
        New-Item -ItemType Directory -Force "out/$cfg" | Out-Null
        Set-Content -Path "out/$cfg/args.gn" -Value $gnArgs -Encoding ascii
        Invoke-Checked 'gn gen' { & .\bin\gn.exe gen "out/$cfg" }
        Invoke-Checked 'ninja' { & $ninja -C "out/$cfg" skia pathops }

        # Skia's GN "skia" target excludes pathops; bro's SVG module needs
        # SkPathOps::Op, so merge the pathops objects into the library.
        $dest = Join-Path $ScriptDir "lib\$cfg"
        New-Item -ItemType Directory -Force $dest | Out-Null
        $objs = Get-ChildItem "out/$cfg/obj/src/pathops" -Filter *.obj | ForEach-Object { $_.FullName }
        Invoke-Checked 'lib.exe' { lib.exe /NOLOGO "/OUT:$dest\skia.lib" "out/$cfg/skia.lib" @objs }
        Write-Host "=== Installed skia.lib to $dest ==="
        Get-FileHash "$dest\skia.lib" -Algorithm SHA256 | Format-List
    }

    if ($Config -eq 'all') { Build-Config 'Release'; Build-Config 'Debug' }
    else { Build-Config $Config }
} finally {
    Pop-Location
}
