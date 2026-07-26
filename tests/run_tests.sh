#!/usr/bin/env bash
# Test runner for bro integration tests.
# Usage: ./tests/run_tests.sh [filter]
#   filter: optional substring to match test file paths (e.g. "dom" or "click")
#
# Discovers all tests/*/test_*.js files, runs each via bro-headless, and reports
# pass/fail with a summary. Runs on the GPU path (headless's default) so the
# tests exercise the same renderer, WebGL, and layer compositing that ship —
# CPU-only raster is a different code path and would leave those untested. A
# test whose engine silently fell back to raster is reported as a FAIL for that
# reason (see run_one_test); BRO_TEST_ALLOW_RASTER=1 permits it.
#
# Parallelism: test GROUPS (per-directory) run concurrently, tests WITHIN a
# group stay serial — that preserves intra-group ordering/resource assumptions
# (net ports, fixed scratch dirs) while cutting wall time by roughly the job
# count. Control with BRO_TEST_JOBS (default: min(#groups, nproc/4), floor 2 —
# each headless instance runs ~4 threads of its own, and on a 32-thread box 8
# jobs measured faster than 16 or 24). BRO_TEST_JOBS=1 runs the original
# fully-serial path.
#
# The groups audio/gamepad/settings/style are chained into ONE serial unit:
# settings, gamepad, and style tests persist user overrides to the shared
# .bro_settings.json next to the binary, and the engine applies persisted
# audio.muted/masterVolume at startup — an audio test launched inside the
# settings test's brief muted window could measure silence. Chaining the
# writers (and the audio readers) removes both writer-writer torn saves and
# that reader race.
#
# Output is aggregated per group and printed in the same stable sorted order
# as the serial run (groups flush incrementally as they finish, in order).
# The final summary format is unchanged. BRO_TEST_TIMING=1 appends a per-group
# wall-time table after the parallel run (tuning aid; off by default).

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_APP="$SCRIPT_DIR/test_app"

# Find the headless binary. BRO_HEADLESS overrides auto-detection so the suite
# can run against an arbitrary build dir or a packaged dist binary.
#
# Auto-detection picks the NEWEST candidate, not the first one that exists. The
# old rule ("Debug if present, else Release") silently ran the suite against a
# Debug binary left over from an earlier commit, so a green run proved nothing
# about the code actually in the tree. Whichever binary is chosen, it is then
# checked against the source tree and the run is REFUSED if any source file is
# newer (see the staleness gate below) — a stale binary must never be able to
# masquerade as a passing run.
BRO=""
BRO_SELECTED_BY=""
if [[ -n "${BRO_HEADLESS:-}" ]]; then
    BRO="$BRO_HEADLESS"
    BRO_SELECTED_BY="BRO_HEADLESS env override"
    if [[ ! -x "$BRO" ]]; then
        echo "ERROR: BRO_HEADLESS=$BRO is not an executable"
        exit 1
    fi
