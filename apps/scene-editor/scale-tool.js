// =============================================================================
// Scale tool — scale a whole primitive around its pivot (bbox centroid).
//
// Driven by the engine gizmo's scale handles: each frame the gizmo fires a
// per-axis multiplicative factor (current / last), which we accumulate
// component-wise into the running scale and re-apply to a snapshot of the
// start positions. Commit bakes via updateGeometry so BVH / face groups /
// inference / edges rebuild; cancel rolls back via revertMesh.
//
// Normals are kept as the canonical start normals — they are correct for
// uniform scales and visually close enough for the mild non-uniform scales
// users typically pull. (Proper fix is inverse-transpose, not worth the
// complexity for a tool-preview path.)
//
// Pure-state module: camera / input / gizmo plumbing lives in app.js.
// =============================================================================

(function (global) {
    'use strict';

    function createState() {
        return {
            active: false,
            primitive: null,
            pivot: [0, 0, 0],
            startPositions: null,
            workingPositions: null,
            accumScale: [1, 1, 1],
        };
    }

    function begin(state, primitive, pivot) {
        state.active = true;
        state.primitive = primitive;
        state.pivot[0] = pivot[0]; state.pivot[1] = pivot[1]; state.pivot[2] = pivot[2];
        state.startPositions   = new Float32Array(primitive.positions);
        state.workingPositions = new Float32Array(primitive.positions.length);
        state.accumScale[0] = state.accumScale[1] = state.accumScale[2] = 1;
        applyDelta(state, 1, 1, 1);
    }

    // Multiply the per-frame per-axis factor into the accumulated scale and
    // rebuild the working positions relative to pivot.
    function applyDelta(state, sx, sy, sz) {
        if (!state.active) return;
        state.accumScale[0] *= sx;
        state.accumScale[1] *= sy;
        state.accumScale[2] *= sz;
        const ax = state.accumScale[0], ay = state.accumScale[1], az = state.accumScale[2];
        const sp = state.startPositions, wp = state.workingPositions;
        const px = state.pivot[0], py = state.pivot[1], pz = state.pivot[2];
        for (let i = 0; i < sp.length; i += 3) {
            wp[i    ] = px + (sp[i    ] - px) * ax;
            wp[i + 1] = py + (sp[i + 1] - py) * ay;
            wp[i + 2] = pz + (sp[i + 2] - pz) * az;
        }
        const prim = state.primitive;
        prim.previewMesh(wp, prim.indices, prim.normals);
    }

    function commit(state) {
        if (!state.active) return null;
        const prim = state.primitive;
        const s    = state.accumScale.slice();
        const newP = new Float32Array(state.workingPositions);
        prim.updateGeometry(newP, prim.indices, prim.normals);
        clear(state);
        return { primitive: prim, scale: s };
    }

    function cancel(state) {
        if (!state.active) return;
        state.primitive.revertMesh();
        clear(state);
    }

    function clear(state) {
        state.active = false;
        state.primitive = null;
        state.startPositions = null;
        state.workingPositions = null;
        state.accumScale[0] = state.accumScale[1] = state.accumScale[2] = 1;
    }

    global.ScaleTool = { createState, begin, applyDelta, commit, cancel, clear };

})(typeof globalThis !== 'undefined' ? globalThis : this);
