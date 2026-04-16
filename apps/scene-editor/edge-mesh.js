// =============================================================================
// Edge mesh — thin oriented-prism geometry for model edges.
//
// Turns inferenceGeo.edges into visible silhouette lines so the model reads
// as a SketchUp-style mesh (edges-first) rather than a flat-shaded blob.
// Each edge becomes a thin axis-aligned-in-edge-frame prism with per-face
// outward normals + uniform color; all edges pack into one mesh so it costs
// one scene node and one draw call.
//
// Why a 3D prism instead of a line list:
//   - The renderer uses GL_CULL_FACE + GL_BACK with per-vertex normals; a
//     line primitive would need a separate pipeline. A thin prism runs
//     through the existing mesh path with zero engine changes.
//   - Oriented (not axis-aligned) so diagonal edges render cleanly — the
//     scene-axes helper emits axis-aligned boxes and can't handle arbitrary
//     directions. Kept separate so each module stays focused.
//
// Edges straddle two adjacent faces; the prism protrudes half-thickness
// past each face's plane, so the outer half is visible even if the inner
// half z-fights with the face. No explicit z-offset is applied.
// =============================================================================

(function (global) {
    'use strict';

    // Build an oriented basis {u, v, w} with w along `dir`. Chooses an
    // initial up vector that isn't parallel to `dir` to keep the cross
    // product well-conditioned; the resulting u/v are an arbitrary rotation
    // around w, which is fine because the prism is rotationally symmetric
    // around its length axis.
    function orthoBasis(dir) {
        const wx = dir[0], wy = dir[1], wz = dir[2];
        // Pick a reference direction least parallel to w.
        let ax = 0, ay = 1, az = 0;
        if (Math.abs(wy) > 0.9) { ax = 1; ay = 0; az = 0; }
        // u = normalize(w × a)
        let ux = wy * az - wz * ay;
        let uy = wz * ax - wx * az;
        let uz = wx * ay - wy * ax;
        const ul = Math.hypot(ux, uy, uz) || 1;
        ux /= ul; uy /= ul; uz /= ul;
        // v = w × u  (already unit since w and u are unit + orthogonal)
        const vx = wy * uz - wz * uy;
        const vy = wz * ux - wx * uz;
        const vz = wx * uy - wy * ux;
        return [ux, uy, uz, vx, vy, vz];
    }

    // Emit one oriented prism for a segment a→b with half-thickness `ht`.
    // Winding is chosen so each face's (v1-v0)×(v2-v1) equals the outward
    // normal, matching scene-axes.js#emitAABB's convention — required for
    // GL_CULL_FACE + GL_BACK to show the outer surfaces.
    function emitEdge(out, a, b, ht, color) {
        const dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
        const L = Math.hypot(dx, dy, dz);
        if (L < 1e-9) return;
        const wx = dx / L, wy = dy / L, wz = dz / L;
        const [ux, uy, uz, vx, vy, vz] = orthoBasis([wx, wy, wz]);

        // 8 corners of the prism. `sign` packs as [±u, ±v, 0|1 for b-end].
        function corner(su, sv, endB) {
            const base = endB ? b : a;
            return [
                base[0] + su * ht * ux + sv * ht * vx,
                base[1] + su * ht * uy + sv * ht * vy,
                base[2] + su * ht * uz + sv * ht * vz,
            ];
        }
        const c000 = corner(-1, -1, 0);
        const c010 = corner(-1,  1, 0);
        const c110 = corner( 1,  1, 0);
        const c100 = corner( 1, -1, 0);
        const c001 = corner(-1, -1, 1);
        const c011 = corner(-1,  1, 1);
        const c111 = corner( 1,  1, 1);
        const c101 = corner( 1, -1, 1);

        const r = color[0], g = color[1], bb = color[2], aa = color[3];

        // Six faces, each as [4 verts CCW from outside, outward normal].
        const faces = [
            [[c000, c010, c110, c100], [-wx, -wy, -wz]],  // -W end cap (at a)
            [[c001, c101, c111, c011], [ wx,  wy,  wz]],  // +W end cap (at b)
            [[c000, c001, c011, c010], [-ux, -uy, -uz]],  // -U side
            [[c100, c110, c111, c101], [ ux,  uy,  uz]],  // +U side
            [[c000, c100, c101, c001], [-vx, -vy, -vz]],  // -V side
            [[c010, c011, c111, c110], [ vx,  vy,  vz]],  // +V side
        ];
        for (let f = 0; f < faces.length; f++) {
            const verts = faces[f][0];
            const n     = faces[f][1];
            const baseV = out.positions.length / 3;
            for (let i = 0; i < 4; i++) {
                const p = verts[i];
                out.positions.push(p[0], p[1], p[2]);
                out.normals.push(n[0], n[1], n[2]);
                out.colors.push(r, g, bb, aa);
            }
            out.indices.push(
                baseV + 0, baseV + 1, baseV + 2,
                baseV + 0, baseV + 2, baseV + 3);
        }
    }

    // Build one MeshData-shaped payload for a set of edges.
    //
    //   positions: Float32Array (deduped world positions, xyz interleaved)
    //   edges:     [{ a: idx, b: idx }, ...] (indices into positions)
    //   opts:
    //     thickness — prism side length perpendicular to edge (default 0.01)
    //     color     — RGBA float[4] (default dark slate)
    //
    // Returns { positions: Float32Array, normals: Float32Array,
    //          colors: Float32Array, indices: Uint32Array } — plug straight
    // into scene.createMesh({positions, normals, colors, indices, ...}).
    function buildEdgeMesh(positions, edges, opts) {
        opts = opts || {};
        const thickness = opts.thickness != null ? opts.thickness : 0.01;
        const color = opts.color || [0.17, 0.24, 0.31, 1.0];
        const ht = thickness * 0.5;

        const out = { positions: [], normals: [], colors: [], indices: [] };
        for (const e of edges) {
            const a = [
                positions[e.a * 3 + 0],
                positions[e.a * 3 + 1],
                positions[e.a * 3 + 2],
            ];
            const b = [
                positions[e.b * 3 + 0],
                positions[e.b * 3 + 1],
                positions[e.b * 3 + 2],
            ];
            emitEdge(out, a, b, ht, color);
        }
        return {
            positions: new Float32Array(out.positions),
            normals:   new Float32Array(out.normals),
            colors:    new Float32Array(out.colors),
            indices:   new Uint32Array(out.indices),
        };
    }

    global.EdgeMesh = { buildEdgeMesh, orthoBasis };

})(typeof globalThis !== 'undefined' ? globalThis : this);