else
    CANDIDATES=(
        "$PROJECT_DIR/build/Debug/bro-headless.exe"
        "$PROJECT_DIR/build/Release/bro-headless.exe"
        "$PROJECT_DIR/build/bro-headless"
        "$PROJECT_DIR/build-release/bro-headless"
        "$PROJECT_DIR/build-debug/bro-headless"
    )
    FOUND=()
    for C in "${CANDIDATES[@]}"; do
        [[ -f "$C" ]] && FOUND+=("$C")
    done
    if [[ ${#FOUND[@]} -eq 0 ]]; then
        echo "ERROR: bro-headless not found. Build first with: cmake --build build"
        exit 1
    fi
    for C in "${FOUND[@]}"; do
        if [[ -z "$BRO" || "$C" -nt "$BRO" ]]; then
            BRO="$C"
        fi
    done
    BRO_SELECTED_BY="auto-detected (newest of ${#FOUND[@]} candidate(s))"
fi

# --- Staleness gate ---------------------------------------------------------
# Refuse to run if any source file is newer than the selected binary. Fails
# closed: an unbuildable answer is better than a false green. Scans bro's own
# src/ plus any sibling library working trees the build compiles from (see
# docs/multi-repo-workflow.md) — an edit in ../htmlayout is just as capable of
# invalidating a binary as an edit in src/.
SOURCE_ROOTS=("$PROJECT_DIR/src")
for SIB in htmlayout brokit qjsbind bromath broaudio bromesh broflora brotensor \
           brogameagent brolm brodiffusion broimage brosoundml brovisionml; do
    [[ -d "$PROJECT_DIR/../$SIB/src" ]] && SOURCE_ROOTS+=("$PROJECT_DIR/../$SIB/src")
    [[ -d "$PROJECT_DIR/../$SIB/include" ]] && SOURCE_ROOTS+=("$PROJECT_DIR/../$SIB/include")
done

NEWER=$(find "${SOURCE_ROOTS[@]}" \
            \( -name '*.cpp' -o -name '*.cc' -o -name '*.h' -o -name '*.hpp' \
               -o -name '*.c' -o -name '*.cu' -o -name '*.cuh' \) \
            -newer "$BRO" -print 2>/dev/null | head -5)

BRO_AGE_S=""
if command -v stat >/dev/null 2>&1; then
    BIN_MTIME=$(stat -c %Y "$BRO" 2>/dev/null || stat -f %m "$BRO" 2>/dev/null || echo "")
    if [[ -n "$BIN_MTIME" ]]; then
        BRO_AGE_S=$(( $(date +%s) - BIN_MTIME ))
    fi
fi
fmt_age() {
    local s="$1"
    if [[ -z "$s" ]]; then echo "unknown age"
    elif [[ $s -lt 120 ]]; then echo "${s}s old"
    elif [[ $s -lt 7200 ]]; then echo "$(( s / 60 ))m old"
    else echo "$(( s / 3600 ))h $(( (s % 3600) / 60 ))m old"
    fi
}

echo "════════════════════════════════════════════════════════════════"
echo "  binary:   $BRO"
echo "  selected: $BRO_SELECTED_BY"
echo "  built:    $(fmt_age "$BRO_AGE_S")"
echo "════════════════════════════════════════════════════════════════"

if [[ -n "$NEWER" ]]; then
    echo ""
    echo "  ############################################################"
    echo "  #  REFUSING TO RUN — THE BINARY IS STALE                   #"
    echo "  ############################################################"
    echo ""
    echo "  These source files are NEWER than the binary above:"
    echo "$NEWER" | sed 's/^/      /'
    echo "      ... (first 5 shown)"
    echo ""
    echo "  A run against a stale binary tests code that is not in the tree."
    echo "  Rebuild, then re-run:"
    echo "      cmake --build build --config Release"
    echo ""
    echo "  To run anyway (you are asserting the binary is correct):"
    echo "      BRO_ALLOW_STALE=1 $0 ${FILTER:-}"
    echo ""
    if [[ "${BRO_ALLOW_STALE:-}" != "1" ]]; then
        exit 1
    fi
    echo "  BRO_ALLOW_STALE=1 set — proceeding against a STALE binary."
    echo ""
fi

FILTER="${1:-}"

# Per-test timeout so one hung test can't wedge the whole suite (or CI).
# Override with BRO_TEST_TIMEOUT (seconds). Uses coreutils `timeout` when
# available (git-bash and Linux have it; stock macOS may not — fall back to
# running the test bare there).
TEST_TIMEOUT="${BRO_TEST_TIMEOUT:-120}"
TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="timeout"
fi

# Collect test files
mapfile -t TEST_FILES < <(find "$SCRIPT_DIR" -path "*/test_app" -prune -o -name "test_*.js" -print | sort)

if [[ ${#TEST_FILES[@]} -eq 0 ]]; then
    echo "No test files found."
    exit 1
fi

# Run one test. Prints the PASS/FAIL lines (identical to the historical serial
# runner) to stdout. Returns 0 on pass, 1 on fail, 2 on timeout.
run_one_test() {
    local TEST_FILE="$1" REL="$2" OUTPUT STATUS

    if [[ -n "$TIMEOUT_BIN" ]]; then
        OUTPUT=$("$TIMEOUT_BIN" -k 10 "$TEST_TIMEOUT" "$BRO" "$TEST_APP" "$TEST_FILE" 2>&1)
        STATUS=$?
    else
        OUTPUT=$("$BRO" "$TEST_APP" "$TEST_FILE" 2>&1)
        STATUS=$?
    fi

    # The engine falls back to CPU raster when it can't get a GL context, which
    # for a test run is an infrastructure failure wearing a warning's clothes:
    # WebGL, layer compositing, and the whole 3D scene silently stop being
    # exercised, so a green result proves nothing about the code that ships. It
    # also used to be flaky per-process (one test losing the xvfb display while
    # its neighbours kept it), which reads as "one weird test" rather than "no
    # GPU here". Fail loudly, pass or crash. BRO_TEST_ALLOW_RASTER=1 opts out
    # for a deliberate raster-only run on a box with no GL at all.
    if [[ "${BRO_TEST_ALLOW_RASTER:-0}" != "1" ]] &&
       [[ "$OUTPUT" == *"falling back to CPU raster rendering"* ]]; then
        echo "  FAIL  $REL  (NO GPU — SDL/GL init failed, engine fell back to CPU raster)"
        echo "$OUTPUT" | grep -iE "SDL|GPU init failed" | head -5 | sed 's/^/        /'
        echo "        Set BRO_TEST_ALLOW_RASTER=1 to run anyway (GPU paths untested)."
        return 1
    fi

    if [[ $STATUS -eq 0 ]]; then
        echo "  PASS  $REL"
        return 0
    elif [[ -n "$TIMEOUT_BIN" && ($STATUS -eq 124 || $STATUS -eq 137) ]]; then
        # 124 = timeout sent TERM, 137 = timeout escalated to KILL
        echo "  FAIL  $REL  (TIMEOUT after ${TEST_TIMEOUT}s)"
        echo "$OUTPUT" | tail -20 | sed 's/^/        /'
        return 2
    else
        echo "  FAIL  $REL"
        # Show first 20 lines of output for diagnosis
        echo "$OUTPUT" | head -20 | sed 's/^/        /'
        return 1
    fi
}

# --- Build the filtered test list and its per-group partition ---------------

FILTERED_FILES=()
FILTERED_RELS=()
for TEST_FILE in "${TEST_FILES[@]}"; do
    REL="${TEST_FILE#$SCRIPT_DIR/}"
    if [[ -n "$FILTER" && "$REL" != *"$FILTER"* ]]; then
        continue
    fi
    FILTERED_FILES+=("$TEST_FILE")
    FILTERED_RELS+=("$REL")
done

# Group key = first path component (the tests/<dir>/). Sorted input keeps
# groups contiguous and in canonical order.
GROUPS_ORDERED=()   # canonical (sorted) group order, used for output
declare -A GROUP_INDICES=()   # group -> space-separated indices into FILTERED_*
for i in "${!FILTERED_RELS[@]}"; do
    REL="${FILTERED_RELS[$i]}"
    G="${REL%%/*}"
    [[ "$G" == "$REL" ]] && G="."   # top-level test file (none today)
    if [[ -z "${GROUP_INDICES[$G]:-}" ]]; then
        GROUPS_ORDERED+=("$G")
        GROUP_INDICES[$G]="$i"
    else
        GROUP_INDICES[$G]+=" $i"
    fi
done

# --- Job count ---------------------------------------------------------------

detect_nproc() {
    if command -v nproc >/dev/null 2>&1; then nproc
    elif command -v sysctl >/dev/null 2>&1 && sysctl -n hw.ncpu >/dev/null 2>&1; then sysctl -n hw.ncpu
    elif command -v getconf >/dev/null 2>&1; then getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4
    else echo 4
    fi
}

if [[ -n "${BRO_TEST_JOBS:-}" ]]; then
    JOBS="$BRO_TEST_JOBS"
    if ! [[ "$JOBS" =~ ^[0-9]+$ ]] || [[ "$JOBS" -lt 1 ]]; then
        echo "ERROR: BRO_TEST_JOBS must be a positive integer (got '$BRO_TEST_JOBS')"
        exit 1
    fi
else
    NPROC=$(detect_nproc)
    # Each headless instance is itself multi-threaded (main loop, UI raster,
    # canvas worker, audio, JS pumps), so nproc/4 processes already saturate
    # the machine; higher counts measured slower on a 32-thread box.
    JOBS=$(( NPROC / 4 ))
    [[ $JOBS -lt 2 ]] && JOBS=2
    [[ $JOBS -gt ${#GROUPS_ORDERED[@]} ]] && JOBS=${#GROUPS_ORDERED[@]}
    [[ $JOBS -lt 1 ]] && JOBS=1
fi

# The parallel scheduler needs `wait -n` (bash 4.3+). git-bash, brew bash, and
# CI Linux all have it; if this bash predates it, fall back to serial.
if [[ $JOBS -gt 1 ]]; then
    if (( BASH_VERSINFO[0] < 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] < 3) )); then
        echo "note: bash ${BASH_VERSION} lacks 'wait -n'; running serially"
        JOBS=1
    fi
fi

PASSED=0
FAILED=0
ERRORS=()

if [[ $JOBS -le 1 || ${#GROUPS_ORDERED[@]} -le 1 ]]; then
    # ---- Serial path: identical behavior to the historical runner ----------
    for i in "${!FILTERED_FILES[@]}"; do
        run_one_test "${FILTERED_FILES[$i]}" "${FILTERED_RELS[$i]}"
        RC=$?
        if [[ $RC -eq 0 ]]; then
            ((PASSED++))
        elif [[ $RC -eq 2 ]]; then
            ((FAILED++))
            ERRORS+=("${FILTERED_RELS[$i]} (TIMEOUT)")
        else
            ((FAILED++))
            ERRORS+=("${FILTERED_RELS[$i]}")
        fi
    done
else
    # ---- Parallel path: one job per group, serial within a group -----------
    TMPDIR_TESTS=$(mktemp -d "${TMPDIR:-/tmp}/bro_tests.XXXXXX")
    cleanup() { rm -rf "$TMPDIR_TESTS"; }
    trap cleanup EXIT

    # Groups that share mutable cross-process state (.bro_settings.json next
    # to the binary) run chained in one serial unit. Each still gets its own
    # log so output order stays canonical.
    CONFLICT_GROUPS=" audio gamepad settings style "

    # Build units: the conflict chain (if any of its groups are present, in
    # canonical order) plus one unit per remaining group.
    UNITS=()          # unit spec = space-separated group names
    CHAIN=""
    for G in "${GROUPS_ORDERED[@]}"; do
        if [[ "$CONFLICT_GROUPS" == *" $G "* ]]; then
            CHAIN+="${CHAIN:+ }$G"
        fi
    done
    [[ -n "$CHAIN" ]] && UNITS+=("$CHAIN")
    for G in "${GROUPS_ORDERED[@]}"; do
        if [[ "$CONFLICT_GROUPS" != *" $G "* ]]; then
            UNITS+=("$G")
        fi
    done

    # Schedule longest units first to minimize the tail.
    unit_size() {
        local total=0 g idx
        for g in $1; do
            idx=(${GROUP_INDICES[$g]})
            total=$(( total + ${#idx[@]} ))
        done
        echo "$total"
    }
    SIZED=()
    for U in "${UNITS[@]}"; do
        SIZED+=("$(printf '%05d' "$(unit_size "$U")")|$U")
    done
    mapfile -t SIZED < <(printf '%s\n' "${SIZED[@]}" | sort -r)
    UNITS=()
    for S in "${SIZED[@]}"; do
        UNITS+=("${S#*|}")
    done

    # Run all of a group's tests serially; write display output + counts, then
    # mark the group done (the .done file carries the group's wall seconds).
    run_group() {
        local G="$1" idx i p=0 f=0 t0 t1
        local LOG="$TMPDIR_TESTS/$G.log" ERRF="$TMPDIR_TESTS/$G.errs"
        t0=$SECONDS
        : > "$LOG"; : > "$ERRF"
        idx=(${GROUP_INDICES[$G]})
        for i in "${idx[@]}"; do
            run_one_test "${FILTERED_FILES[$i]}" "${FILTERED_RELS[$i]}" >> "$LOG"
            local rc=$?
            if [[ $rc -eq 0 ]]; then
                ((p++))
            elif [[ $rc -eq 2 ]]; then
                ((f++))
                echo "${FILTERED_RELS[$i]} (TIMEOUT)" >> "$ERRF"
            else
                ((f++))
                echo "${FILTERED_RELS[$i]}" >> "$ERRF"
            fi
        done
        t1=$SECONDS
        echo "$p $f" > "$TMPDIR_TESTS/$G.counts"
        echo "$(( t1 - t0 ))" > "$TMPDIR_TESTS/$G.done"
    }

    run_unit() {
        local g
        for g in $1; do
            run_group "$g"
        done
    }

    # Flush finished groups to the terminal in canonical order.
    FLUSH_IDX=0
    flush_ready() {
        while [[ $FLUSH_IDX -lt ${#GROUPS_ORDERED[@]} ]]; do
            local G="${GROUPS_ORDERED[$FLUSH_IDX]}"
            [[ -f "$TMPDIR_TESTS/$G.done" ]] || break
            cat "$TMPDIR_TESTS/$G.log"
            read -r GP GF < "$TMPDIR_TESTS/$G.counts"
            PASSED=$(( PASSED + GP ))
            FAILED=$(( FAILED + GF ))
            if [[ -s "$TMPDIR_TESTS/$G.errs" ]]; then
                while IFS= read -r E; do
                    ERRORS+=("$E")
                done < "$TMPDIR_TESTS/$G.errs"
            fi
            ((FLUSH_IDX++))
        done
    }

    RUNNING=0
    for U in "${UNITS[@]}"; do
        run_unit "$U" &
        ((RUNNING++))
        if [[ $RUNNING -ge $JOBS ]]; then
            wait -n
            ((RUNNING--))
            flush_ready
        fi
    done
    while [[ $RUNNING -gt 0 ]]; do
        wait -n
        ((RUNNING--))
        flush_ready
    done
    flush_ready

    if [[ "${BRO_TEST_TIMING:-}" == "1" ]]; then
        echo ""
        echo "  group wall times (s):"
        for G in "${GROUPS_ORDERED[@]}"; do
            [[ -f "$TMPDIR_TESTS/$G.done" ]] && printf '    %4ss  %s\n' "$(cat "$TMPDIR_TESTS/$G.done")" "$G"
        done
    fi
fi

TOTAL=$((PASSED + FAILED))
echo ""
echo "────────────────────────────────────"
echo "  $TOTAL tests: $PASSED passed, $FAILED failed"

if [[ $FAILED -gt 0 ]]; then
    echo ""
    echo "  Failed:"
    for E in "${ERRORS[@]}"; do
        echo "    - $E"
    done
    echo "────────────────────────────────────"
    exit 1
fi

echo "────────────────────────────────────"
