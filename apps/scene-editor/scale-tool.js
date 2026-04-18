// =============================================================================
// Scale tool — per-axis scale about a world-space pivot by composing into the
// SceneObject's local scale + translation. No mesh-buffer mutation.
//
// The gizmo feeds per-frame multiplicative factors. For parent=identity (the
// only case until edit contexts ship):
//   new_scale       = startScale * accumScale
//   new_translation = pivot + accumScale · (startTranslation - pivot)
// The scale * rel is applied *in the object's world frame axes* — axis-
// aligned scaling via the gizmo matches this assumption.
//
// Pure-state module — gizmo plumbing lives in app.js.
// =============================================================================

(function (global) {
    'use strict';

    function createState() {
        return {
            active: false,
            object: null,
            pivot: [0, 0, 0],
            startTranslation: [0, 0, 0],
            startScale: [1, 1, 1],
            accumScale: [1, 1, 1],
        };
    }

    function begin(state, object, pivot) {
        state.active = true;
        state.object = object;
        state.pivot[0] = pivot[0]; state.pivot[1] = pivot[1]; state.pivot[2] = pivot[2];
        state.startTranslation = object.translation.slice();
        state.startScale       = object.scale.slice();
        state.accumScale[0] = state.accumScale[1] = state.accumScale[2] = 1;
        applyDelta(state, 1, 1, 1);
    }

    function applyDelta(state, sx, sy, sz) {
        if (!state.active) return;
        state.accumScale[0] *= sx;
        state.accumScale[1] *= sy;
        state.accumScale[2] *= sz;
        const ax = state.accumScale[0], ay = state.accumScale[1], az = state.accumScale[2];
        const obj = state.object;
        obj.setScale([
            state.startScale[0] * ax,
            state.startScale[1] * ay,
            state.startScale[2] * az,
        ]);
        obj.setTranslation([
            state.pivot[0] + (state.startTranslation[0] - state.pivot[0]) * ax,
            state.pivot[1] + (state.startTranslation[1] - state.pivot[1]) * ay,
            state.pivot[2] + (state.startTranslation[2] - state.pivot[2]) * az,
        ]);
    }

    function commit(state) {
        if (!state.active) return null;
        const obj = state.object;
        const s   = state.accumScale.slice();
        clear(state);
        return { primitive: obj, object: obj, scale: s };
    }

    function cancel(state) {
        if (!state.active) return;
        state.object.setTranslation(state.startTranslation.slice());
        state.object.setScale(state.startScale.slice());
        clear(state);
    }

    function clear(state) {
        state.active = false;
        state.object = null;
        state.accumScale[0] = state.accumScale[1] = state.accumScale[2] = 1;
    }

    global.ScaleTool = { createState, begin, applyDelta, commit, cancel, clear };

})(typeof globalThis !== 'undefined' ? globalThis : this);
