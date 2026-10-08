#!/usr/bin/env bash
# Test runner for bro integration tests.
# Usage: ./tests/run_tests.sh [filter]
#   filter: optional substring to match test file paths (e.g. "dom" or "click")
#
# Discovers all tests/*/test_*.js files, runs each via bro-headless, and reports
# pass/fail with a summary. The JS tests are the default run; BRO_TEST_JS=0
# leaves them out (a filter naming a .js file or test_* re-enables them, since
# a filter that can match nothing else is asking for them). Also runs the
# bronze_host checks, enumerated from
# tests/bronze_host/run_checks.sh --list — their subject is a bronze-COMPILED
# app rather than a script a JS realm could evaluate, so each is a shell check
# rather than a test_*.js. Each runs the same bro-headless and reports its own
# PASS/FAIL lines; exit 77 (the automake convention) counts as SKIP, which a
# tree without the bronze CLI (the bronze-cli target not built) uses so that "this tree
# cannot build the subject" never reads as "the subject is broken".
# BRO_TEST_BRONZE=0 leaves them out entirely, BRO_TEST_BRONZE_SKIP=<names>
# leaves out the ones it names; their per-check timeout is
# BRO_TEST_SH_TIMEOUT (default 120 s — a first run compiles its module).
#
# A JS test that cannot test its subject in this environment says so with
# skipTest(reason) (weights absent, feature compiled out) or, when
# getContext('webgl2' / 'scene') returned null, missingGpuContext(kind) — which
# SKIPs only on a run with no GPU (BRO_TEST_ALLOW_RASTER) and FAILS otherwise.
# bro-headless then exits 77 and the test is reported SKIP, never PASS.
#
# Also runs bro's C++ test binaries (the `native` group: the Vulkan core,
# scene, scene-pass and WebGL tests, tile, media clock/backend, video encode),
# found beside bro-headless — whichever this build produced (BRO_BUILD_TESTS
# and the feature flags decide which exist). Each runs under the same
# validation settings. BRO_TEST_NATIVE=0 leaves them out.
#
# And the windowed self-tests (the `windowed` group): each tests/windowed/<app>/
# is run by the windowed `bro` in a real window on SDL's offscreen video driver,
# checking what its swapchain presented (see run_one_test). They need `bro`
# beside bro-headless; BRO_TEST_WINDOWED=0 leaves them out.
#
# Runs on the GPU path (headless's default) so the
# tests exercise the same renderer, WebGL, and layer compositing that ship —
# CPU-only raster is a different code path and would leave those untested. A
# test whose engine silently fell back to raster is reported as a FAIL for that
# reason (see run_one_test); BRO_TEST_ALLOW_RASTER=1 permits it.
#
# Vulkan validation: where the Khronos validation layer is installed, every
# test runs with BRO_VK_VALIDATION=1 and a validation error FAILS the test
# (bro-headless exits nonzero and reports "Vulkan validation: N error(s)").
# Message ids listed in tests/vk_validation_known.txt — errors owned by work
# still in progress — are reported as KNOWN warnings instead; delete a line
# there once its errors are fixed. BRO_TEST_VK_VALIDATION=0 turns validation
# off (faster runs); =1 insists on it (the run stops if the layer is missing).
#
# Parallelism: tests run serially (1 job at a time) by default to prevent OOM
# on memory-constrained systems where multiple headless instances with Skia/Vulkan/Audio
# saturate RAM. Control with BRO_TEST_JOBS (default: 1). Pass BRO_TEST_PARALLEL=1
# or BRO_TEST_JOBS=auto (min(#groups, nproc/4)) to opt in to parallel group execution.
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

# mapfile, associative arrays and `wait -n` need bash 4+. macOS ships 3.2 as
# /bin/bash, where this script would otherwise die halfway with a syntax-shaped
# error; say what is wrong instead.
if (( BASH_VERSINFO[0] < 4 )); then
    echo "ERROR: tests/run_tests.sh needs bash 4+ (this is ${BASH_VERSION})."
    echo "       macOS: brew install bash, then run it with /opt/homebrew/bin/bash"
    exit 1
fi

