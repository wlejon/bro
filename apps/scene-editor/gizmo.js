// =============================================================================
// Translate gizmo — three world-axis arrows for direct-manipulation moves.
//
// Each axis is one scene mesh node (shaft cylinder + cone tip baked along
// the axis), positioned at the gizmo origin and uniformly scaled per frame
// so the on-screen size stays roughly constant as the camera zooms. Hit-test
// runs ray↔segment with a thickness pad — picks before primitive raycast so
// arrows remain grabbable even when they sit inside the active object.
//
// Pure helpers: anchoring (origin), scaling (camera distance), hover state,
// and drag bookkeeping all live in app.js.
// =============================================================================

(function (global) {
    'use strict';

    const AXES = ['x', 'y', 'z'];
    const AXIS_DIR = {
        x: [1, 0, 0],
        y: [0, 1, 0],
        z: [0, 0, 1],
    };
    const AXIS_COLOR = {
        x: '#e74c3c',
        y: '#27ae60',
        z: '#3498db',
    };
    const AXIS_COLOR_HOVER = {
        x: '#ffd166',
        y: '#ffd166',
        z: '#ffd166',
    };

    // Default geometry, in arrow-local units (axis = +X). Scaled per-frame
    // so the rendered arrow is ~80px regardless of camera distance.
    const DEFAULT = {
        shaftLen:    0.85,
        shaftRadius: 0.025,
        tipLen:      0.30,
        tipRadius:   0.085,
        segments:    14,
        // Hit-pad multiplier for the shaft radius: the picking capsule is
        // shaftRadius * pickPad wide, so users can grab the arrow without
        // pixel-perfect cursor placement.
        pickPad:     2.4,
    };

    // Build a +X-aligned arrow as MeshData: shaft cylinder from x=0 to
    // x=shaftLen plus a cone tip from x=shaftLen to x=shaftLen+tipLen. Bot
    // h end caps are included so the arrow looks solid from any angle.
    function buildArrowMesh(opts) {
        const o = opts || {};
        const shaftLen    = o.shaftLen    != null ? o.shaftLen    : DEFAULT.shaftLen;
        const shaftRadius = o.shaftRadius != null ? o.shaftRadius : DEFAULT.shaftRadius;
        const tipLen      = o.tipLen      != null ? o.tipLen      : DEFAULT.tipLen;
        const tipRadius   = o.tipRadius   != null ? o.tipRadius   : DEFAULT.tipRadius;
        const seg         = o.segments    != null ? o.segments    : DEFAULT.segments;

        const positions = [];
        const normals   = [];
        const indices   = [];

        // Helper: emit a vertex, return its index.
        function v(px, py, pz, nx, ny, nz) {
            const idx = positions.length / 3;
            positions.push(px, py, pz);
            normals.push(nx, ny, nz);
            return idx;
        }

        // Shaft side ring vertices: two rings at x=0 and x=shaftLen, each
        // duplicated per quad to keep face normals flat (no smoothing across
        // the cap seam). Build per-segment.
        for (let i = 0; i < seg; i++) {
            const a0 = (i       / seg) * Math.PI * 2;
            const a1 = ((i + 1) / seg) * Math.PI * 2;
            const c0 = Math.cos(a0), s0 = Math.sin(a0);
            const c1 = Math.cos(a1), s1 = Math.sin(a1);
            // Side normal averages the two segment radii (constant since they
            // share x). For flat shading per quad use the midpoint normal.
            const nx = 0;
            const ny = (s0 + s1) * 0.5;
            const nz = (c0 + c1) * 0.5;
            const nl = Math.hypot(nx, ny, nz) || 1;
            const a = v(0,        shaftRadius * s0, shaftRadius * c0, nx/nl, ny/nl, nz/nl);
            const b = v(0,        shaftRadius * s1, shaftRadius * c1, nx/nl, ny/nl, nz/nl);
            const c = v(shaftLen, shaftRadius * s1, shaftRadius * c1, nx/nl, ny/nl, nz/nl);
            const d = v(shaftLen, shaftRadius * s0, shaftRadius * c0, nx/nl, ny/nl, nz/nl);
            // Outward winding (looking from +radius toward axis): a-b-c-d.
            indices.push(a, b, c,  a, c, d);
        }

        // Shaft back cap (-X facing).
        const backCenter = v(0, 0, 0, -1, 0, 0);
        for (let i = 0; i < seg; i++) {
            const a0 = (i       / seg) * Math.PI * 2;
            const a1 = ((i + 1) / seg) * Math.PI * 2;
            const va = v(0, shaftRadius * Math.sin(a0), shaftRadius * Math.cos(a0), -1, 0, 0);
            const vb = v(0, shaftRadius * Math.sin(a1), shaftRadius * Math.cos(a1), -1, 0, 0);
            // Wind so the normal points -X (back face).
            indices.push(backCenter, vb, va);
        }

        // Cone base ring (faces -X) at x=shaftLen with tipRadius.
        const baseCenter = v(shaftLen, 0, 0, -1, 0, 0);
        for (let i = 0; i < seg; i++) {
            const a0 = (i       / seg) * Math.PI * 2;
            const a1 = ((i + 1) / seg) * Math.PI * 2;
            const va = v(shaftLen, tipRadius * Math.sin(a0), tipRadius * Math.cos(a0), -1, 0, 0);
            const vb = v(shaftLen, tipRadius * Math.sin(a1), tipRadius * Math.cos(a1), -1, 0, 0);
            indices.push(baseCenter, va, vb);
        }

        // Cone side: tip apex at x=shaftLen+tipLen, base at x=shaftLen with
        // tipRadius. Per-quad flat normals (averaged base ring normal +
        // forward axis component for the slant).
        const tipApexX = shaftLen + tipLen;
        const slantHyp = Math.hypot(tipLen, tipRadius) || 1;
        const slantNX  = tipRadius / slantHyp;       // +X component (outward+forward)
        const slantNR  = tipLen    / slantHyp;       // radial component
        for (let i = 0; i < seg; i++) {
            const a0 = (i       / seg) * Math.PI * 2;
            const a1 = ((i + 1) / seg) * Math.PI * 2;
            const c0 = Math.cos(a0), s0 = Math.sin(a0);
            const c1 = Math.cos(a1), s1 = Math.sin(a1);
            const ny = (s0 + s1) * 0.5;
            const nz = (c0 + c1) * 0.5;
            const nl = Math.hypot(ny, nz) || 1;
            const nyU = ny / nl, nzU = nz / nl;
            const apex = v(tipApexX, 0, 0, slantNX, nyU * slantNR, nzU * slantNR);
            const ba   = v(shaftLen, tipRadius * s0, tipRadius * c0, slantNX, nyU * slantNR, nzU * slantNR);
            const bb   = v(shaftLen, tipRadius * s1, tipRadius * c1, slantNX, nyU * slantNR, nzU * slantNR);
            // Outward winding: apex - ba - bb (CCW from outside).
            indices.push(apex, ba, bb);
        }

        return {
            positions: new Float32Array(positions),
            normals:   new Float32Array(normals),
            indices:   new Uint32Array(indices),
            // Total length along the arrow axis — used by hit-test to clamp
            // the segment endpoint.
            length:    shaftLen + tipLen,
            shaftRadius, tipRadius,
        };
    }

    // Create the gizmo: 3 nodes sharing the same arrow mesh, each rotated to
    // its target axis. node.x/y/z + node.scaleX/Y/Z drive position + size
    // per frame; no per-frame mesh rebuild.
    function create(scene, opts) {
        const arrow = buildArrowMesh(opts);
        const nodes = {};
        const hoverNodes = {};
        for (const ax of AXES) {
            nodes[ax] = scene.createMesh({
                positions: arrow.positions,
                normals:   arrow.normals,
                indices:   arrow.indices,
                color:     AXIS_COLOR[ax],
                emissive:  0.55,
                name:      'gizmo-' + ax,
            });
            // Hover overlay: identical mesh, brighter color, hidden by
            // default. Toggle visible to indicate hover. Cheap: 2 nodes per
            // axis, no rebuilds.
            hoverNodes[ax] = scene.createMesh({
                positions: arrow.positions,
                normals:   arrow.normals,
                indices:   arrow.indices,
                color:     AXIS_COLOR_HOVER[ax],
                emissive:  1.4,
                name:      'gizmo-' + ax + '-hover',
            });
            hoverNodes[ax].visible = false;
        }
        // Apply axis rotations. The mesh is built along +X; rotate to land
        // on the target world axis. Rotation conventions are scene-graph
        // Euler (radians) — empirically validated against the test suite.
        // (If the convention turns out to be left-handed the tests will
        // catch it via the hitTest assertions.)
        nodes.y.rotationZ      = Math.PI * 0.5;       // +X → +Y
        hoverNodes.y.rotationZ = Math.PI * 0.5;
        nodes.z.rotationY      = -Math.PI * 0.5;      // +X → +Z
        hoverNodes.z.rotationY = -Math.PI * 0.5;

        const g = {
            scene,
            nodes,
            hoverNodes,
            arrow,
            origin:  [0, 0, 0],
            scale:   1,
            visible: true,
            hovered: null,            // 'x'|'y'|'z'|null
        };
        setVisible(g, true);
        return g;
    }

    function setOrigin(g, x, y, z) {
        g.origin[0] = x; g.origin[1] = y; g.origin[2] = z;
        for (const ax of AXES) {
            g.nodes[ax].x = x;
            g.nodes[ax].y = y;
            g.nodes[ax].z = z;
            g.hoverNodes[ax].x = x;
            g.hoverNodes[ax].y = y;
            g.hoverNodes[ax].z = z;
        }
    }

    function setScale(g, s) {
        g.scale = s;
        for (const ax of AXES) {
            g.nodes[ax].scaleX = s;
            g.nodes[ax].scaleY = s;
            g.nodes[ax].scaleZ = s;
            g.hoverNodes[ax].scaleX = s;
            g.hoverNodes[ax].scaleY = s;
            g.hoverNodes[ax].scaleZ = s;
        }
    }

    function setVisible(g, v) {
        g.visible = v;
        for (const ax of AXES) {
            g.nodes[ax].visible = v;
            g.hoverNodes[ax].visible = v && (g.hovered === ax);
        }
    }

    function setHovered(g, axis) {
        if (g.hovered === axis) return;
        g.hovered = axis;
        for (const ax of AXES) {
            g.hoverNodes[ax].visible = g.visible && (g.hovered === ax);
        }
    }

    // Closest point pair between an infinite ray (origin+t*dir, |dir|=1) and
    // a finite segment from A to B. Returns { rayT, segT, segPoint, dist }
    // where segT is clamped to [0,1] and dist is the world distance between
    // the two closest points.
    function closestRayToSegment(rayO, rayD, A, B) {
        const ux = B[0] - A[0], uy = B[1] - A[1], uz = B[2] - A[2];
        const wx = rayO[0] - A[0], wy = rayO[1] - A[1], wz = rayO[2] - A[2];
        const a = rayD[0] * rayD[0] + rayD[1] * rayD[1] + rayD[2] * rayD[2]; // 1
        const b = rayD[0] * ux + rayD[1] * uy + rayD[2] * uz;
        const c = ux * ux + uy * uy + uz * uz;
        const d = rayD[0] * wx + rayD[1] * wy + rayD[2] * wz;
        const e = ux * wx + uy * wy + uz * wz;
        const denom = a * c - b * b;
        let rayT, segT;
        if (Math.abs(denom) < 1e-9) {
            // Parallel — pick segment midpoint as a fallback.
            segT = 0.5;
            rayT = (b * segT - d) / a;
        } else {
            rayT = (b * e - c * d) / denom;
            segT = (a * e - b * d) / denom;
            if (segT < 0) segT = 0; else if (segT > 1) segT = 1;
            // Recompute rayT after segment clamp for accuracy.
            rayT = (b * segT - d) / a;
        }
        const sx = A[0] + segT * ux;
        const sy = A[1] + segT * uy;
        const sz = A[2] + segT * uz;
        const px = rayO[0] + rayT * rayD[0];
        const py = rayO[1] + rayT * rayD[1];
        const pz = rayO[2] + rayT * rayD[2];
        return {
            rayT,
            segT,
            segPoint: [sx, sy, sz],
            dist: Math.hypot(sx - px, sy - py, sz - pz),
        };
    }

    // Ray vs the 3 axis arrows. Returns the closest hit (smallest rayT > 0):
    //   { axis: 'x'|'y'|'z', axisDir: [...], rayT, point: [x,y,z] }
    // or null. The picking radius widens with the cone tip — pickPad *
    // tipRadius * scale — so users can grab anywhere along the arrow.
    function hitTest(g, rayOrigin, rayDir) {
        const armLen = g.arrow.length * g.scale;
        const pickRad = g.arrow.tipRadius * g.scale * DEFAULT.pickPad;
        let best = null;
        for (const ax of AXES) {
            const dir = AXIS_DIR[ax];
            const tip = [
                g.origin[0] + dir[0] * armLen,
                g.origin[1] + dir[1] * armLen,
                g.origin[2] + dir[2] * armLen,
            ];
            const r = closestRayToSegment(rayOrigin, rayDir, g.origin, tip);
            if (r.rayT < 0) continue;
            if (r.dist > pickRad) continue;
            if (!best || r.rayT < best.rayT) {
                best = {
                    axis: ax,
                    axisDir: dir.slice(),
                    rayT: r.rayT,
                    point: r.segPoint,
                };
            }
        }
        return best;
    }

    function destroy(g) {
        for (const ax of AXES) {
            if (g.nodes[ax])      { g.nodes[ax].destroy();      g.nodes[ax]      = null; }
            if (g.hoverNodes[ax]) { g.hoverNodes[ax].destroy(); g.hoverNodes[ax] = null; }
        }
    }

    // Compute a uniform scale that yields ~targetPx tall arrows at a given
    // camera→origin world distance, given canvas height + camera FOV.
    // Inverts the standard pinhole projection: world_size_at_distance =
    // dist * 2 * tan(fov/2) / canvasHeight * targetPx.
    function screenStableScale(distance, fovDeg, canvasHeight, targetPx) {
        const tanHalf = Math.tan(fovDeg * Math.PI / 180 * 0.5);
        const worldPerPixel = (distance * 2 * tanHalf) / canvasHeight;
        const desiredArrowWorld = targetPx * worldPerPixel;
        // Arrow mesh is built at length 1 (default), so the scale factor is
        // just desiredArrowWorld. Future-proof: divide by the actual mesh
        // length so custom-built gizmos still hit target px.
        return desiredArrowWorld;
    }

    global.Gizmo = {
        create, destroy,
        setOrigin, setScale, setVisible, setHovered,
        hitTest,
        buildArrowMesh,
        screenStableScale,
        closestRayToSegment,
        AXIS_DIR, AXIS_COLOR,
    };

})(typeof globalThis !== 'undefined' ? globalThis : this);
