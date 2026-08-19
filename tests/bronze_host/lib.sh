# Shared plumbing for the bronze_host checks. Sourced, never executed.
#
# WHY THIS FILE EXISTS NOW AND DID NOT BEFORE. Each check used to pin its OWN
# executable ? bro-bronze-host-dom, bro-bronze-host-events, one per app ?
# because the app was linked in, so "which app" and "which binary" were the same
# question. Seven scripts therefore had seven copies of a discovery block that
# differed only in the name it searched for. Now that an app is a MODULE its
# folder carries, all seven run the same stock bro-headless that every other
# test in tests/ uses, and the only thing that varies is the folder. One copy.
#
# Exit codes are the automake convention the checks already used: 0 pass, 1
# fail, 77 skip. Skip is load-bearing ? "the compiler is not in this tree" and
# "the check found a real defect" must never be the same result.

# The .exe takes Windows paths; git-bash hands out /d/... ones.
bh_to_win_path() {
    local p="$1"
    if [[ "$p" =~ ^/mnt/([a-zA-Z])/(.*) ]]; then echo "${BASH_REMATCH[1]}:/${BASH_REMATCH[2]}"
    elif [[ "$p" =~ ^/([a-zA-Z])/(.*) ]]; then echo "${BASH_REMATCH[1]}:/${BASH_REMATCH[2]}"
    else echo "$p"; fi
}

# The stock headless binary ? the same one tests/run_tests.sh drives. No
# bronze-specific name and no bronze-specific build: if bro-headless exists at
# all it can load a compiled app, because the loader is in every build.
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
        # -f as well as -x, here and in the search below: a directory carries
        # the execute bit too. It is not hypothetical — build/bronze WAS a
        # directory (bronze's build tree) until that tree moved to
        # build/bronze-build, and "found it" has to mean a file.
        [[ -f "$candidate" && -x "$candidate" ]] && { echo "$candidate"; return 0; }
    done
    return 1
}

# The bronze CLI that compiles an app into the module its folder carries.
# Separate from the runtime on purpose: bro NEEDS the runtime and only WANTS
# the compiler, so a tree configured -DBRONZE_WITH_LLVM=OFF has a working
# bro-headless and no way to build an app. That tree skips.
bh_find_bronze() {
    if [[ -n "${BRONZE:-}" ]]; then
        [[ -x "$BRONZE" ]] && { echo "$BRONZE"; return 0; }
        return 1
    fi
    local project_dir="$1" candidate
    for candidate in \
        "$project_dir/build/Release/bronze.exe" \
        "$project_dir/build/Debug/bronze.exe" \
        "$project_dir/build-release/bronze" \
        "$project_dir/build/bronze"
    do
        [[ -f "$candidate" && -x "$candidate" ]] && { echo "$candidate"; return 0; }
    done
    return 1
}

# The import library a module links against. bronze's own search
# (src/cli/link.cpp) probes the per-config subdirectory a multi-config generator
# appends, so a plain `bronze build` in this tree now finds it unaided. The path
# is still passed explicitly: it pins WHICH build's runtime a check links
# against, which matters in a tree that has more than one.
bh_find_shared_rt_lib() {
    local project_dir="$1" candidate
    for candidate in \
        "$project_dir/build/shared/Release/bronze_runtime_shared.lib" \
        "$project_dir/build/shared/Debug/bronze_runtime_shared.lib" \
        "$project_dir/build/shared/bronze_runtime_shared.lib" \
        "$project_dir/build-release/shared/libbronze_runtime_shared.so" \
        "$project_dir/build-release/shared/libbronze_runtime_shared.dylib" \
        "$project_dir/build/shared/libbronze_runtime_shared.so" \
        "$project_dir/build/shared/libbronze_runtime_shared.dylib"
    do
        [[ -f "$candidate" ]] && { echo "$candidate"; return 0; }
    done
    return 1
}

# The module name bro looks for inside an app directory (src/bronze_host/
# app_module.cpp picks the platform's own extension, so a folder can carry one
# per platform side by side).
bh_module_name() {
    case "$(uname -s)" in
        Darwin) echo "app.dylib" ;;
        MINGW*|MSYS*|CYGWIN*) echo "app.dll" ;;
        *)
            if [[ "${BRO_HEADLESS:-}" == *.exe || -f "${PROJECT_DIR:-}/build/Release/bro-headless.exe" || -f "${PROJECT_DIR:-}/build/Release/bronze.exe" ]]; then
                echo "app.dll"
            else
                echo "app.so"
            fi
            ;;
    esac
}

