# bronze_host integration checks

Each check is an app page, `appdir_<name>/`, whose `index.html` runs its
`probe.js` as a module script beside whatever markup and classic scripts the
check needs. `run_checks.sh` runs the page under the stock `bro-headless`
(granted what its `bro.json` declares) and diffs the probe's output against
`expected/`. `tests/run_tests.sh` runs them with the rest of the suite.

```bash
tests/bronze_host/run_checks.sh           # run all, with a summary
tests/bronze_host/run_checks.sh dom       # run one (exit 0 pass, 1 fail, 77 skip)
tests/bronze_host/run_checks.sh --list    # the names
```

- `BRO_TEST_BRONZE=0` leaves them out of `run_tests.sh`;
  `BRO_TEST_BRONZE_SKIP=<names>` (space or comma separated) drops some.
- A check skips (77) when there is no `bro-headless`.
- `natives` is not a page: it compares the native registry
  (`--print-native-manifest`) with the manifest bro's own JavaScript was
  compiled against.

## Reading a failure

The probe's `APP ` lines are cut out of stdout and diffed. Checks run with
`--two-block` (a driver script beside the page) diff all `APP ` lines, then the
driver's, so their interleaving never matters.

## Writing a check

Add `appdir_<name>/` (a `bro.json`, an `index.html` ending in
`<script type="module" src="probe.js"></script>`, the probe), a function in
`run_checks.sh` calling `bh_run_check`, and its name in `CHECKS`. Print only
`APP <name>=<value>` lines whose values are booleans or integers, and derive
the expectation from the spec and the fixture before the first run. Never pin a
float that came out of accumulation, or a device value like
`MAX_TEXTURE_SIZE`; print whether it holds the property you care about.
