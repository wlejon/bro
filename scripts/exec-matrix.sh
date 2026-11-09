#!/usr/bin/env bash
# exec-matrix.sh — run a test suite once per way bronze can execute it, and
# report the tests whose verdict depends on the way.
#
#   scripts/exec-matrix.sh [--modes auto,1,2,aot] [--out <dir>] -- <runner> [args...]
#
# <runner> is any suite runner that prints `PASS <name>` / `FAIL <name>` /
# `SKIP <name>` lines: bro's tests/run_tests.sh, helmapps' scripts/run_tests.sh,
# broworkshop's scripts/validate.sh. Each mode runs it with the environment
# that selects the mode:
#
#   auto     the default tiered JIT
#
# The first mode listed is the reference every other mode is held to.
#
#   0        BRO_JIT_TIER=0, interpreter only
#   1        BRO_JIT_TIER=1, every function baseline-compiled up front
#   2        BRO_JIT_TIER=2, the whole program optimized before it runs
#   aot      TEST_AOT=1, the page's scripts from ahead-of-time modules
#            (runners that support it: helmapps' run_tests.sh)
#   gc       BRONZE_GC_STRESS=1, a collection at every allocation (slow)
#
# Default modes: auto,1,2,aot. A test that passes in `auto` and fails in
# another mode is a bronze bug until shown otherwise; a pinned tier also
# shifts timing (tier 0 is slow, tier 2 compiles everything first), so check
# for a race before blaming the compiler. Names that look like timing tests
# (perf, budget, latency) are marked `timing?`.
#
# Writes <out>/<mode>.log (the runner's output) and <out>/matrix.txt (every
# test's verdict per mode); prints the divergences. Exit 1 when any test's
# verdict differs from auto's, 0 otherwise.
set -u

modes="auto,1,2,aot"
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
    case "$m" in auto|0|1|2|aot|gc) ;; *) echo "exec-matrix: unknown mode $m" >&2; exit 2 ;; esac
done

for m in "${mode_list[@]}"; do
    log="$out/$m.log"
    start=$(date +%s)
    case "$m" in
        auto) env -u BRO_JIT_TIER -u TEST_AOT -u BRONZE_GC_STRESS "$@" > "$log" 2>&1 ;;
        0|1|2) env -u TEST_AOT -u BRONZE_GC_STRESS BRO_JIT_TIER="$m" "$@" > "$log" 2>&1 ;;
        aot) env -u BRO_JIT_TIER -u BRONZE_GC_STRESS TEST_AOT=1 "$@" > "$log" 2>&1 ;;
        gc)  env -u BRO_JIT_TIER -u TEST_AOT BRONZE_GC_STRESS=1 "$@" > "$log" 2>&1 ;;
    esac
    echo "mode $m: $(grep -cE '^\s*PASS ' "$log") passed, $(grep -cE '^\s*FAIL ' "$log") failed," \
         "$(grep -cE '^\s*SKIP ' "$log") skipped ($(( $(date +%s) - start )) s)"
done

# One row per test: name, then its verdict in each mode (- = not run).
python3 - "$out" "${mode_list[@]}" <<'EOF'
import re, sys
out, modes = sys.argv[1], sys.argv[2:]
# `PASS  name`, or validate.sh's `PASS  test  name` / `PASS  smoke  name`.
line = re.compile(r'^\s*(PASS|FAIL|SKIP)\s+(?:(test|smoke)\s+)?(\S+?)(?::|\s|$)')
verdicts = {}
for m in modes:
    with open(f'{out}/{m}.log', errors='replace') as f:
        for l in f:
            hit = line.match(l)
            if hit:
                name = (hit.group(2) + ' ' if hit.group(2) else '') + hit.group(3)
                verdicts.setdefault(name, {})[m] = hit.group(1)
timing = re.compile(r'perf|budget|latency|timing|stall|jank', re.I)
ref = modes[0]
rows, diverged = [], []
for name in sorted(verdicts):
    v = verdicts[name]
    cells = [v.get(m, '-') for m in modes]
    rows.append(name + '\t' + '\t'.join(cells))
    base = v.get(ref)
    odd = [m for m in modes[1:] if m in v and base and v[m] != base and 'SKIP' not in (v[m], base)]
    if odd:
        diverged.append((name, base, {m: v[m] for m in odd}))
with open(f'{out}/matrix.txt', 'w') as f:
    f.write('test\t' + '\t'.join(modes) + '\n' + '\n'.join(rows) + '\n')
print(f'{len(verdicts)} tests; matrix in {out}/matrix.txt')
if not diverged:
    print(f'no test\'s verdict depends on the mode ({ref} is the reference)')
    sys.exit(0)
print(f'{len(diverged)} tests differ from {ref}:')
for name, base, odd in diverged:
    tag = '  timing?' if timing.search(name) else ''
    print(f'  {name}: {ref} {base}, ' + ', '.join(f'{m} {r}' for m, r in odd.items()) + tag)
sys.exit(1)
EOF
