// =============================================================================
// Offset tool — SketchUp-style face-boundary offset.
//
// Two-stage state machine:
//   stage 'idle'           — cursor hovers; app highlights the candidate face.
//   stage 'await-distance' — face captured; distance derived from cursor or
//                            VCB. Click again (or VCB Enter) commits.
//
// The captured face is the input to commit(), which produces a new
// triangulated face polygon coplanar with the source. A separate primitive
// is created for the offset face — the source mesh is left intact (subdividing
// the source's interior to embed the offset ring is a TODO).
//
// Math: extract the boundary loop in the face's plane basis (2D), call
// Sketch.offsetPolygon2D, lift back to 3D. Negative distance = inset,
// positive = expand. Sign chosen by the caller.
// =============================================================================

(function (global) {
    'use strict';

    function createState() {
        return {
            stage:        'idle',
            primitive:    null,        // source primitive
            groupIdx:     -1,          // face group index in source
            loopWorld:    null,        // boundary loop in WORLD space [3D]
            plane:        null,        // { origin, normal, u, v } in WORLD
            clickPos:     null,        // 3D world point of the initial click
            distance:     0,           // signed offset
            previewLoop:  null,        // last offset loop (3D world)
        };
    }

    function active(state) { return state.stage !== 'idle'; }

    // Begin offset on a face group. `loopWorld` is the ordered boundary
    // (length >=3) in world space; `plane` is the world-space face plane
    // basis (origin = first loop vert, normal = face normal, u/v in plane).
    // `clickPos` is where the user clicked on the face (used to project the
    // mouse-driven distance onto the offset axis).
    function begin(state, primitive, groupIdx, loopWorld, plane, clickPos) {
        state.stage      = 'await-distance';
        state.primitive  = primitive;
        state.groupIdx   = groupIdx;
        state.loopWorld  = loopWorld.map(p => p.slice());
        state.plane      = plane;
        state.clickPos   = clickPos.slice();
        state.distance   = 0;
        state.previewLoop = null;
    }

    // Set the signed offset distance directly (e.g. from VCB).
    function setDistance(state, d) {
        if (state.stage !== 'await-distance') return;
        state.distance = d;
    }

    // Update from a 3D cursor point on the face plane. The signed distance is
    // computed by projecting the cursor along the click point's outward
    // direction relative to the loop centroid — this matches the user's
    // intuition: "drag away from the face → expand; drag in → inset".
    function updateFromCursor(state, cursorWorld) {
        if (state.stage !== 'await-distance') return;
        const c = _centroid3D(state.loopWorld);
        const ax = state.clickPos[0] - c[0];
        const ay = state.clickPos[1] - c[1];
        const az = state.clickPos[2] - c[2];
        const al = Math.sqrt(ax*ax + ay*ay + az*az);
        if (al < 1e-9) {
            // Click was at the centroid — fall back to plane-u as the axis.
            const u = state.plane.u;
            const dx = cursorWorld[0] - state.clickPos[0];
            const dy = cursorWorld[1] - state.clickPos[1];
            const dz = cursorWorld[2] - state.clickPos[2];
            state.distance = dx*u[0] + dy*u[1] + dz*u[2];
            return;
        }
        const dirx = ax / al, diry = ay / al, dirz = az / al;
        const dx = cursorWorld[0] - state.clickPos[0];
        const dy = cursorWorld[1] - state.clickPos[1];
        const dz = cursorWorld[2] - state.clickPos[2];
        // Signed projection: + when cursor moves OUTWARD from centroid,
        // - when it moves toward it.
        state.distance = dx*dirx + dy*diry + dz*dirz;
    }

    // Build the offset polygon (3D world-space loop), or null when the
    // current distance collapses or skips the polygon. Updates previewLoop.
    function buildOffsetLoop(state) {
        if (state.stage !== 'await-distance') return null;
        if (Math.abs(state.distance) < 1e-9) {
            state.previewLoop = state.loopWorld.map(p => p.slice());
            return state.previewLoop;
        }
        const { plane, loopWorld } = state;
        // Project loop to the plane's 2D basis at plane.origin.
        const loop2D = loopWorld.map(p =>
            Sketch.project3Dto2D(p, plane.origin, plane.u, plane.v));
        const offset2D = Sketch.offsetPolygon2D(loop2D, state.distance);
        if (!offset2D) {
            state.previewLoop = null;
            return null;
        }
        const loop3D = offset2D.map(p =>
            Sketch.unproject2Dto3D(p, plane.origin, plane.u, plane.v));
        state.previewLoop = loop3D;
        return loop3D;
    }

    // Commit. Returns the offset 3D loop and clears state. Caller is
    // responsible for triangulating + creating the new primitive.
    function commit(state) {
        const out = buildOffsetLoop(state);
        clear(state);
        return out;
    }

    function cancel(state) { clear(state); }

    function clear(state) {
        state.stage       = 'idle';
        state.primitive   = null;
        state.groupIdx    = -1;
        state.loopWorld   = null;
        state.plane       = null;
        state.clickPos    = null;
        state.distance    = 0;
        state.previewLoop = null;
    }

    function _centroid3D(loop) {
        let cx = 0, cy = 0, cz = 0;
        for (const p of loop) { cx += p[0]; cy += p[1]; cz += p[2]; }
        const n = loop.length;
        return [cx / n, cy / n, cz / n];
    }

    global.OffsetTool = {
        createState, active, begin, setDistance, updateFromCursor,
        buildOffsetLoop, commit, cancel,
    };

})(typeof globalThis !== 'undefined' ? globalThis : this);
