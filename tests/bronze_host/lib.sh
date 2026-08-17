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
        [[ -x "$candidate" ]] && { echo "$candidate"; return 0; }
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
        [[ -x "$candidate" ]] && { echo "$candidate"; return 0; }
    done
    return 1
}

# The import library a module links against. bronze's own search
# (src/cli/link.cpp) looks beside the CLI and one or two directories up, which
# finds it in a single-config tree and misses it under a multi-config generator
# ? there the library is one level deeper, in shared/<Config>/. So the path is
# passed explicitly rather than left to chance.
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
