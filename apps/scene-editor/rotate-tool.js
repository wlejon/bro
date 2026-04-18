// =============================================================================
// Rotate tool — rotate a SceneObject around a world-space pivot by composing
// quaternion rotations onto its local TRS. Mesh buffers are untouched: the
// object's world matrix (and all downstream rendering/picking/inference) is
// re-derived from the new translation + rotation.
//
// The gizmo fires world-space per-frame quaternion deltas. For an object
// with parent transform P and local TRS (t, q, s), applying a world-space
// rotation ΔQ about world pivot p means:
//   new_world = T(p) · ΔQ · T(-p) · old_world
// Peeling off the parent, the new local is:
//   new_local = P⁻¹ · T(p) · ΔQ · T(-p) · P · old_local
// For the common case parent = identity (top-level primitive), this reduces
// to the snapshot-start rotation composed with the accumulated ΔQ, plus a
// translation correction so the pivot stays fixed.
//
// We avoid parent composition for now (tools only run on top-level primitives
// until Phase 3 wires edit contexts) — ΔQ composes directly onto the start
// rotation and the pivot correction is computed in world space.
//
// Pure-state module — gizmo plumbing lives in app.js.
// =============================================================================

(function (global) {
    'use strict';

    const M = Mat4Lib;

    function createState() {
        return {
            active: false,
            object: null,
            pivot: [0, 0, 0],
            startTranslation: [0, 0, 0],
            startRotation: [0, 0, 0, 1],
            accumQ: [0, 0, 0, 1],
        };
    }

    function begin(state, object, pivot) {
        state.active = true;
        state.object = object;
        state.pivot[0] = pivot[0]; state.pivot[1] = pivot[1]; state.pivot[2] = pivot[2];
        state.startTranslation = object.translation.slice();
        state.startRotation    = object.rotation.slice();
        state.accumQ = [0, 0, 0, 1];
        applyDelta(state, 0, 0, 0, 1);
    }

    // Multiply per-frame world-space ΔQ into the accumulated rotation and
    // re-apply to the object: rotation = accumQ · startRotation, translation
    // = pivot + accumQ(startTranslation - pivot). Unaware of parent transforms
    // today (see module docstring).
    function applyDelta(state, qx, qy, qz, qw) {
        if (!state.active) return;
        state.accumQ = M.quatNorm(M.quatMul([qx, qy, qz, qw], state.accumQ));
        const obj = state.object;
        const newRot = M.quatMul(state.accumQ, state.startRotation);
        const rel = [
            state.startTranslation[0] - state.pivot[0],
            state.startTranslation[1] - state.pivot[1],
            state.startTranslation[2] - state.pivot[2],
        ];
        const rotated = M.quatRotVec(state.accumQ, rel);
        obj.setRotation(newRot);
        obj.setTranslation([
            state.pivot[0] + rotated[0],
            state.pivot[1] + rotated[1],
            state.pivot[2] + rotated[2],
        ]);
    }

    function commit(state) {
        if (!state.active) return null;
        const obj = state.object;
        const q   = state.accumQ.slice();
        clear(state);
        return { primitive: obj, object: obj, quat: q };
    }

    function cancel(state) {
        if (!state.active) return;
        state.object.setTranslation(state.startTranslation.slice());
        state.object.setRotation(state.startRotation.slice());
        clear(state);
    }

    function clear(state) {
        state.active = false;
        state.object = null;
        state.accumQ = [0, 0, 0, 1];
    }

    global.RotateTool = { createState, begin, applyDelta, commit, cancel, clear };

})(typeof globalThis !== 'undefined' ? globalThis : this);
