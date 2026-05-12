# Internal helper: invoked by scripts/coverage.ps1 under OpenCppCoverage --cover_children.
# Runs every tests/**/test_*.js through build/Debug/bro-headless.exe.
# stdout/stderr from each test are discarded; we only care about the C++ paths exercised.

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$bro  = Join-Path $root 'build\Debug\bro-headless.exe'
$app  = Join-Path $root 'tests\test_app'

$filter = $env:BRO_COVERAGE_FILTER
$tests = Get-ChildItem -Path (Join-Path $root 'tests') -Recurse -Filter 'test_*.js' |
    Where-Object { $_.FullName -notmatch '\\test_app\\' } |
    Sort-Object FullName
if ($filter) {
    $tests = $tests | Where-Object { $_.FullName -like "*$filter*" }
}

$pass = 0; $fail = 0
foreach ($t in $tests) {
    $rel = $t.FullName.Substring($root.Length + 1)
    & $bro $app $t.FullName *> $null
    if ($LASTEXITCODE -eq 0) { $pass++ } else { $fail++; Write-Host "  FAIL $rel" }
}
Write-Host "`n$($pass + $fail) tests: $pass passed, $fail failed"