# Bump the file descriptor limit so long test runs don't exhaust the Vulkan
# driver's DRM client fds (default 1024).
ulimit -n 65536 2>/dev/null || ulimit -n 4096 2>/dev/null || true

# On Linux without a display, use dummy SDL video driver for native headless Vulkan offscreen rendering.
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" && "$(uname -s)" == "Linux" ]]; then
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
fi

to_win_path() {
    local p="$1"
    if [[ "${BRO:-}" == *.exe ]]; then
        if [[ "$p" =~ ^/mnt/([a-zA-Z])/(.*) ]]; then
            echo "${BASH_REMATCH[1]}:/${BASH_REMATCH[2]}"
        elif [[ "$p" =~ ^/([a-zA-Z])/(.*) ]]; then
            echo "${BASH_REMATCH[1]}:/${BASH_REMATCH[2]}"
        else
            echo "$p"
        fi
    else
        echo "$p"
    fi
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
export BRO_PROJECT_ROOT="$PROJECT_DIR"

# Find the headless binary. BRO_HEADLESS overrides auto-detection so the suite
# can run against an arbitrary build dir or a packaged dist binary.
BRO=""
BRO_SELECTED_BY=""
if [[ -n "${BRO_HEADLESS:-}" ]]; then
    BRO="$BRO_HEADLESS"
    BRO_SELECTED_BY="BRO_HEADLESS env override"
    if [[ ! -x "$BRO" ]]; then
        echo "ERROR: BRO_HEADLESS=$BRO is not an executable"
        exit 1
    fi
    # Absolute, so it survives the check scripts that cd before running it.
    BRO="$(cd "$(dirname "$BRO")" && pwd)/$(basename "$BRO")"
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

TEST_APP="$(to_win_path "$SCRIPT_DIR/test_app")"

# --- Staleness gate ---------------------------------------------------------
# Refuse to run if any source file is newer than the selected binary. Fails
# closed: an unbuildable answer is better than a false green. Scans bro's own
# src/ plus any sibling library working trees the build compiles from (the
# engine and terminal groups of scripts/repos.txt; see docs/ecosystem.md) — an
# edit in ../htmlayout is just as capable of invalidating a binary as an edit
# in src/.
SOURCE_ROOTS=("$PROJECT_DIR/src")
SIBLING_LIBS=()
while read -r SIB GROUP _REST; do
    [[ "$GROUP" == "engine" || "$GROUP" == "terminal" ]] && SIBLING_LIBS+=("$SIB")
done < <(grep -v '^#' "$PROJECT_DIR/scripts/repos.txt" 2>/dev/null)
for SIB in "${SIBLING_LIBS[@]}"; do
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
# available (git-bash and Linux have it), else Homebrew coreutils' `gtimeout`,
# else a Perl stand-in with the same contract (below) — stock macOS has
# neither binary, and a suite whose hung test wedges it forever is the thing
# the cap exists to prevent, so the cap is never silently dropped. Under
# BRONZE_GC_STRESS every allocation collects, and a test that takes a minute
# normally can take several; the default cap grows to 1200 s there so only a
# real hang trips it.
if [[ -n "${BRONZE_GC_STRESS:-}" && "${BRONZE_GC_STRESS}" != "0" ]]; then
    TEST_TIMEOUT="${BRO_TEST_TIMEOUT:-1200}"
else
    TEST_TIMEOUT="${BRO_TEST_TIMEOUT:-300}"
fi

# `perl_timeout -k KILL SECS cmd args...`: GNU timeout's contract, for a box
# with no coreutils. The command runs in its own process group (as under GNU
# timeout) so a TERM reaches whatever it spawned — a bronze_host check is a
# bash script driving bro-headless — and KILL follows KILL seconds later if
# the group is still there. Exit 124 on timeout, 137 if KILL was needed,
# otherwise the command's own status (128+N for a signal death). A TERM/INT
# sent to the runner is passed on to the group rather than orphaning it.
perl_timeout() {
    perl -e '
        use strict; use POSIX ();
        my $kill = 0;
        if ($ARGV[0] eq "-k") { shift; $kill = shift; }
        my $secs = shift;
        my $pid = fork();
        die "fork: $!" unless defined $pid;
        if ($pid == 0) { setpgrp(0, 0); exec { $ARGV[0] } @ARGV; exit 127; }
        my ($timed, $killed) = (0, 0);
        $SIG{TERM} = $SIG{INT} = sub { kill "TERM", -$pid; };
        $SIG{ALRM} = sub {
            if (!$timed) {
                $timed = 1; kill "TERM", -$pid; alarm $kill if $kill;
            } else {
                $killed = 1; kill "KILL", -$pid;
            }
        };
        alarm $secs;
        1 while waitpid($pid, 0) == -1 && $!{EINTR};
        my $st = $?;
        alarm 0;
        exit($killed ? 137 : 124) if $timed;
        exit(128 + ($st & 127)) if $st & 127;
        exit($st >> 8);
    ' -- "$@"
}

TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN="gtimeout"
elif command -v perl >/dev/null 2>&1; then
    TIMEOUT_BIN="perl_timeout"
else
    echo "WARNING: no timeout, gtimeout or perl on PATH — per-test timeouts are OFF;"
    echo "         a hung test will hang the suite. (macOS: brew install coreutils)"
fi

# --- Vulkan device / driver probe -------------------------------------------
# Inspect Vulkan driver, physical device, and API version.
# Captured first: `| grep -q` under pipefail would fail on SIGPIPE.
VK_PROBE_OUTPUT=$("$BRO" "$TEST_APP" -e "0" 2>&1)
VK_DEVICE=$(echo "$VK_PROBE_OUTPUT" | sed -n 's/.*Vulkan: Initialized successfully on device: \([^(]*\).*/\1/p' | head -1 | sed 's/[[:space:]]*$//')
VK_DRIVER=$(echo "$VK_PROBE_OUTPUT" | sed -n 's/.*(Driver \([^)]*\)).*/\1/p' | head -1 | sed 's/[[:space:]]*$//')

if [[ -n "$VK_DEVICE" ]]; then
    echo "  Vulkan: $VK_DEVICE (Driver $VK_DRIVER)"
else
    # Do not use silent fallbacks that hide missing features or bugs.
    if [[ "${BRO_TEST_ALLOW_RASTER:-0}" != "1" ]]; then
        echo "ERROR: Vulkan initialization failed or no Vulkan device found."
        echo "       Vulkan is required for tests. To allow CPU raster fallback, set BRO_TEST_ALLOW_RASTER=1."
        echo "$VK_PROBE_OUTPUT" | grep -iE "Vulkan|SDL|failed|error|Fatal" | head -10 | sed 's/^/       /'
        exit 1
    else
        echo "  Vulkan: initialization failed — continuing with CPU raster fallback (BRO_TEST_ALLOW_RASTER=1)"
    fi
fi

# --- Vulkan validation --------------------------------------------------------
# Homebrew's validation layer manifest names its dylib bare, for dyld to find
# on its search path, which does not include Homebrew's lib/ — the layer is
# then listed but fails to load. DYLD_* cannot fix that here: SIP strips it
# from anything run through /usr/bin/perl (the timeout fallback). Instead hand
# the loader a copy of the manifest whose library_path is absolute.
if [[ "$(uname -s)" == "Darwin" && -z "${VK_LAYER_PATH:-}" ]] && command -v brew >/dev/null 2>&1; then
    BREW_PREFIX="$(brew --prefix)"
    VK_VAL_MANIFEST="$BREW_PREFIX/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json"
    VK_VAL_DYLIB="$BREW_PREFIX/lib/libVkLayer_khronos_validation.dylib"
    if [[ -f "$VK_VAL_MANIFEST" && -f "$VK_VAL_DYLIB" ]]; then
        VK_LAYER_DIR="$(mktemp -d -t bro-vk-layers)"
        sed "s|\"library_path\": *\"[^\"]*\"|\"library_path\": \"$VK_VAL_DYLIB\"|" \
            "$VK_VAL_MANIFEST" > "$VK_LAYER_DIR/VkLayer_khronos_validation.json"
        export VK_LAYER_PATH="$VK_LAYER_DIR"
        trap 'rm -rf "$VK_LAYER_DIR"' EXIT
    fi
fi
BRO_TEST_VK_VALIDATION="${BRO_TEST_VK_VALIDATION:-auto}"
if [[ "$BRO_TEST_VK_VALIDATION" != "0" && -n "$VK_DEVICE" ]]; then
    VK_VAL_PROBE=$(BRO_VK_VALIDATION=1 "$BRO" "$TEST_APP" -e "0" 2>&1)
    if [[ "$VK_VAL_PROBE" == *"Validation layers requested, but not available"* ||
          "$VK_VAL_PROBE" != *"Vulkan: Initialized successfully"* ]]; then
        if [[ "$BRO_TEST_VK_VALIDATION" == "1" ]]; then
            echo "ERROR: BRO_TEST_VK_VALIDATION=1 but the Vulkan validation layer is not usable:"
            echo "$VK_VAL_PROBE" | grep -iE "validation|layer|vkCreateInstance" | head -5 | sed 's/^/       /'
            exit 1
        fi
        if [[ "$VK_VAL_PROBE" == *"Validation layers requested, but not available"* ]]; then
            echo "  Vulkan validation: layer not installed — validation off"
        else
            echo "  Vulkan validation: layer installed but would not load — validation off"
            echo "$VK_VAL_PROBE" | grep -iE "layer|vkCreateInstance" | head -3 | cut -c1-200 | sed 's/^/       /'
        fi
    else
        export BRO_VK_VALIDATION=1
        export BRO_VK_VALIDATION_KNOWN="$(to_win_path "$SCRIPT_DIR/vk_validation_known.txt")"
        echo "  Vulkan validation: on (errors fail tests; known ids: tests/vk_validation_known.txt)"
    fi
fi

# The JS tests run by default. BRO_TEST_JS=0 skips them (a bronze_host-only
# run); a filter that names a .js file or a test_* stem overrides that, because
# such a filter can match nothing else.
BRO_TEST_JS="${BRO_TEST_JS:-1}"
if [[ -n "${FILTER:-}" && ( "$FILTER" == *".js" || "$FILTER" == *"test_"* ) ]]; then
    BRO_TEST_JS=1
fi

# bro's C++ test binaries, built beside bro-headless (src/*/CMakeLists.txt,
# BRO_BUILD_TESTS). Listed by name so a renamed or dropped one is noticed here.
NATIVE_TESTS=(bro_vulkan_test bro_vulkan_scene_test bro_vulkan_scene_passes_test
              bro_vulkan_webgl_test bro_tile_test bro_mediaclocktest
              bro_mediabackendtest bro_videoencodetest bro_terminal_test
              bro_a11y_test bro_vulkan_dmabuf_test bro_nested_compositor_test
              bro_drm_screen_test bro_shell_surfaces_test bro_xwayland_test
              bro_daily_driver_test bro_window_stacking_test bro_source_preflight_test)
BRO_DIR="$(dirname "$BRO")"
EXE_SUFFIX=""
[[ "$BRO" == *.exe ]] && EXE_SUFFIX=".exe"

# Collect test files: the JS tests, plus the bronze_host checks — enumerated
# from that folder's manifest (run_checks.sh --list) as `bronze:<name>`
# entries, in manifest order, so each name is one test here. They skip
# themselves with exit 77 in a tree that cannot build their subject.
mapfile -t TEST_FILES < <(
    if [[ "$BRO_TEST_JS" == "1" ]]; then
        find "$SCRIPT_DIR" -path "*/test_app" -prune -o -name "test_*.js" -print | sort
    fi
    if [[ "${BRO_TEST_BRONZE:-1}" != "0" && -f "$SCRIPT_DIR/bronze_host/run_checks.sh" ]]; then
        bash "$SCRIPT_DIR/bronze_host/run_checks.sh" --list | sed 's/^/bronze:/'
    fi
    if [[ "${BRO_TEST_NATIVE:-1}" != "0" ]]; then
        for N in "${NATIVE_TESTS[@]}"; do
            [[ -f "$BRO_DIR/$N$EXE_SUFFIX" ]] && echo "native:$N"
        done
    fi
    if [[ "${BRO_TEST_WINDOWED:-1}" != "0" && -f "$BRO_DIR/bro$EXE_SUFFIX" ]]; then
        for D in "$SCRIPT_DIR"/windowed/*/; do
            [[ -f "$D/index.html" ]] && echo "windowed:$(basename "$D")"
        done
    fi)

if [[ ${#TEST_FILES[@]} -eq 0 ]]; then
    echo "No test files found."
    exit 1
fi

# Run one test. Prints the PASS/FAIL lines (identical to the historical serial
# runner) to stdout. Returns 0 on pass, 1 on fail, 2 on timeout, 3 on skip.
run_one_test() {
    local TEST_FILE="$(to_win_path "$1")" REL="$2" OUTPUT STATUS

    # A bronze_host check (a `bronze:<name>` entry from run_checks.sh --list)
    # runs itself and prints its own PASS/FAIL/SKIP lines; only its exit code
    # is mapped here. It gets a longer timeout than a JS test because a first
    # run compiles its module with the bronze CLI, and two of the probes take
    # minutes.
    if [[ "$1" == bronze:* ]]; then
        local SH_TIMEOUT="${BRO_TEST_SH_TIMEOUT:-120}"
        local CHECK="$SCRIPT_DIR/bronze_host/run_checks.sh" NAME="${1#bronze:}"
        if [[ -n "$TIMEOUT_BIN" ]]; then
            OUTPUT=$(BRO_HEADLESS="$BRO" "$TIMEOUT_BIN" -k 10 "$SH_TIMEOUT" bash "$CHECK" "$NAME" 2>&1)
        else
            OUTPUT=$(BRO_HEADLESS="$BRO" bash "$CHECK" "$NAME" 2>&1)
        fi
        STATUS=$?
        echo "$OUTPUT"
        case $STATUS in
            0)  return 0 ;;
            77) return 3 ;;
            124|137)
                echo "  FAIL  $REL  (TIMEOUT after ${SH_TIMEOUT}s)"
                return 2 ;;
            *)  return 1 ;;
        esac
    fi

    # A C++ test binary (`native:<name>`): exit 0 passes, 77 skips, anything
    # else fails. Under BRO_TEST_ALLOW_RASTER there is no Vulkan device for
    # the GPU ones to use, so those are skipped rather than failed.
    if [[ "$1" == native:* ]]; then
        local NAME="${1#native:}" BIN
        BIN="$BRO_DIR/$NAME$EXE_SUFFIX"
        if [[ "${BRO_TEST_ALLOW_RASTER:-0}" == "1" && "$NAME" == bro_vulkan_* ]]; then
            echo "  SKIP  $REL  (no GPU: BRO_TEST_ALLOW_RASTER=1)"
            return 3
        fi
        # In a scratch directory: some write their output to the cwd
        # (bro_videoencodetest's encode_test.webm), which must not be the repo.
        local NATIVE_CWD
        NATIVE_CWD=$(mktemp -d "${TMPDIR:-/tmp}/bro_native.XXXXXX")
        if [[ -n "$TIMEOUT_BIN" ]]; then
            OUTPUT=$(cd "$NATIVE_CWD" && "$TIMEOUT_BIN" -k 10 "$TEST_TIMEOUT" "$BIN" 2>&1)
        else
            OUTPUT=$(cd "$NATIVE_CWD" && "$BIN" 2>&1)
        fi
        STATUS=$?
        rm -rf "$NATIVE_CWD"
        case $STATUS in
            0)  echo "  PASS  $REL"; return 0 ;;
            77) echo "  SKIP  $REL"; return 3 ;;
            124|137)
                echo "  FAIL  $REL  (TIMEOUT after ${TEST_TIMEOUT}s)"
                echo "$OUTPUT" | tail -20 | sed 's/^/        /'
                return 2 ;;
            *)
                echo "  FAIL  $REL  (exit $STATUS)"
                echo "$OUTPUT" | tail -40 | sed 's/^/        /'
                return 1 ;;
        esac
    fi

    # A windowed self-test (`windowed:<app>`, tests/windowed/<app>/): the
    # windowed `bro` itself runs the app in a real window — SDL's offscreen
    # video driver, whose Vulkan surface (VK_EXT_headless_surface) needs no
    # display — so the frame loop, raster thread, swapchains and presenters a
    # headless run never touches are what it exercises. BRO_CAPTURE_PRESENTS=1
    # reads every presented frame back for its presentedFrame() checks. It
    # reports like a JS test: assert() fails it (exit 1), skipTest() 77.
    if [[ "$1" == windowed:* ]]; then
        local NAME="${1#windowed:}" BIN="$BRO_DIR/bro$EXE_SUFFIX"
        if [[ "${BRO_TEST_ALLOW_RASTER:-0}" == "1" ]]; then
            echo "  SKIP  $REL  (no GPU: BRO_TEST_ALLOW_RASTER=1)"
            return 3
        fi
        local APP RUN_CWD
        APP="$(to_win_path "$SCRIPT_DIR/windowed/$NAME")"
        # bro writes its log to bro.log in the cwd: a scratch one, read back.
        RUN_CWD=$(mktemp -d "${TMPDIR:-/tmp}/bro_windowed.XXXXXX")
        if [[ -n "$TIMEOUT_BIN" ]]; then
            (cd "$RUN_CWD" && SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy BRO_CAPTURE_PRESENTS=1 \
                "$TIMEOUT_BIN" -k 10 "$TEST_TIMEOUT" "$BIN" --no-splash "$APP" >/dev/null 2>&1)
        else
            (cd "$RUN_CWD" && SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy BRO_CAPTURE_PRESENTS=1 \
                "$BIN" --no-splash "$APP" >/dev/null 2>&1)
        fi
        STATUS=$?
        OUTPUT=$(cat "$RUN_CWD"/bro*.log 2>/dev/null)
        rm -rf "$RUN_CWD"
        if [[ "$OUTPUT" =~ Vulkan\ validation:\ ([0-9]+)\ error ]]; then
            echo "  FAIL  $REL  (${BASH_REMATCH[1]} Vulkan validation error(s))"
            echo "$OUTPUT" | grep -A2 "\[Vulkan .* ERROR\]" | head -24 | sed 's/^/        /'
            return 1
        fi
        case $STATUS in
            0)  echo "  PASS  $REL"; return 0 ;;
            77) echo "  SKIP  $REL  ($(echo "$OUTPUT" | sed -n 's/.*SKIP: //p' | head -1))"; return 3 ;;
            124|137)
                echo "  FAIL  $REL  (TIMEOUT after ${TEST_TIMEOUT}s)"
                echo "$OUTPUT" | tail -20 | sed 's/^/        /'
                return 2 ;;
            *)
                echo "  FAIL  $REL  (exit $STATUS)"
                echo "$OUTPUT" | grep -E "ASSERTION FAILED|ERROR|Fatal" | head -20 | sed 's/^/        /'
                echo "$OUTPUT" | tail -15 | sed 's/^/        /'
                return 1 ;;
        esac
    fi

    local TEST_APP_FOR_RUN="$TEST_APP"
    local EXTRA_ENV=()
    case "$REL" in
        compositor/*|cred/*|displays/*|portal/*|seat/*|sys/*|wl/*)
            TEST_APP_FOR_RUN="$(to_win_path "$SCRIPT_DIR/desktop_trust/trusted_app")"
            EXTRA_ENV+=( "BRO_TRUSTED_APP_DIR=$TEST_APP_FOR_RUN" )
            ;;
    esac
    # The window-frame tests run the DRM shell host's compositor headless,
    # with brocompositor's test clients (a sibling checkout's build).
    case "$REL" in
        compositor/test_window_frames*)
            EXTRA_ENV+=( "BRO_HEADLESS_COMPOSITOR=1"
                         "BC_TEST_CLIENT_DIR=${BC_TEST_CLIENT_DIR:-$SCRIPT_DIR/../../brocompositor/build-release/tests}" )
            ;;
    esac

    local EXTRA_TEST_ARGS=()
    if [[ "${BRO_TEST_ALLOW_RASTER:-0}" == "1" ]]; then
        EXTRA_TEST_ARGS+=( "--no-gpu" )
    fi

    if [[ -n "$TIMEOUT_BIN" ]]; then
        OUTPUT=$(env "${EXTRA_ENV[@]}" "$TIMEOUT_BIN" -k 10 "$TEST_TIMEOUT" "$BRO" "${EXTRA_TEST_ARGS[@]}" "$TEST_APP_FOR_RUN" "$TEST_FILE" 2>&1)
        STATUS=$?
    else
        OUTPUT=$(env "${EXTRA_ENV[@]}" "$BRO" "${EXTRA_TEST_ARGS[@]}" "$TEST_APP_FOR_RUN" "$TEST_FILE" 2>&1)
        STATUS=$?
    fi

    # The engine fails fast or throws if Vulkan initialization fails when GPU is configured.
    # Catch any Vulkan failure and fail loud and clear.
    # BRO_TEST_ALLOW_RASTER=1 opts out for a deliberate raster-only run on a box with no Vulkan at all.
    if [[ "${BRO_TEST_ALLOW_RASTER:-0}" != "1" ]] &&
       [[ "$OUTPUT" == *"continuing with CPU raster fallback"* ||
          "$OUTPUT" == *"falling back to CPU raster rendering"* ||
          "$OUTPUT" == *"Headless Vulkan init failed"* ||
          "$OUTPUT" == *"Headless Vulkan initialization failed"* ||
          "$OUTPUT" == *"Vulkan initialization failed"* ]]; then
        echo "  FAIL  $REL  (NO GPU — Vulkan init failed)"
        echo "$OUTPUT" | grep -iE "Vulkan|SDL|GPU init failed|Fatal" | head -5 | sed 's/^/        /'
        echo "        Set BRO_TEST_ALLOW_RASTER=1 to run anyway (GPU paths untested)."
        return 1
    fi

    # A rejection nothing handled is a failure, whatever the exit code says.
    # The headless driver already fails the run for one it reports (an
    # uncancelled `unhandledrejection`, a top-level await that rejected or
    # never settled); bronze's own stderr line is what a realm with no host
    # hook still prints, and it must not read as a pass either.
    if [[ $STATUS -eq 0 && "$OUTPUT" == *"Unhandled promise rejection:"* ]]; then
        echo "  FAIL  $REL  (unhandled promise rejection)"
        echo "$OUTPUT" | grep -A3 "Unhandled promise rejection:" | head -12 | sed 's/^/        /'
        return 1
    fi

    if [[ "$OUTPUT" =~ Vulkan\ validation:\ ([0-9]+)\ error ]]; then
        echo "  FAIL  $REL  (${BASH_REMATCH[1]} Vulkan validation error(s))"
        echo "$OUTPUT" | grep -A2 "\[Vulkan .* ERROR\]" | head -24 | sed 's/^/        /'
        return 1
    fi

    if [[ "$OUTPUT" == *"=== bro-headless crash"* ]]; then
        echo "  FAIL  $REL  (CRASH)"
        echo "$OUTPUT" | grep -B 25 -A 80 "=== bro-headless crash" | head -120 | sed 's/^/        /'
        return 1
    fi

    if [[ $STATUS -eq 0 ]]; then
        echo "  PASS  $REL"
        return 0
    elif [[ $STATUS -eq 77 ]]; then
        # skipTest() / missingGpuContext() on a run without the capability.
        local WHY
        WHY=$(echo "$OUTPUT" | sed -n 's/.*SKIP: //p' | head -1)
        echo "  SKIP  $REL  (${WHY:-skipped})"
        return 3
    elif [[ -n "$TIMEOUT_BIN" && ($STATUS -eq 124 || $STATUS -eq 137) ]]; then
        # 124 = timeout sent TERM, 137 = timeout escalated to KILL
        echo "  FAIL  $REL  (TIMEOUT after ${TEST_TIMEOUT}s)"
        echo "$OUTPUT" | tail -20 | sed 's/^/        /'
        return 2
    else
        echo "  FAIL  $REL"
        # Show output for diagnosis (tail captures the error and stack trace after engine startup logs)
        echo "$OUTPUT" | tail -40 | sed 's/^/        /'
        return 1
    fi
}

# --- Build the filtered test list and its per-group partition ---------------

FILTERED_FILES=()
FILTERED_RELS=()
for TEST_FILE in "${TEST_FILES[@]}"; do
    if [[ "$TEST_FILE" == bronze:* ]]; then
        REL="bronze_host/${TEST_FILE#bronze:}"
    elif [[ "$TEST_FILE" == native:* ]]; then
        REL="native/${TEST_FILE#native:}"
    elif [[ "$TEST_FILE" == windowed:* ]]; then
        REL="windowed/${TEST_FILE#windowed:}"
    else
        REL="${TEST_FILE#$SCRIPT_DIR/}"
    fi
    if [[ -n "$FILTER" && "$REL" != *"$FILTER"* && "$TEST_FILE" != *"$FILTER"* ]]; then
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

if [[ "${BRO_TEST_JOBS:-1}" == "auto" || "${BRO_TEST_PARALLEL:-0}" == "1" ]]; then
    NPROC=$(detect_nproc)
    # Each headless instance is itself multi-threaded (main loop, UI raster,
    # canvas worker, audio, JS pumps), so nproc/4 processes saturate the machine.
    JOBS=$(( NPROC / 4 ))
    [[ $JOBS -lt 2 ]] && JOBS=2
    [[ $JOBS -gt ${#GROUPS_ORDERED[@]} ]] && JOBS=${#GROUPS_ORDERED[@]}
    [[ $JOBS -lt 1 ]] && JOBS=1
elif [[ -n "${BRO_TEST_JOBS:-}" ]]; then
    JOBS="$BRO_TEST_JOBS"
    if ! [[ "$JOBS" =~ ^[0-9]+$ ]] || [[ "$JOBS" -lt 1 ]]; then
        echo "ERROR: BRO_TEST_JOBS must be a positive integer or 'auto' (got '$BRO_TEST_JOBS')"
        exit 1
    fi
else
    JOBS=1
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
SKIPPED=0
ERRORS=()

if [[ $JOBS -le 1 || ${#GROUPS_ORDERED[@]} -le 1 ]]; then
    # ---- Serial path: identical behavior to the historical runner ----------
    for i in "${!FILTERED_FILES[@]}"; do
        run_one_test "${FILTERED_FILES[$i]}" "${FILTERED_RELS[$i]}"
        RC=$?
        if [[ $RC -eq 0 ]]; then
            ((PASSED++))
        elif [[ $RC -eq 3 ]]; then
            ((SKIPPED++))
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
    cleanup() { rm -rf "$TMPDIR_TESTS" ${VK_LAYER_DIR:+"$VK_LAYER_DIR"}; }
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
        local G="$1" idx i p=0 f=0 s=0 t0 t1
        local LOG="$TMPDIR_TESTS/$G.log" ERRF="$TMPDIR_TESTS/$G.errs"
        t0=$SECONDS
        : > "$LOG"; : > "$ERRF"
        idx=(${GROUP_INDICES[$G]})
        for i in "${idx[@]}"; do
            run_one_test "${FILTERED_FILES[$i]}" "${FILTERED_RELS[$i]}" >> "$LOG"
            local rc=$?
            if [[ $rc -eq 0 ]]; then
                ((p++))
            elif [[ $rc -eq 3 ]]; then
                ((s++))
            elif [[ $rc -eq 2 ]]; then
                ((f++))
                echo "${FILTERED_RELS[$i]} (TIMEOUT)" >> "$ERRF"
            else
                ((f++))
                echo "${FILTERED_RELS[$i]}" >> "$ERRF"
            fi
        done
        t1=$SECONDS
        echo "$p $f $s" > "$TMPDIR_TESTS/$G.counts"
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
            read -r GP GF GS < "$TMPDIR_TESTS/$G.counts"
            PASSED=$(( PASSED + GP ))
            FAILED=$(( FAILED + GF ))
            SKIPPED=$(( SKIPPED + GS ))
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

TOTAL=$((PASSED + FAILED + SKIPPED))
echo ""
echo "────────────────────────────────────"
SUMMARY="  $TOTAL tests: $PASSED passed, $FAILED failed"
[[ $SKIPPED -gt 0 ]] && SUMMARY+=", $SKIPPED skipped"
echo "$SUMMARY"

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
