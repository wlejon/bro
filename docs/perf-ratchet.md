# The end-to-end perf ratchet: what the user feels

`bench/ratchet.sh` runs a handful of end-to-end probes against the built
`bro-headless` (and, when it is built, tats's own headless binary), takes the
median of several repetitions per number, and compares each median with the
checked-in golden in `bench/goldens.<platform>.json`. A number fails only when
it is worse than its golden by more than its margin; there are no aspiration
targets. The script prints one table and an overall PASS/FAIL, and exits 1 on
FAIL. Code-generation ratios are brass's own ratchet
(`brass_benchmarks --check-ratchet`).

## Running it

```bash
bench/ratchet.sh                      # check: 5 reps per probe, exit 1 on a regression
bench/ratchet.sh --reps 7             # more repetitions
bench/ratchet.sh --only cpu,gc        # a subset: cpu, gc, compile, tats
bench/ratchet.sh --update             # rewrite the goldens from this run's medians
bench/ratchet.sh --work <dir>         # keep the probe logs somewhere specific
```

It uses `build/Release/bro-headless.exe` on Windows and
`build-release/bro-headless` elsewhere (`--bin` or `BRO_HEADLESS` to
override). Probe logs, a `summary.txt`, and the throwaway code caches go
under `--work` (default: a fresh directory under `$TMPDIR`). A full run with
tats takes about five minutes. It needs bash 4+ (macOS: `brew install bash`).

## What it measures

Every number is milliseconds, lower is better.

| Key | Probe | What it is |
|---|---|---|
| `cpu_nbody_ms`, `cpu_toplevel_ms`, `cpu_strings_ms`, `cpu_total_ms` | `bench/probes/cpu_work.js` on a blank page | object math in hot functions, a hot top-level loop only OSR can compile, string/Map work; timed in-script with `Date.now()` |
| `gc_<N>_avg_ms`, `gc_<N>_worst_ms` | `bench/probes/gc_pause.js -- <N>`, N = 20k, 500k, 1.5M | with N objects live, 200 batches of 20k short-lived allocations; the mean batch and the worst batch (the longest pause a frame would feel) |
| `page_compile_cold_ms`, `page_compile_warm_ms` | `tools/scene-editor` from `../broworkshop` (`--app`) | bro's own `compiled …/index.html in N ms` line: cold with an empty code cache of its own (a miss, compiled from source and stored), warm with that same cache (a hit) |
| `boot_cold_ms`, `boot_warm_ms` | same launches | wall time from launching the process to its exit after a one-line driver |
| `tats_boot_ms`, `tats_world_entry_ms`, `tats_mission_launch_ms`, `tats_lab_battle_ms` | `../tats/tools/startupprobe.js` via tats's `tats-headless` | the probe's own stage times: binary boot to `window.tats`, survey WORLD entry, mission launch, lab battle |

The probes never touch the user's code cache. When `../tats` or its build is
missing, or the scene-editor app is, those rows print `[SKIP]` and do not
count. The tats numbers come from tats's own binary, which embeds `bro_engine`
as of its last build: rebuild tats to see a bro change there.

## The pass rule and the margins

A key passes when `median <= golden * (1 + margin)`. The margin is 10% unless
`margin_for()` in `bench/ratchet.sh` widens it for a key whose median moves
more than that between quiet runs:

| Keys | Margin |
|---|---|
| `cpu_nbody_ms`, `cpu_toplevel_ms`, `cpu_total_ms`, `gc_*_avg_ms` | 10% |
| `cpu_strings_ms` | 20% |
| `gc_*_worst_ms` | 25% |
| `page_compile_cold_ms`, `page_compile_warm_ms`, `tats_*` | 15% |
| `boot_cold_ms`, `boot_warm_ms` | 25% |

## Machine load

The script samples the whole machine's CPU use for a few seconds before the
probes and again after them (`typeperf` on Windows, `/proc/stat` on Linux)
and prints it. Above 20% it prints a WARNING that the timings are
untrustworthy, and `--update` refuses to record goldens (`--force`
overrides). Windows machines often sit near 100% from work bash cannot see,
so read the `[LOAD]` lines before reading a failure as a regression.

## Updating the goldens

Re-baseline after a change that is meant to move a number, or on a new
machine, from a quiet machine:

1. `typeperf "\Processor(_Total)\% Processor Time" -sc 5` (Windows): wait
   until it reads low.
2. Run `bench/ratchet.sh` two or three times and check the medians agree.
3. `bench/ratchet.sh --update` and review the diff of
   `bench/goldens.<platform>.json` like any other change.

`--update` keeps goldens for probe groups the run skipped. Goldens are per
platform; a platform with no file reports every key as `[NEW]` and passes.
