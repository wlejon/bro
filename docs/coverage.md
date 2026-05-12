# Test coverage

Line-level C++ coverage reports for bro and every sibling library, via [OpenCppCoverage](https://github.com/OpenCppCoverage/OpenCppCoverage). **Windows / MSVC only.**

## One-time setup

```pwsh
winget install OpenCppCoverage.OpenCppCoverage
```

Installs to `C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe`, which the scripts pick up.

The Debug build needs PDBs to be present — `cmake -B build` with the default Visual Studio generator emits them. For bromesh specifically, tests run in **Release** (Debug meshoptimizer asserts hang on a modal abort dialog); see notes in its `scripts/coverage.ps1`.

## Generating a report

Each repo has its own `scripts/coverage.ps1`. From the repo root:

```pwsh
pwsh scripts/coverage.ps1
```

HTML report lands at `build/coverage/index.html`. Open it; per-file pages live under `build/coverage/Modules/`. Green = covered, red = uncovered.

bro's script accepts a filter:

```pwsh
pwsh scripts/coverage.ps1 -Filter dom        # only tests whose path contains "dom"
pwsh scripts/coverage.ps1 -Output build/cov  # custom output dir
```

## How it works

OpenCppCoverage attaches to a running process via PDB symbols — no recompile needed. The pattern across all repos:

```
OpenCppCoverage.exe `
  --sources <repo>\src `
  --modules <exe-substring> `
  --cover_children `
  --export_type "html:build\coverage" `
  -- <parent program that drives the tests>
```

`--cover_children` means OCC instruments every child process the parent spawns. For bro that's `bro-headless.exe` running JS tests; for the sibling libraries it's the native test exes invoked by `ctest`.

| Repo | Driver under `--cover_children` |
|---|---|
| bro | `scripts/_coverage_run_all.ps1` loops over `tests/**/test_*.js` through `bro-headless.exe` |
| qjsbind, brokit, htmlayout | the single test exe directly (no ctest registration) |
| broaudio, brogameagent | `ctest --test-dir build` runs every registered test exe |
| bromesh | `bromesh_test.exe` directly, Release config |

## Bro-specific quirk

bro has no native C++ unit tests — its tests are all JS-driven through `bro-headless`. So bro's coverage measures *engine code exercised by the JS test suite*. Uncovered C++ usually means "no JS test reaches this path," not "no test file exists." Adding coverage in bro means writing new headless JS tests under `tests/`, not C++ test exes.

## Per-library coverage as of last full run

| Repo | Coverage | Tests |
|---|---|---|
| qjsbind | 97% | 1 |
| bromesh | 95.7% | 4315 assertions |
| brogameagent | 92% | 34 |
| htmlayout | 89% | 1604 |
| brokit | 85% | 2732 assertions |
| broaudio | 81% | 33 |
| bro | 43% | 130 |

Numbers move as code changes; re-run the script to refresh.
