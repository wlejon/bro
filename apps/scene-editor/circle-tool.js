// =============================================================================
// Circle tool — click-center + click-radius drawing. Commit produces a
// triangulated regular polygon (the approximation SketchUp uses for circles
// too; "circle" in SketchUp is an N-segment polyline under the hood).
//
// Pure-state module. The app walks the tool through begin → update* →
// commit/cancel, and owns input routing, preview rendering, and primitive
// creation. We just hold:
//   - the sketch plane (origin + normal + u/v basis)
//   - center (first click)
//   - edge (live cursor, updated on each mouse move)
//   - segment count (default 32)
// =============================================================================

(function (global) {
    'use strict';

    const DEFAULT_SEGMENTS = 32;

    function createState() {
        return {
            active:   false,
            plane:    null,      // { origin, normal, u, v }
            center:   null,
            edge:     null,
            segments: DEFAULT_SEGMENTS,
        };
    }

    function begin(state, plane, pos, segments) {
        state.active   = true;
        state.plane    = plane;
        state.center   = pos.slice();
        state.edge     = pos.slice();
        state.segments = segments || DEFAULT_SEGMENTS;
    }

    function update(state, pos) {
        if (!state.active) return;
        state.edge = pos.slice();
    }

    function radius(state) {
        if (!state.active || !state.center || !state.edge) return 0;
        const dx = state.edge[0] - state.center[0];
        const dy = state.edge[1] - state.center[1];
        const dz = state.edge[2] - state.center[2];
        return Math.sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Build a triangulated Mesh for the current circle, or null while the
    // radius is degenerate. Uses Sketch.circlePolyline to lay out N verts
    // on the plane, then Mesh.polygon3D to triangulate.
    function buildMesh(state) {
        if (!state.active || !state.plane) return null;
        const r = radius(state);
        if (r < 1e-9) return null;
        const ring = Sketch.circlePolyline(
            state.center, r, state.plane.normal, state.segments);
        const flat = Sketch.flatten3D(ring);
        const m = Mesh.polygon3D(flat, [], state.plane.normal);
        if (!m || m.vertexCount === 0) return null;
        return m;
    }

    function commit(state) {
        const m = buildMesh(state);
        clear(state);
        return m;
    }

    function cancel(state) {
        clear(state);
    }

    function clear(state) {
        state.active = false;
        state.plane  = null;
        state.center = null;
        state.edge   = null;
        state.segments = DEFAULT_SEGMENTS;
    }

    global.CircleTool = {
        createState, begin, update, radius, buildMesh, commit, cancel,
        DEFAULT_SEGMENTS,
    };

})(typeof globalThis !== 'undefined' ? globalThis : this);