# Build <appdir>/app.<ext> from <probe.js> if it is missing or out of date.
#
# STALENESS IS CHECKED AGAINST THE COMPILER, not just the source. A module
# carries the ABI fingerprint of the bronze that emitted it, and bro refuses one
# whose stamp is not its own ? so a rebuilt runtime invalidates every module in
# the tree even though no .js changed. Rebuilding on a newer bronze.exe is what
# keeps that from surfacing as a mysterious refusal.
#
# Echoes the module path on success. Returns 77 when there is no compiler to
# build with and no usable module already present.
bh_ensure_module() {
    local project_dir="$1" appdir="$2" probe="$3"
    local module="$appdir/$(bh_module_name)"

    local bronze rtlib
    bronze="$(bh_find_bronze "$project_dir")" || bronze=""

    if [[ -z "$bronze" ]]; then
        # No compiler. A module already sitting there is still worth running ?
        # it is what a release checkout or a CI artifact would have.
        [[ -f "$module" ]] && { echo "$module"; return 0; }
        return 77
    fi

    if [[ -f "$module" && "$module" -nt "$probe" && "$module" -nt "$bronze" ]]; then
        echo "$module"
        return 0
    fi

    rtlib="$(bh_find_shared_rt_lib "$project_dir")" || {
        [[ -f "$module" ]] && { echo "$module"; return 0; }
        return 77
    }

    export BRONZE_SHARED_RT_LIB="$(bh_to_win_path "$rtlib")"
    export WSLENV="${WSLENV:-}${WSLENV:+:}BRONZE_SHARED_RT_LIB"
    local log
    log="$("$bronze" build "$(bh_to_win_path "$probe")" \
                -o "$(bh_to_win_path "$module")" \
                --emit-shared \
                --host-globals "$(bh_to_win_path "$project_dir/src/bronze_host/web_host.globals")" 2>&1)" || {
        echo "COMPILE FAILED" >&2
        printf '%s\n' "$log" | tail -20 >&2
        return 1
    }
    echo "$module"
}

