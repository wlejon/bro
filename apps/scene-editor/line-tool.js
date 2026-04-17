// =============================================================================
// Line tool — click-chain polyline drawing. Each click extends the current
// polyline by one segment; a click that lands on a previously-drawn point
// closes a planar sub-loop, which the app turns into a filled face
// primitive. Tool stays armed after committing a face so the user can
// continue drawing.
//
// Pure-state module: preview rendering, inference, and primitive creation
// live in app.js. We hold:
//   - the sketch plane (locked at the first click)
//   - the committed polyline vertices (>=1 while active)
//   - the live cursor position (for rubber-band preview)
//
// The first point is just the anchor; addPoint is what actually grows the
// polyline to a drawable length.
// =============================================================================

(function (global) {
    'use strict';

    // Epsilon in world units for "this click landed on a previous vertex".
    // Tuned for ground-plane / cube-face scales; the scene editor's inference
    // uses a screen-space radius for snapping, but the close-to-vertex
    // predicate needs a world-space tolerance because the app resolves the
    // click to a 3D point before asking us about closure.
    const CLOSE_EPS = 1e-4;

    function createState() {
        return {
            active:  false,
            plane:   null,     // { origin, normal, u, v }
            points:  [],       // committed polyline vertices
            preview: null,     // live cursor position (for the HUD/rubber-band)
        };
    }

    // Start a new polyline at `pos` on the given sketch plane.
    function begin(state, plane, pos) {
        state.active  = true;
        state.plane   = plane;
        state.points  = [pos.slice()];
        state.preview = pos.slice();
    }

    // Update the live cursor (rubber-band endpoint). Does not commit a
    // segment; the app calls this every mousemove.
    function update(state, pos) {
        if (!state.active) return;
        state.preview = pos.slice();
    }

    // Commit a new vertex into the polyline. Returns one of:
    //   { kind: 'segment' }       — polyline grew by one; no closure.
    //   { kind: 'closed',
    //     polygon: [[x,y,z]...],
    //     orphan:  [[x,y,z]...] } — the click closed a sub-loop. Polygon
    //     holds the closed vertices (CCW-from-+normal is up to the caller);
    //     orphan is the leftover prefix, which the app can discard (MVP)
    //     or persist separately later.
    //   { kind: 'ignored' }       — tool inactive or the click coincides
    //     with the last committed point (zero-length segment).
    function addPoint(state, pos, eps) {
        if (!state.active) return { kind: 'ignored' };
        const e = eps != null ? eps : CLOSE_EPS;
        const N = state.points.length;
        if (N > 0 && pointsNear(state.points[N - 1], pos, e)) {
            return { kind: 'ignored' };
        }
        // A closure needs at least 3 prior points to form a polygon (+ the
        // click that closes the loop). Without that, the "closure" would be
        // a degenerate 2-vertex loop.
        if (N >= 3) {
            // Exclude the most-recent point (pointsNear above rejected that
            // already) and check the remaining vertices in LIFO order, so
            // the closest closure wins if two candidates are within eps.
            for (let i = N - 2; i >= 0; i--) {
                if (pointsNear(state.points[i], pos, e)) {
                    const polygon = state.points.slice(i);
                    const orphan  = state.points.slice(0, i);
                    clear(state);
                    return { kind: 'closed', polygon, orphan };
                }
            }
        }
        state.points.push(pos.slice());
        state.preview = pos.slice();
        return { kind: 'segment' };
    }

    // True if a click at `pos` would close a valid (≥3-vertex) sub-loop.
    // The app uses this during mousemove to show a closure indicator.
    function findClosureIndex(state, pos, eps) {
        if (!state.active) return -1;
        const e = eps != null ? eps : CLOSE_EPS;
        const N = state.points.length;
        if (N < 3) return -1;
        for (let i = N - 2; i >= 0; i--) {
            if (pointsNear(state.points[i], pos, e)) {
                // Sub-loop length = N - i. Need ≥3.
                if (N - i >= 3) return i;
            }
        }
        return -1;
    }

    function commit(state) {
        // Explicit-commit path (double-click / Enter): the app calls commit()
        // to end the chain without closing. No face is produced — orphan
        // edges are discarded.
        clear(state);
    }

    function cancel(state) {
        clear(state);
    }

    function clear(state) {
        state.active  = false;
        state.plane   = null;
        state.points  = [];
        state.preview = null;
    }

    function pointsNear(a, b, eps) {
        const dx = a[0] - b[0];
        const dy = a[1] - b[1];
        const dz = a[2] - b[2];
        return (dx*dx + dy*dy + dz*dz) <= eps * eps;
    }

    global.LineTool = {
        createState, begin, update, addPoint, findClosureIndex,
        commit, cancel, CLOSE_EPS,
    };

})(typeof globalThis !== 'undefined' ? globalThis : this);
