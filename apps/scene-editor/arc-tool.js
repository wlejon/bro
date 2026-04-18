// =============================================================================
// Arc tool — SketchUp-classic 2-point + bulge.
//
// Three-click state machine:
//   click 1 → start point      (begin)
//   click 2 → end point        (setEnd)        — chord locked
//   click 3 → bulge point      (commit)        — perpendicular distance from
//                                                 chord midpoint defines the
//                                                 arc height. Click 3 commits.
// Between clicks, update(pos) tracks the live cursor for rubber-band preview.
//
// Output is a polyline (segments + 1 points). The app persists it as an
// EdgePrimitive — open arcs are edge-only geometry, never auto-filled.
//
// Pure-state module. The plane, ray resolution, preview rendering, and
// EdgePrimitive creation all live in app.js.
// =============================================================================

(function (global) {
    'use strict';

    const DEFAULT_SEGMENTS = 16;

    function createState() {
        return {
            stage:    'idle',     // 'idle' | 'await-end' | 'await-bulge'
            plane:    null,
            start:    null,       // 3D
            end:      null,       // 3D
            bulge:    null,       // 3D (live cursor while await-bulge)
            preview:  null,       // 3D live cursor (rubber-band on the chord)
            segments: DEFAULT_SEGMENTS,
        };
    }

    function active(state) { return state.stage !== 'idle'; }

    function begin(state, plane, pos, segments) {
        state.stage    = 'await-end';
        state.plane    = plane;
        state.start    = pos.slice();
        state.end      = null;
        state.bulge    = null;
        state.preview  = pos.slice();
        state.segments = segments || DEFAULT_SEGMENTS;
    }

    // Track the cursor between clicks. Sets `preview` (used as the chord
    // endpoint in await-end, and as the bulge point in await-bulge).
    function update(state, pos) {
        if (state.stage === 'idle') return;
        state.preview = pos.slice();
        if (state.stage === 'await-bulge') state.bulge = pos.slice();
    }

    // Click 2: lock the chord. Returns true on success, false if the click
    // is too close to the start (degenerate chord rejected).
    function setEnd(state, pos, eps) {
        if (state.stage !== 'await-end') return false;
        if (eps == null) eps = 1e-4;
        if (_dist(state.start, pos) < eps) return false;
        state.end     = pos.slice();
        state.bulge   = pos.slice();   // initialize so preview has a value
        state.preview = pos.slice();
        state.stage   = 'await-bulge';
        return true;
    }

    // Build a polyline preview for whichever stage we're in. Returns null if
    // the current state is degenerate (no preview to show).
    //   await-end   → straight chord (start → preview)
    //   await-bulge → arc (start → end via preview as the bulge point)
    function buildPolyline(state) {
        if (state.stage === 'await-end') {
            if (!state.start || !state.preview) return null;
            if (_dist(state.start, state.preview) < 1e-9) return null;
            return [state.start.slice(), state.preview.slice()];
        }
        if (state.stage === 'await-bulge') {
            if (!state.start || !state.end || !state.bulge) return null;
            const arc = Sketch.arcPolyline(
                state.start, state.end, state.bulge,
                state.plane.normal, state.segments);
            return arc;
        }
        return null;
    }

    // Commit at click 3 (arc bulge fixed). Returns the final polyline or
    // null when degenerate. State is cleared.
    function commit(state) {
        if (state.stage !== 'await-bulge') {
            cancel(state);
            return null;
        }
        const poly = buildPolyline(state);
        clear(state);
        return poly;
    }

    function cancel(state) { clear(state); }

    function clear(state) {
        state.stage    = 'idle';
        state.plane    = null;
        state.start    = null;
        state.end      = null;
        state.bulge    = null;
        state.preview  = null;
        state.segments = DEFAULT_SEGMENTS;
    }

    function _dist(a, b) {
        const dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return Math.sqrt(dx*dx + dy*dy + dz*dz);
    }

    global.ArcTool = {
        createState, active, begin, update, setEnd,
        buildPolyline, commit, cancel,
        DEFAULT_SEGMENTS,
    };

})(typeof globalThis !== 'undefined' ? globalThis : this);
