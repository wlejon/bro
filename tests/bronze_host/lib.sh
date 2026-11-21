# Shared plumbing for the bronze_host checks. Sourced, never executed.
#
# Each check is an app folder (appdir_<name>) whose page runs its probe.js as
# a module script, compiled in process by the engine as any app's is, under
# the stock bro-headless every other test drives. The probe prints `APP `
# lines; the check diffs them against expected/<name>_probe.expected.
#
# Exit codes are the automake convention: 0 pass, 1 fail, 77 skip.

# The .exe takes Windows paths; git-bash hands out /d/... ones.
bh_to_win_path() {
    local p="$1"
    if [[ "$p" =~ ^/mnt/([a-zA-Z])/(.*) ]]; then echo "${BASH_REMATCH[1]}:/${BASH_REMATCH[2]}"
    elif [[ "$p" =~ ^/([a-zA-Z])/(.*) ]]; then echo "${BASH_REMATCH[1]}:/${BASH_REMATCH[2]}"
    else echo "$p"; fi
}

# The stock headless binary, the same one tests/run_tests.sh drives.
bh_find_bro_headless() {
    if [[ -n "${BRO_HEADLESS:-}" ]]; then
        [[ -x "$BRO_HEADLESS" ]] && { echo "$BRO_HEADLESS"; return 0; }
        return 1
    fi
    local project_dir="$1" candidate
    for candidate in \
        "$project_dir/build/Release/bro-headless.exe" \
        "$project_dir/build/Debug/bro-headless.exe" \
        "$project_dir/build-release/bro-headless" \
        "$project_dir/build/bro-headless"
    do
        # -f as well as -x: a directory carries the execute bit too.
        [[ -f "$candidate" && -x "$candidate" ]] && { echo "$candidate"; return 0; }
    done
    return 1
}

# Run one check end to end: run the app, cut the pinned lines out of what it
# printed, and diff them against the committed expectation. Prints the usual
# "  PASS/FAIL/SKIP  <name>" lines and returns 0 pass, 1 fail, 77 skip — the
# codes tests/run_tests.sh maps onto its own counters.
#
#   bh_run_check <name> <appdir> <expected> [options]
#
#   --frames <n>       frames behind the default expression (default 8). The
#                      env override BRO_BRONZE_FRAMES beats it. Spare frames
#                      past what the app needs must print nothing, which is
#                      itself a check: an app still drawing after `done` has a
#                      rAF it never stopped rescheduling.
#   --expr <js>        run `-e <js>` instead of `-e advanceTime(frames*16)`
#   --driver <file>    run under a driver script instead of -e — the only mode
#                      that can produce a click
#   --two-block        diff TWO blocks: the probe's `APP ` lines in order, then
#                      the `PAGE `/`DRV ` console lines in order (stdout's,
#                      then the engine log's). Causality across the two
#                      survives because it is carried in the payload rather
#                      than in the interleaving.
#   --run-in <dir>     cd there for the run (the video probe writes its
#                      encodes into the CWD)
#   --diff-head <n>    diff lines shown on failure (default 60)
#   --quiet-pass       return 0 without printing PASS — for a wrapper that has
#                      its own checks to run after the diff and prints its own
#
# stdout and stderr are ALWAYS captured apart, never merged with 2>&1. The
# app's console (brokit's log sink) is stdout, block-buffered into a pipe; the
# engine log is stderr, written by whichever thread logs — the net service's
# "hosting on port" line lands from its own thread mid-run. Merged into one
# pipe, a stdout buffer flush can split an `APP ` line at any byte and an
# engine line lands inside it. Each stream's own order is sound; their
# interleaving never is.
#
# CR is stripped from both streams before anything is compared: Windows text
# mode expands each newline, and the expectations are byte-for-byte.
bh_run_check() {
    local name="$1" appdir="$2" expected="$3"; shift 3
    local frames=8 expr="" driver="" twoblock=0 runin=""
    local diffhead=60 quietpass=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --frames)        frames="$2";   shift 2 ;;
            --expr)          expr="$2";     shift 2 ;;
            --driver)        driver="$2";   shift 2 ;;
            --two-block)     twoblock=1;    shift ;;
            --run-in)        runin="$2";    shift 2 ;;
            --diff-head)     diffhead="$2"; shift 2 ;;
            --quiet-pass)    quietpass=1;   shift ;;
            *) echo "bh_run_check: unknown option '$1'" >&2; return 1 ;;
        esac
    done
    frames="${BRO_BRONZE_FRAMES:-$frames}"
    [[ -z "$expr" ]] && expr="advanceTime($((frames * 16)))"

    PROJECT_DIR="${PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
    export BRO_PROJECT_ROOT="$PROJECT_DIR"

    local bin
    bin="$(bh_find_bro_headless "$PROJECT_DIR")" || {
        echo "  SKIP  $name  (bro-headless not built)"
        return 77
    }

    # Each probe app's bro.json declares the capabilities it uses (appdir_net:
    # net); the run grants exactly those, as a user who said yes would.
    local -a cmd=("$bin" --grant declared "$(bh_to_win_path "$appdir")")
    if [[ -n "$driver" ]]; then cmd+=("$(bh_to_win_path "$driver")")
    else cmd+=(-e "$expr"); fi

    local raw err_raw="" status
    local err_file="${TMPDIR:-/tmp}/${name}_err.$$"
    if [[ -n "$runin" ]]; then raw="$(cd "$runin" && "${cmd[@]}" 2>"$err_file")"
    else raw="$("${cmd[@]}" 2>"$err_file")"; fi
    status=$?
    err_raw="$(cat "$err_file" 2>/dev/null || true)"
    rm -f "$err_file"

    # `APP ` is the probe's prefix, printed by the app's console on stdout
    # (`[INFO]  APP ...`); the engine log on stderr is neither deterministic
    # nor this check's business. Console lines carry a timestamp, stripped:
    # it is the one part that differs every run.
    local clean_out clean_err app_lines js_lines actual
    clean_out="$(printf '%s\n' "$raw" | tr -d '\r')"
    clean_err="$(printf '%s\n' "$err_raw" | tr -d '\r')"
    app_lines="$(printf '%s\n' "$clean_out" | sed -n 's/^\(.*\[INFO\]  \)*\(APP .*\)$/\2/p' || true)"
    if [[ $twoblock -eq 1 ]]; then
        js_lines="$(printf '%s\n%s\n' "$clean_out" "$clean_err" \
            | sed -nE 's/^(.*\[(console|INFO)\][ ]*)*((PAGE|DRV) .*)$/\3/p' || true)"
        actual="$(printf '%s\n%s\n' "$app_lines" "$js_lines")"
    else
        actual="$app_lines"
    fi

    if [[ $status -ne 0 ]]; then
        echo "  FAIL  $name  (exit $status)"
        printf '%s\n%s\n' "$clean_out" "$clean_err" | tail -20 | sed 's/^/        /'
        return 1
    fi

    local diff_file="${TMPDIR:-/tmp}/${name}_diff.$$"
    if diff -u --strip-trailing-cr "$expected" <(printf '%s\n' "$actual") \
            > "$diff_file" 2>&1; then
        rm -f "$diff_file"
        [[ $quietpass -eq 1 ]] || echo "  PASS  $name"
        return 0
    fi
    echo "  FAIL  $name  (output differs from the pinned expectation)"
    sed 's/^/        /' "$diff_file" | head -"$diffhead"
    rm -f "$diff_file"
    return 1
}
