# bronze_host integration checks

Compiled-app checks: each compiles a probe in `apps/` into an `appdir_*/app.<ext>`
with the bronze CLI, runs it under the stock `bro-headless`, and diffs the output
against `expected/`. `tests/run_tests.sh` runs them with the rest of the suite.

```bash
tests/bronze_host/run_checks.sh           # run all, with a summary
tests/bronze_host/run_checks.sh dom       # run one (exit 0 pass, 1 fail, 77 skip)
tests/bronze_host/run_checks.sh --list    # the names
```

- `BRO_TEST_BRONZE=0` leaves them out of `run_tests.sh`;
  `BRO_TEST_BRONZE_SKIP=<names>` (space or comma separated) drops some.
- A check skips (77) when there is no `bro-headless`, or no bronze CLI and no
  already-built module. Build the CLI with
  `cmake --build build --config Release --target bronze-cli`.
- `lib.sh` rebuilds a module when it is older than its probe or the compiler,
  so a rebuilt bronze recompiles every probe on the next run.
- `wild`, `instanced` and `pixi` are defined in `run_checks.sh` but not in the
  manifest; they run only by editing it.

## Reading a failure

Output is captured as two streams: the compiled app's `APP ` lines on stdout,
and the page's and driver's lines in the engine log on stderr. Checks run with
`--two-block` diff all `APP ` lines, then the rest, so their interleaving never
matters.

## Writing a check

Add a function to `run_checks.sh` calling `bh_run_check` and add its name to
`CHECKS`. Print only `APP <name>=<value>` lines whose values are booleans or
integers, and derive the expectation from the spec and the fixture before the
first run. Never pin a float that came out of accumulation, or a device value
like `MAX_TEXTURE_SIZE`; print whether it holds the property you care about.
