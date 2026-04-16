// =============================================================================
// Move tool — translate a whole primitive.
//
// Drag plane is camera-facing through the grab pivot, so mouse motion maps
// 1:1 to world distance at the pivot's depth. Each frame the app resolves an
// inference snap (across visible primitives, excluding the moving one) and
// either uses the snap position or the ray↔plane intersection as the drag
// target — delta = target - pivot. Writes start+delta into a working buffer
// and previews the primitive without mutating canonical state; commit bakes
// via Primitive.updateGeometry which rebuilds BVH / face groups / inference /
// edges.
//
// Pure-state module: mouse, camera, and snap resolution live in app.js.
// =============================================================================

(function (global) {
    'use strict';

    function createState() {
        return {
            active: false,
            primitive: null,
            pivot: [0, 0, 0],          // world-space grab point
            planeNormal: [0, 0, 1],    // camera forward at begin()
            startPositions: null,      // Float32Array snapshot of prim.positions
            workingPositions: null,    // Float32Array scratch (start + delta)
            delta: [0, 0, 0],
        };
    }

    // Begin a move drag on `primitive` pivoted at `pivot` with drag plane
    // defined by `planeNormal`. The state captures the pre-drag positions so
    // apply/cancel round-trip cleanly.
    function begin(state, primitive, pivot, planeNormal) {
        state.active = true;
        state.primitive = primitive;
        state.pivot[0] = pivot[0];
        state.pivot[1] = pivot[1];
        state.pivot[2] = pivot[2];
        state.planeNormal[0] = planeNormal[0];
        state.planeNormal[1] = planeNormal[1];
        state.planeNormal[2] = planeNormal[2];
        state.startPositions   = new Float32Array(primitive.positions);
        state.workingPositions = new Float32Array(primitive.positions.length);
        state.delta[0] = state.delta[1] = state.delta[2] = 0;
        // Seed at zero delta so a click-release with no motion commits a
        // no-op rather than baking a zero-filled workingPositions buffer.
        applyDelta(state, 0, 0, 0);
    }

    // Write start + delta into working positions and preview the primitive.
    // Translation preserves indices and normals so we reuse the canonical
    // versions for the preview.
    function applyDelta(state, dx, dy, dz) {
        if (!state.active) return;
        state.delta[0] = dx;
        state.delta[1] = dy;
        state.delta[2] = dz;
        const start = state.startPositions;
        const work  = state.workingPositions;
        const n = start.length;
        for (let i = 0; i < n; i += 3) {
            work[i    ] = start[i    ] + dx;
            work[i + 1] = start[i + 1] + dy;
            work[i + 2] = start[i + 2] + dz;
        }
        const prim = state.primitive;
        prim.previewMesh(work, prim.indices, prim.normals);
    }

    // Bake the current delta into the primitive. Goes through updateGeometry
    // so BVH / face groups / inference / edges all rebuild — even though
    // translation alone preserves indices and normals.
    function commit(state) {
        if (!state.active) return null;
        const prim  = state.primitive;
        const delta = state.delta.slice();
        const newPositions = new Float32Array(state.workingPositions);
        prim.updateGeometry(newPositions, prim.indices, prim.normals);
        clear(state);
        return { primitive: prim, delta };
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
        state.delta[0] = state.delta[1] = state.delta[2] = 0;
    }

    // Ray↔plane intersection in world space. Plane is defined by a point and
    // a normal (need not be unit-length but caller usually passes a unit
    // vector). Returns the intersection point or null if the ray is parallel
    // to the plane or hits behind the origin.
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
