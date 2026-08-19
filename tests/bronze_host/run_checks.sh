#!/usr/bin/env bash
# Every bronze_host check, in one manifest. The mechanics — binary discovery,
# module build, output cut, diff — are lib.sh's bh_run_check; each check here
# is one call, under a comment saying what only it catches. README.md is the
# long-form reference.
#
# Usage:
#   run_checks.sh --list     the check names, one per line (this is what
#                            tests/run_tests.sh discovers and runs one by one)
#   run_checks.sh <name>     run one check: exit 0 pass, 1 fail, 77 skip
#   run_checks.sh            run every check, with a summary line
#
# Environment: BRO_HEADLESS, BRONZE, BRO_BRONZE_FRAMES — see lib.sh.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/lib.sh"

# The loader itself — the one check whose subject is not a compiled app but
# what bro does with a folder's module: the ABI guard, the missing entry
# point, the file that is not a module at all. Bespoke enough (it builds four
# synthetic C modules with CMake) to keep its own file.
check_loader() { bash "$SCRIPT_DIR/run_loader_test.sh"; }

# The scene graph, a fixed frame count: three.js's own arithmetic surviving
# compilation, timers before rAF, the microtask checkpoint after it. The app
# needs 5 frames and the spare ones must print nothing — an app still drawing
# after `done` has a rAF it never stopped rescheduling.
check_scenegraph() {
    bh_run_check bronze_host \
        "$PROJECT_DIR/src/bronze_host/fixtures/appdir" \
        "$PROJECT_DIR/src/bronze_host/fixtures/main_scenegraph.js" \
        "$SCRIPT_DIR/expected/main_scenegraph.expected" \
        --diff-head 40
}

# Event dispatch into a compiled app, on the HYBRID app dir: a bro.json with
# "compiled": true and an index.html whose own <script> is interpreted UI JS
# beside the compiled program. drive_events.js is the driver — the only mode
# that can produce a click. Three programs, two languages, one Engine, one
# DOM, one thread.
check_events() {
    bh_run_check bronze_host_events \
        "$SCRIPT_DIR/appdir_events" \
        "$SCRIPT_DIR/apps/events_probe.js" \
        "$SCRIPT_DIR/expected/events_probe.expected" \
        --driver "$SCRIPT_DIR/drive_events.js" --split-streams
}

# fetch() in a compiled app, under a driver.
check_fetch() {
    bh_run_check bronze_host_fetch \
        "$SCRIPT_DIR/appdir_fetch" \
        "$SCRIPT_DIR/apps/fetch_probe.js" \
        "$SCRIPT_DIR/expected/fetch_probe.expected" \
        --driver "$SCRIPT_DIR/drive_fetch.js" --split-streams
}

# The ELEMENT surface (host_element.cpp): identity, real arrays, tree edits,
# classList, style, geometry. The geometry lines are safe to pin because
# appdir_dom/index.html gives #panel an explicit content box in pixels at the
# origin — the expectation is written from the CSS, not recorded from a run.
check_dom() {
    bh_run_check bronze_host_dom \
        "$SCRIPT_DIR/appdir_dom" \
        "$SCRIPT_DIR/apps/dom_probe.js" \
        "$SCRIPT_DIR/expected/dom_probe.expected" \
        --frames 4
}

# The NODE surface (host_node.cpp): the three node kinds that are not
# elements, and the tree an app is HANDED. The element surface breaks loudly;
# this one breaks silently, by skipping nodes that are there — which is why
# appdir_node/index.html carries real markup with text between elements and a
# comment at the end, on one line so the child count follows from the markup.
check_node() {
    bh_run_check bronze_host_node \
        "$SCRIPT_DIR/appdir_node" \
        "$SCRIPT_DIR/apps/node_probe.js" \
        "$SCRIPT_DIR/expected/node_probe.expected" \
        --frames 4 --diff-head 80
}

# The FILE surface (host_file.cpp): Blob, File, FileReader, object URLs. The
# only surface where the app HOLDS the bytes; its central failure is quiet — a
# blob: URL that mints but does not resolve reports success at every step and
# then 404s. Extra frames because a FileReader read is async by spec; `done`
# prints from the fourth frame, so in-flight work shows as a missing line.
check_file() {
    bh_run_check bronze_host_file \
        "$SCRIPT_DIR/appdir_file" \
        "$SCRIPT_DIR/apps/file_probe.js" \
        "$SCRIPT_DIR/expected/file_probe.expected" \
        --diff-head 80
}

# AbortController / AbortSignal / the fetch that must obey one. Every
# assertion is NEGATIVE — a fetch that must not deliver, a listener that must
# not fire twice, a reason that must not be replaced — and each fails
# silently. One signal is a 30 ms AbortSignal.timeout that must come due on
# the frame clock.
check_abort() {
    bh_run_check bronze_host_abort \
        "$SCRIPT_DIR/appdir_abort" \
        "$SCRIPT_DIR/apps/abort_probe.js" \
        "$SCRIPT_DIR/expected/abort_probe.expected" \
        --diff-head 80
}

