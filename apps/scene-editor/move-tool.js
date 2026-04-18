// =============================================================================
// Move tool — translate a SceneObject (primitive, group, or component
// instance) by mutating its local translation. No mesh-buffer rewrites:
// rendering and inference compose through the object's world matrix, so a
// move is just `object.setTranslation(start + delta)`.
//
// During a drag we keep the start translation around so apply/cancel/commit
// round-trip cleanly. The pivot captures the world-space grab point so the
// plane-intersection math in app.js is unchanged.
//
// Pure-state module — camera, input, and snap resolution live in app.js.
// =============================================================================

(function (global) {
    'use strict';

    function createState() {
        return {
            active: false,
            object: null,
            pivot: [0, 0, 0],
            planeNormal: [0, 0, 1],
            startTranslation: [0, 0, 0],
            delta: [0, 0, 0],
        };
    }

    function begin(state, object, pivot, planeNormal) {
        state.active = true;
        state.object = object;
        state.pivot[0] = pivot[0];
        state.pivot[1] = pivot[1];
        state.pivot[2] = pivot[2];
        state.planeNormal[0] = planeNormal[0];
        state.planeNormal[1] = planeNormal[1];
        state.planeNormal[2] = planeNormal[2];
        state.startTranslation[0] = object.translation[0];
        state.startTranslation[1] = object.translation[1];
        state.startTranslation[2] = object.translation[2];
        state.delta[0] = state.delta[1] = state.delta[2] = 0;
        applyDelta(state, 0, 0, 0);
    }

    function applyDelta(state, dx, dy, dz) {
        if (!state.active) return;
        state.delta[0] = dx;
        state.delta[1] = dy;
        state.delta[2] = dz;
        state.object.setTranslation([
            state.startTranslation[0] + dx,
            state.startTranslation[1] + dy,
            state.startTranslation[2] + dz,
        ]);
    }

    function commit(state) {
        if (!state.active) return null;
        const obj   = state.object;
        const delta = state.delta.slice();
        clear(state);
        return { primitive: obj, object: obj, delta };
    }

    function cancel(state) {
        if (!state.active) return;
        state.object.setTranslation(state.startTranslation.slice());
        clear(state);
    }

    function clear(state) {
        state.active = false;
        state.object = null;
        state.delta[0] = state.delta[1] = state.delta[2] = 0;
    }

    function rayVsPlane(ray, planePoint, planeNormal) {
        const denom = ray.dir[0] * planeNormal[0] +
                      ray.dir[1] * planeNormal[1] +
                      ray.dir[2] * planeNormal[2];
        if (Math.abs(denom) < 1e-6) return null;
        const wx = planePoint[0] - ray.origin[0];
        const wy = planePoint[1] - ray.origin[1];
        const wz = planePoint[2] - ray.origin[2];
        const t  = (wx * planeNormal[0] +
                    wy * planeNormal[1] +
                    wz * planeNormal[2]) / denom;
        if (t < 0) return null;
        return [
            ray.origin[0] + t * ray.dir[0],
            ray.origin[1] + t * ray.dir[1],
            ray.origin[2] + t * ray.dir[2],
        ];
    }

    global.MoveTool = {
        createState,
        begin, applyDelta, commit, cancel, clear,
        rayVsPlane,
    };

})(typeof globalThis !== 'undefined' ? globalThis : this);
