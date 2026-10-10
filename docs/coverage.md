# Test coverage

Line-level C++ coverage reports for bro and every sibling library, via [OpenCppCoverage](https://github.com/OpenCppCoverage/OpenCppCoverage). **Windows / MSVC only.**

## One-time setup

```pwsh
winget install OpenCppCoverage.OpenCppCoverage
```

Installs to `C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe`, which the scripts pick up.

The Debug build needs PDBs to be present; `cmake -B build` with the default Visual Studio generator emits them. For bromesh specifically, tests run in **Release** (Debug meshoptimizer asserts hang on a modal abort dialog); see notes in its `scripts/coverage.ps1`.

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

bro's report covers only engine code the JS suite exercises: the native C++ tests under `src/*/tests/` are not run by it.