# MutationObserver (host_observers.cpp) and the DOM-level notice it is built
# on (Document::notifyMutation). The layer could have watched its OWN mutators
# and every assertion but one would pass; the one is `page.*` — a QuickJS page
# script sets an attribute and the compiled observer must hear it, which only
# happens because dom::Element itself fires the notice.
check_observer() {
    bh_run_check bronze_host_observer \
        "$SCRIPT_DIR/appdir_observer" \
        "$SCRIPT_DIR/apps/observer_probe.js" \
        "$SCRIPT_DIR/expected/observer_probe.expected" \
        --diff-head 80
}

# ResizeObserver: a poll of the layout box, with two quiet failures — no
# initial report (looks correct until the first layout), and reporting every
# frame (indistinguishable from working, relayouts the app forever). Six
# frames: initial report, resize-and-measure in one frame, one to prove an
# unchanged size reports nothing, the rest to prove it keeps not reporting.
check_resize() {
    bh_run_check bronze_host_resize \
        "$SCRIPT_DIR/appdir_resize" \
        "$SCRIPT_DIR/apps/resize_probe.js" \
        "$SCRIPT_DIR/expected/resize_probe.expected" \
        --diff-head 80
}

# DOMParser (host_parser.cpp): the result is a SECOND DOCUMENT, which is the
# half that goes wrong quietly. `scope.*` — a query on one document must not
# answer from the other. `adopt.*` — appending a parsed node must transfer
# ownership, in Node::appendChild itself because a compiled program never
# passes through the JS bindings. `obs.*` — observers in both documents must
# report; the host used to remember only that SOME document had been hooked.
check_parser() {
    bh_run_check bronze_host_parser \
        "$SCRIPT_DIR/appdir_parser" \
        "$SCRIPT_DIR/apps/parser_probe.js" \
        "$SCRIPT_DIR/expected/parser_probe.expected" \
        --diff-head 80
}

# The four proxy-backed live views: el.style, getComputedStyle, el.dataset,
# localStorage.
check_proxy() {
    bh_run_check bronze_host_proxy \
        "$SCRIPT_DIR/appdir_proxy" \
        "$SCRIPT_DIR/apps/proxy_probe.js" \
        "$SCRIPT_DIR/expected/proxy_probe.expected" \
        --expr "advanceTime(64);"
}

# `new Function` in compiled code, answered by the engine's QuickJS realm, and
# the value bridge that makes the result usable. Only this check catches a
# crossing that copies where it must wrap (a written property the compiled side
# never sees), an identity that does not round-trip, and the three non-ordinary
# constructors being answered as if they were `Function`.
check_interp() {
    bh_run_check bronze_host_interp         "$SCRIPT_DIR/appdir_interp"         "$SCRIPT_DIR/apps/interp_probe.js"         "$SCRIPT_DIR/expected/interp_probe.expected"         --expr "advanceTime(64);"
}

# Host classes, via Image as the worked example: a handle born on its
# constructor's prototype, methods shared per class rather than closed over
# per instance, instanceof answering.
check_class() {
    bh_run_check bronze_host_class \
        "$SCRIPT_DIR/appdir_class" \
        "$SCRIPT_DIR/apps/class_probe.js" \
        "$SCRIPT_DIR/expected/class_probe.expected" \
        --expr "advanceTime(64);"
}

# pointerLock, fullscreen, and the Gamepad API, with a virtual gamepad
# connected, pressed, and steered through headless injection.
check_input() {
    bh_run_check bronze_host_input \
        "$SCRIPT_DIR/appdir_input" \
        "$SCRIPT_DIR/apps/input_probe.js" \
        "$SCRIPT_DIR/expected/input_probe.expected" \
        --expr "gamepadConnect('Virtual Controller'); gamepadButton(0, 'south', true, 1.0); gamepadAxis(0, 0, 0.75); advanceTime(64);"
}

# VideoEncoder / GifEncoder (host_video.cpp): every pixel source and every
# refusal — and the KIND of every refusal (TypeError / RangeError / Error),
# because an app branches on e.name and a layer answering plain Error to all
# three would quietly take that away. The probe has no filesystem
# (deliberately), so the app runs in a scratch dir and the runner asserts
# afterwards that each encode exists and clears a size floor; that the files
# DECODE is pinned by tests/video/*, not duplicated here.
check_video() {
    local out rc
    out="$(mktemp -d)"
    bh_run_check bronze_host_video \
        "$SCRIPT_DIR/appdir_video" \
        "$SCRIPT_DIR/apps/video_probe.js" \
        "$SCRIPT_DIR/expected/video_probe.expected" \
        --expr "advanceTime(64)" --run-in "$out" --diff-head 80 --quiet-pass
    rc=$?
    if [[ $rc -ne 0 ]]; then rm -rf "$out"; return $rc; fi

    local f floor size
    while read -r f floor; do
        if [[ ! -f "$out/$f" ]]; then
            echo "  FAIL  bronze_host_video  ($f was never written)"
            rm -rf "$out"; return 1
        fi
        size="$(wc -c < "$out/$f" | tr -d ' ')"
        if (( size < floor )); then
            echo "  FAIL  bronze_host_video  ($f is $size bytes, expected > $floor)"
            rm -rf "$out"; return 1
        fi
    done <<'EOF'
bronze_video_probe.webm 200
bronze_video_audio.webm 200
bronze_video_canvas.webm 100
bronze_video_probe.gif 200
EOF
    if [[ -e "$out/bronze_video_odd.webm" ]]; then
        echo "  FAIL  bronze_host_video  (a refused config still created a file)"
        rm -rf "$out"; return 1
    fi
    rm -rf "$out"
    echo "  PASS  bronze_host_video"
    return 0
}