# Run one check end to end: find the stock binary, ensure the module is built,
# run the app, cut the pinned lines out of what it printed, and diff them
# against the committed expectation. Eighteen scripts used to spell this out
# one at a time, each a near-identical ~90 lines; each is now one call here.
# Prints the usual "  PASS/FAIL/SKIP  <name>" lines and returns 0 pass, 1 fail,
# 77 skip — the codes tests/run_tests.sh maps onto its own counters.
#
#   bh_run_check <name> <appdir> <probe.js> <expected> [options]
#
#   --frames <n>       frames behind the default expression (default 8). The
#                      env override BRO_BRONZE_FRAMES beats it, as it always
#                      has. Spare frames past what the app needs must print
#                      nothing, which is itself a check: an app still drawing
#                      after `done` has a rAF it never stopped rescheduling.
#   --expr <js>        run `-e <js>` instead of `-e advanceTime(frames*16)`
#   --driver <file>    run under a driver script instead of -e — the only mode
#                      that can produce a click
#   --split-streams    capture stderr separately and diff TWO blocks: the
#                      compiled app's `APP ` lines from stdout in order, then
#                      the interpreted `PAGE `/`DRV ` console lines from the
#                      engine log in order. The two are different OS streams
#                      with different buffers, so their INTERLEAVING is not
#                      something a byte-for-byte expectation may depend on —
#                      each stream's own order is. Causality across the
#                      boundary survives the split because it is carried in
#                      the payload rather than in the interleaving.
#   --two-block        the same two-block cut, from one merged 2>&1 stream
#   --pre-clean <str>  rm -f these space-separated CWD-relative files first
#                      (drivers that call screenshot() write into the CWD)
#   --run-in <dir>     cd there for the run (the video probe writes its
#                      encodes into the CWD)
#   --diff-head <n>    diff lines shown on failure (default 60)
#   --quiet-pass       return 0 without printing PASS — for a wrapper that has
#                      its own checks to run after the diff and prints its own
#
# CR is stripped from both streams before anything is compared: Windows text
# mode expands each newline, and the expectations are byte-for-byte.
bh_run_check() {
    local name="$1" appdir="$2" probe="$3" expected="$4"; shift 4
    local frames=8 expr="" driver="" split=0 twoblock=0 preclean="" runin=""
    local diffhead=60 quietpass=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --frames)        frames="$2";   shift 2 ;;
            --expr)          expr="$2";     shift 2 ;;
            --driver)        driver="$2";   shift 2 ;;
            --split-streams) split=1;       shift ;;
            --two-block)     twoblock=1;    shift ;;
            --pre-clean)     preclean="$2"; shift 2 ;;
            --run-in)        runin="$2";    shift 2 ;;
            --diff-head)     diffhead="$2"; shift 2 ;;
            --quiet-pass)    quietpass=1;   shift ;;
            *) echo "bh_run_check: unknown option '$1'" >&2; return 1 ;;
        esac
    done
    frames="${BRO_BRONZE_FRAMES:-$frames}"
    [[ -z "$expr" ]] && expr="advanceTime($((frames * 16)))"

    # Global, not local: bh_module_name reads it as a fallback.
    PROJECT_DIR="${PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

    local bin
    bin="$(bh_find_bro_headless "$PROJECT_DIR")" || {
        echo "  SKIP  $name  (bro-headless not built)"
        return 77
    }

    bh_ensure_module "$PROJECT_DIR" "$appdir" "$probe" > /dev/null
    case $? in
        0)  ;;
        77) echo "  SKIP  $name  (no bronze CLI in this tree)"
            echo "        Configure with -DBRONZE_WITH_LLVM=ON to build one."
            return 77 ;;
        *)  echo "  FAIL  $name  ($(basename "$probe") did not compile)"
            return 1 ;;
    esac

    [[ -n "$preclean" ]] && rm -f $preclean

    local -a cmd=("$bin" "$(bh_to_win_path "$appdir")")
    if [[ -n "$driver" ]]; then cmd+=("$(bh_to_win_path "$driver")")
    else cmd+=(-e "$expr"); fi

    local raw err_raw="" status
    if [[ $split -eq 1 ]]; then
        local err_file="${TMPDIR:-/tmp}/${name}_err.$$"
        if [[ -n "$runin" ]]; then raw="$(cd "$runin" && "${cmd[@]}" 2>"$err_file")"
        else raw="$("${cmd[@]}" 2>"$err_file")"; fi
        status=$?
        err_raw="$(cat "$err_file" 2>/dev/null || true)"
        rm -f "$err_file"
    else
        if [[ -n "$runin" ]]; then raw="$(cd "$runin" && "${cmd[@]}" 2>&1)"
        else raw="$("${cmd[@]}" 2>&1)"; fi
        status=$?
    fi

    # `APP ` is the app's own prefix; everything else on the stream is engine
    # log, which is neither deterministic nor this check's business. The
    # interpreted halves log as `[hh:mm:ss.mmm] [INFO] [console] <text>`, and
    # the timestamp is stripped — it is the one part that differs every run.
    local clean_out clean_err app_lines js_lines actual
    clean_out="$(printf '%s\n' "$raw" | tr -d '\r')"
    app_lines="$(printf '%s\n' "$clean_out" | grep '^APP ' || true)"
    if [[ $split -eq 1 ]]; then
        clean_err="$(printf '%s\n' "$err_raw" | tr -d '\r')"
        js_lines="$(printf '%s\n' "$clean_err" \
            | sed -n 's/^.*\[console\] \(\(PAGE\|DRV\) .*\)$/\1/p' || true)"
        actual="$(printf '%s\n%s\n' "$app_lines" "$js_lines")"
    elif [[ $twoblock -eq 1 ]]; then
        js_lines="$(printf '%s\n' "$clean_out" \
            | sed -n 's/^.*\[console\] \(\(PAGE\|DRV\) .*\)$/\1/p' || true)"
        actual="$(printf '%s\n%s\n' "$app_lines" "$js_lines")"
    else
        actual="$app_lines"
    fi

    if [[ $status -ne 0 ]]; then
        echo "  FAIL  $name  (exit $status)"
        local diag="$clean_out"
        [[ $split -eq 1 ]] && diag="$clean_err"
        printf '%s\n' "$diag" | tail -20 | sed 's/^/        /'
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
