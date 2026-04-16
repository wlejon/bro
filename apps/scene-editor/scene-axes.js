// =============================================================================
// Scene axes — ground grid + colored XYZ axes.
//
// SketchUp-style orientation cue: a faint XZ grid plus three colored axis
// lines (X red, Y green, Z blue) so the user always knows which way is which.
// Geometry is packed into one mesh with per-vertex colors → one scene node,
// one draw call. Each "line" is a thin axis-aligned box (24 verts × 6 faces)
// for proper visibility from any orbit angle under GL_CULL_FACE + GL_BACK.
//
// The grid sits at a configurable Y plane (default y=-1, matching the bottom
// of the spike's centered cube) so the box visually rests on the ground.
// Axes pass through (0, gridY, 0) — the "ground origin" — extending equally
// in both directions along their axis.
// =============================================================================

(function (global) {
    'use strict';

    // Append a 24-vertex axis-aligned box (4 unique verts × 6 faces) with
    // per-face outward normals and a single per-vertex color. Per-face
    // duplication keeps lighting flat and outward-correct under back-face
    // culling — shared 8-vertex boxes would smooth-shade with diagonal
    // averaged normals, which look wrong on a thin line.
    function emitAABB(out, x0, y0, z0, x1, y1, z1, r, g, b, a) {
        // [4 verts (CCW from outside), [nx, ny, nz]]. Winding chosen so the
        // first cross product (v1-v0)×(v2-v1) equals the outward normal.
        const faces = [
            [[[x0, y0, z0], [x0, y1, z0], [x1, y1, z0], [x1, y0, z0]], [ 0,  0, -1]], // -Z
            [[[x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1]], [ 0,  0,  1]], // +Z
            [[[x0, y0, z0], [x1, y0, z0], [x1, y0, z1], [x0, y0, z1]], [ 0, -1,  0]], // -Y
            [[[x1, y1, z0], [x0, y1, z0], [x0, y1, z1], [x1, y1, z1]], [ 0,  1,  0]], // +Y
            [[[x0, y0, z0], [x0, y0, z1], [x0, y1, z1], [x0, y1, z0]], [-1,  0,  0]], // -X
            [[[x1, y0, z0], [x1, y1, z0], [x1, y1, z1], [x1, y0, z1]], [ 1,  0,  0]], // +X
        ];
        for (let f = 0; f < faces.length; f++) {
            const verts = faces[f][0];
            const n     = faces[f][1];
            const baseV = out.positions.length / 3;
            for (let v = 0; v < 4; v++) {
                const p = verts[v];
                out.positions.push(p[0], p[1], p[2]);
                out.normals.push(n[0], n[1], n[2]);
                out.colors.push(r, g, b, a);
            }
            out.indices.push(
                baseV + 0, baseV + 1, baseV + 2,
                baseV + 0, baseV + 2, baseV + 3);
        }
    }

    // Build a single MeshData-shaped object with grid + 3 axes.
    //
    // opts:
    //   extent      — grid spans [-extent, +extent] (default 10)
    //   step        — grid line spacing (default 1)
    //   lineThick   — grid line thickness (default 0.005)
    //   axisThick   — axis line thickness (default 0.02)
    //   axisLength  — axes extend ±axisLength from origin (default 5)
    //   gridY       — Y level of grid + axis intersection (default -1)
    //   gridColor   — RGBA float[4] (default faint grey)
    //   xColor/yColor/zColor — RGBA float[4] (defaults: red, green, blue)
    //
    // Returns { positions: Float32Array, normals: Float32Array,
    //          colors: Float32Array, indices: Uint32Array }
    function buildSceneAxes(opts) {
        opts = opts || {};
        const extent     = opts.extent     != null ? opts.extent     : 10;
        const step       = opts.step       != null ? opts.step       : 1;
        const lineThick  = opts.lineThick  != null ? opts.lineThick  : 0.005;
        const axisThick  = opts.axisThick  != null ? opts.axisThick  : 0.02;
        const axisLength = opts.axisLength != null ? opts.axisLength : 5;
        const gridY      = opts.gridY      != null ? opts.gridY      : -1;
        const gridColor  = opts.gridColor  || [0.55, 0.55, 0.55, 0.55];
        const xColor     = opts.xColor     || [0.93, 0.30, 0.30, 1.0];
        const yColor     = opts.yColor     || [0.30, 0.85, 0.30, 1.0];
        const zColor     = opts.zColor     || [0.30, 0.55, 0.95, 1.0];

        const out = { positions: [], normals: [], colors: [], indices: [] };
        const lt = lineThick * 0.5;
        const at = axisThick * 0.5;

        // Grid lines parallel to X (constant Z). Skip the Z=0 line — it lives
        // under the X axis (drawn larger + colored below).
        for (let i = -extent; i <= extent; i += step) {
            if (Math.abs(i) < 1e-6) continue;
            emitAABB(out,
                -extent, gridY - lt, i - lt,
                 extent, gridY + lt, i + lt,
                gridColor[0], gridColor[1], gridColor[2], gridColor[3]);
        }
        // Grid lines parallel to Z (constant X). Skip X=0 — under Z axis.
        for (let i = -extent; i <= extent; i += step) {
            if (Math.abs(i) < 1e-6) continue;
            emitAABB(out,
                i - lt, gridY - lt, -extent,
                i + lt, gridY + lt,  extent,
                gridColor[0], gridColor[1], gridColor[2], gridColor[3]);
        }

        // X axis (red), along ±X at ground level.
        emitAABB(out,
            -axisLength, gridY - at, -at,
             axisLength, gridY + at,  at,
            xColor[0], xColor[1], xColor[2], xColor[3]);
        // Z axis (blue), along ±Z at ground level.
        emitAABB(out,
            -at, gridY - at, -axisLength,
             at, gridY + at,  axisLength,
            zColor[0], zColor[1], zColor[2], zColor[3]);
        // Y axis (green), vertical through (0, gridY, 0). Extends both above
        // and below the ground — below is "underground" but still useful as
        // a reference for the negative-Y direction.
        emitAABB(out,
            -at, gridY - axisLength, -at,
             at, gridY + axisLength,  at,
            yColor[0], yColor[1], yColor[2], yColor[3]);

        return {
            positions: new Float32Array(out.positions),
            normals:   new Float32Array(out.normals),
            colors:    new Float32Array(out.colors),
            indices:   new Uint32Array(out.indices),
        };
    }

    global.SceneAxes = { buildSceneAxes, emitAABB };

})(typeof globalThis !== 'undefined' ? globalThis : this);