# The audio surface (host_audio.cpp): AudioContext, GainNode, OscillatorNode,
# BiquadFilterNode, AnalyserNode, AudioBuffer, AudioBufferSourceNode, and a
# procedural decodeAudioData.
check_audio() {
    bh_run_check bronze_host_audio \
        "$SCRIPT_DIR/appdir_audio" \
        "$SCRIPT_DIR/apps/audio_probe.js" \
        "$SCRIPT_DIR/expected/audio_probe.expected" \
        --diff-head 80
}

# The physics surface (host_physics.cpp): Physics bodies, transforms, the
# character controller, soft bodies, raycasts, overlaps, destruction.
check_physics() {
    bh_run_check bronze_host_physics \
        "$SCRIPT_DIR/appdir_physics" \
        "$SCRIPT_DIR/apps/physics_probe.js" \
        "$SCRIPT_DIR/expected/physics_probe.expected" \
        --diff-head 30
}

# The game-AI surface (host_ai.cpp): NavGrid, NavMesh baking and queries,
# AIAgent.
check_ai() {
    bh_run_check bronze_host_ai \
        "$SCRIPT_DIR/appdir_ai" \
        "$SCRIPT_DIR/apps/ai_probe.js" \
        "$SCRIPT_DIR/expected/ai_probe.expected" \
        --diff-head 30
}

# The networking surface (host_net.cpp): bro.net over GNS, WebSocket, the
# remote fetch/XHR transport.
check_net() {
    bh_run_check bronze_host_net \
        "$SCRIPT_DIR/appdir_net" \
        "$SCRIPT_DIR/apps/net_probe.js" \
        "$SCRIPT_DIR/expected/net_probe.expected" \
        --diff-head 30
}

# A wild three.js scene with OrbitControls, under a driver that screenshots
# into the CWD (hence the pre-clean).
check_wild() {
    bh_run_check bronze_host_wild \
        "$SCRIPT_DIR/appdir_wild" \
        "$SCRIPT_DIR/apps/wild_orbit_probe.js" \
        "$SCRIPT_DIR/expected/wild_orbit_probe.expected" \
        --driver "$SCRIPT_DIR/drive_wild.js" --two-block \
        --pre-clean "wild_before.png wild_after.png"
}

# An instanced three.js mesh under load (2,500 instances). Slow to compile the
# first time — minutes and gigabytes; lib.sh caches the module beside the app
# dir afterwards.
check_instanced() {
    bh_run_check bronze_host_instanced \
        "$SCRIPT_DIR/appdir_instanced" \
        "$SCRIPT_DIR/apps/instanced_mesh_probe.js" \
        "$SCRIPT_DIR/expected/instanced_mesh_probe.expected" \
        --driver "$SCRIPT_DIR/drive_instanced.js" --two-block \
        --pre-clean "instanced_before.png instanced_after.png"
}

# pixi.js v8: WebGL sprites + pixel readback. The slowest module of all to
# compile the first time.
check_pixi() {
    bh_run_check bronze_host_pixi \
        "$SCRIPT_DIR/appdir_pixi" \
        "$SCRIPT_DIR/apps/pixi_sprites_probe.js" \
        "$SCRIPT_DIR/expected/pixi_sprites_probe.expected" \
        --driver "$SCRIPT_DIR/drive_pixi.js" --two-block \
        --pre-clean "pixi_before.png pixi_after.png"
}

# ---------------------------------------------------------------------------
CHECKS=(loader scenegraph events fetch dom node file abort observer resize
        parser proxy class interp input video audio physics ai net wild
        instanced pixi)

case "${1:-}" in
    --list)
        printf '%s\n' "${CHECKS[@]}"
        ;;
    "")
        PASS=0; FAIL=0; SKIP=0
        for C in "${CHECKS[@]}"; do
            "check_$C"
            case $? in
                0)  ((PASS++)) ;;
                77) ((SKIP++)) ;;
                *)  ((FAIL++)) ;;
            esac
        done
        echo "  bronze_host: $PASS passed, $FAIL failed, $SKIP skipped"
        [[ $FAIL -eq 0 ]] || exit 1
        ;;
    *)
        for C in "${CHECKS[@]}"; do
            if [[ "$C" == "$1" ]]; then
                "check_$1"
                exit $?
            fi
        done
        echo "run_checks.sh: unknown check '$1' (try --list)" >&2
        exit 2
        ;;
esac
