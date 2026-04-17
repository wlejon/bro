// =============================================================================
// Rotate tool — rotate a whole primitive around its pivot (bbox centroid).
//
// Driven by the engine gizmo's rotate rings: each frame the gizmo fires a
// per-frame quaternion delta, which we multiply into an accumulated quat and
// re-apply to a snapshot of the start positions + normals. Preview goes
// through Primitive.previewMesh (no canonical mutation until commit). Commit
// bakes via updateGeometry so BVH / face groups / inference / edges all
// rebuild; cancel rolls back via revertMesh.
//
// Pure-state module: camera / input / gizmo plumbing lives in app.js.
// =============================================================================

(function (global) {
    'use strict';

    // Quaternion helpers (quat = [x, y, z, w]).
    function quatMul(a, b) {
        return [
            a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1],
            a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0],
            a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3],
            a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2],
        ];
    }
    function quatNorm(q) {
        const m = Math.hypot(q[0], q[1], q[2], q[3]) || 1;
        return [q[0]/m, q[1]/m, q[2]/m, q[3]/m];
    }
    function quatRotVec(q, v) {
        const x = q[0], y = q[1], z = q[2], w = q[3];
        const vx = v[0], vy = v[1], vz = v[2];
        const tx = 2 * (y*vz - z*vy);
        const ty = 2 * (z*vx - x*vz);
        const tz = 2 * (x*vy - y*vx);
        return [
            vx + w*tx + (y*tz - z*ty),
            vy + w*ty + (z*tx - x*tz),
            vz + w*tz + (x*ty - y*tx),
        ];
    }

    function createState() {
        return {
            active: false,
            primitive: null,
            pivot: [0, 0, 0],
            startPositions: null,
            startNormals:   null,
            workingPositions: null,
            workingNormals:   null,
            accumQ: [0, 0, 0, 1],
        };
    }

    function begin(state, primitive, pivot) {
        state.active = true;
        state.primitive = primitive;
        state.pivot[0] = pivot[0]; state.pivot[1] = pivot[1]; state.pivot[2] = pivot[2];
        state.startPositions   = new Float32Array(primitive.positions);
        state.startNormals     = new Float32Array(primitive.normals);
        state.workingPositions = new Float32Array(primitive.positions.length);
        state.workingNormals   = new Float32Array(primitive.normals.length);
        state.accumQ = [0, 0, 0, 1];
        applyDelta(state, 0, 0, 0, 1);
    }

    // Multiply the per-frame world-space quaternion delta into the accumulated
    // rotation and re-apply to the start buffers. Previews through the
    // primitive's render node.
    function applyDelta(state, qx, qy, qz, qw) {
        if (!state.active) return;
        state.accumQ = quatNorm(quatMul([qx, qy, qz, qw], state.accumQ));
        const q  = state.accumQ;
        const sp = state.startPositions, wp = state.workingPositions;
        const px = state.pivot[0], py = state.pivot[1], pz = state.pivot[2];
        for (let i = 0; i < sp.length; i += 3) {
            const r = quatRotVec(q, [sp[i] - px, sp[i+1] - py, sp[i+2] - pz]);
            wp[i] = r[0] + px; wp[i+1] = r[1] + py; wp[i+2] = r[2] + pz;
        }
        const sn = state.startNormals, wn = state.workingNormals;
        for (let i = 0; i < sn.length; i += 3) {
            const r = quatRotVec(q, [sn[i], sn[i+1], sn[i+2]]);
            wn[i] = r[0]; wn[i+1] = r[1]; wn[i+2] = r[2];
        }
        state.primitive.previewMesh(wp, state.primitive.indices, wn);
    }

    function commit(state) {
        if (!state.active) return null;
        const prim = state.primitive;
        const q    = state.accumQ.slice();
        const newP = new Float32Array(state.workingPositions);
        const newN = new Float32Array(state.workingNormals);
        prim.updateGeometry(newP, prim.indices, newN);
        clear(state);
        return { primitive: prim, quat: q };
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
        state.startNormals   = null;
        state.workingPositions = null;
        state.workingNormals   = null;
        state.accumQ = [0, 0, 0, 1];
    }

    global.RotateTool = { createState, begin, applyDelta, commit, cancel, clear };

})(typeof globalThis !== 'undefined' ? globalThis : this);
