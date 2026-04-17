// =============================================================================
// Rectangle tool — click-click drawing of an axis-aligned rectangle on the
// current sketch plane. Produces a triangulated face primitive on commit.
//
// Pure-state module: ray resolution, preview rendering, and primitive
// creation live in app.js. The tool holds:
//   - the sketch plane (origin + normal + uv basis)
//   - corner 0 (first click)
//   - corner 1 (live cursor, updated on each mouse move)
//
// The app walks the tool through begin → update* → commit/cancel.
// =============================================================================

(function (global) {
    'use strict';

    function createState() {
        return {
            active:  false,
            plane:   null,       // { origin, normal, u, v }
            corner0: null,
            corner1: null,
        };
    }

    // Anchor the first corner. `plane` is the sketch plane resolved by the
    // app (ground plane today; face-pick later). corner1 starts equal to
    // corner0 so degenerate zero-area state is explicit rather than stale.
    function begin(state, plane, pos) {
        state.active  = true;
        state.plane   = plane;
        state.corner0 = pos.slice();
        state.corner1 = pos.slice();
    }

    function update(state, pos) {
        if (!state.active) return;
        state.corner1 = pos.slice();
    }

    // Return the 4 rectangle corners in CCW order (front-face toward
    // +plane.normal) — or null while the rect is degenerate.
    function corners(state) {
        if (!state.active || !state.plane) return null;
        const { u, v } = state.plane;
        return Sketch.rectFromCorners(state.corner0, state.corner1, u, v);
    }

    // Build a triangulated Mesh for the current rectangle, or null if the
    // input is degenerate (zero width or height). The Mesh.polygon3D path
    // tolerates degenerate input by returning empty, but the zero-area
    // check here is cheaper and ensures the caller never sees a 4-coincident
    // -point "rectangle" for any reason.
    function buildMesh(state) {
        if (!state.active || !state.plane) return null;
        const sz = size(state);
        if (!sz || sz.w < 1e-9 || sz.h < 1e-9) return null;
        const c = corners(state);
        if (!c) return null;
        const flat = Sketch.flatten3D(c);
        const m = Mesh.polygon3D(flat, [], state.plane.normal);
        if (!m || m.vertexCount === 0) return null;
        return m;
    }

    // Dimensions of the rectangle in plane-local (u, v) units — handy for
    // the VCB HUD and future "type WxH + Enter" exact entry.
    function size(state) {
        if (!state.active || !state.plane) return null;
        const { u, v } = state.plane;
        const uv = Sketch.project3Dto2D(state.corner1, state.corner0, u, v);
        return { w: Math.abs(uv[0]), h: Math.abs(uv[1]) };
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
        state.active  = false;
        state.plane   = null;
        state.corner0 = null;
        state.corner1 = null;
    }

    global.RectangleTool = {
        createState, begin, update, corners, buildMesh, size, commit, cancel,
    };

})(typeof globalThis !== 'undefined' ? globalThis : this);
