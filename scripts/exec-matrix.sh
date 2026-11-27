#!/usr/bin/env bash
# exec-matrix.sh — run a test suite once per way bronze can execute it, and
# report the tests whose verdict depends on the way.
#
#   scripts/exec-matrix.sh [--modes auto,aot,gc] [--out <dir>] -- <runner> [args...]
#
# <runner> is any suite runner that prints `PASS <name>` / `FAIL <name>` /
# `SKIP <name>` lines: bro's tests/run_tests.sh, helmapps' scripts/run_tests.sh,
# broworkshop's scripts/validate.sh, or ctest (its `Test #N: name ... Passed`
# lines; e.g. helm's UI tests: -- ctest --test-dir ../helm/build-release).
# Each mode runs it with the environment that selects the mode:
#
#   auto     the default Auto multi-tier JIT
#
# The first mode listed is the reference every other mode is held to.
#
#   aot      TEST_AOT=1, the page's scripts from ahead-of-time modules
#            (runners that support it: helmapps' run_tests.sh)
#   gc       BRONZE_GC_STRESS=1, a collection at every allocation (slow)
#
# Default modes: auto,aot. A test that passes in `auto` and fails in
# another mode is a bronze bug until shown otherwise; GC stress shifts timing,
# so check for a race before blaming the compiler. Names that look like timing
# tests (perf, budget, latency) are marked `timing?`.
#
# To turn a divergence into a small repro, scripts/reduce-js.js shrinks a JS
# file while a run of it in the failing mode still shows the symptom.
#
# Writes <out>/<mode>.log (the runner's output) and <out>/matrix.txt (every
# test's verdict per mode); prints the divergences. Exit 1 when any test's
# verdict differs from auto's, 0 otherwise.
set -u

modes="auto,aot"
out=""
while [ $# -gt 0 ]; do
    case "$1" in
        --modes) modes="$2"; shift 2 ;;
        --out)   out="$2"; shift 2 ;;
        --)      shift; break ;;
        -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 0 ;;
        *) echo "exec-matrix: unknown option $1 (runner goes after --)" >&2; exit 2 ;;
    esac
done
if [ $# -eq 0 ]; then
    echo "exec-matrix: no runner given (scripts/exec-matrix.sh -- <runner> [args])" >&2
    exit 2
fi
[ -n "$out" ] || out=$(mktemp -d "${TMPDIR:-/tmp}/exec-matrix.XXXXXX")
mkdir -p "$out"

IFS=',' read -r -a mode_list <<< "$modes"
for m in "${mode_list[@]}"; do
    case "$m" in auto|aot|gc) ;; *) echo "exec-matrix: unknown mode $m" >&2; exit 2 ;; esac
done

for m in "${mode_list[@]}"; do
    log="$out/$m.log"
    start=$(date +%s)
    case "$m" in
        auto) env -u TEST_AOT -u BRONZE_GC_STRESS "$@" > "$log" 2>&1 ;;
        aot) env -u BRONZE_GC_STRESS TEST_AOT=1 "$@" > "$log" 2>&1 ;;
        gc)  env -u TEST_AOT BRONZE_GC_STRESS=1 "$@" > "$log" 2>&1 ;;
    esac
    echo "mode $m: $(grep -cE '^\s*PASS |Test +#[0-9]+: .* Passed' "$log") passed," \
         "$(grep -cE '^\s*FAIL |Test +#[0-9]+: .*\*\*\*(Failed|Timeout|Exception|Not Run)' "$log") failed," \
         "$(grep -cE '^\s*SKIP |Test +#[0-9]+: .*\*\*\*Skipped' "$log") skipped ($(( $(date +%s) - start )) s)"
done

# One row per test: name, then its verdict in each mode (- = not run).
script_dir="$(cd "$(dirname "$0")" && pwd)"
bronze_bin="${BRONZE_BIN:-$script_dir/../../bronze/build/src/cli/bronze}"
"$bronze_bin" run "$script_dir/exec-matrix-report.js" -- "$out" "${mode_list[@]}"
