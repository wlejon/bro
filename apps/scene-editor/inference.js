// =============================================================================
// Inference engine — SketchUp-style cursor snapping.
//
// Resolves a cursor position into the highest-priority snap candidate from a
// pre-computed feature index (unique vertices + model edges). Results are pure
// data — caller renders the visual marker and decides how to consume the snap
// (e.g. push/pull projects the snap point onto its drag axis to lock depth).
//
// Snap priority (highest first):
//   1. endpoint  — at a unique vertex position             (green dot)
//   2. midpoint  — at the centre of a model edge           (cyan diamond)
//   3. on-edge   — closest point along a model edge        (red square)
//   4. on-face   — fallback to a BVH face hit              (blue diamond)
//
// "Model edges" are edges that border two distinct face groups (or fewer than
// two triangles, i.e. a mesh boundary). Interior coplanar-triangle splits do
// NOT generate snap candidates — they aren't visible to the user as edges.
// Match priority is broken first by category, then by screen-space distance.
// =============================================================================

(function (global) {
    'use strict';

    const POS_QUANT = 1e5;
    function posKey(x, y, z) {
        return Math.round(x * POS_QUANT) + ',' +
               Math.round(y * POS_QUANT) + ',' +
               Math.round(z * POS_QUANT);
    }

    // Build the snap-feature index for a single mesh. Re-run after any
    // commit that mutates positions/indices (e.g. push/pull).
    //   positions: Float32Array, xyz interleaved
    //   indices:   Uint32Array, triangle indices into positions
    //   faceGroups: { groups: [{tris,normal}], triToGroup: Int32Array }
    //
    // Returns { positions, vertCount, edges } where positions is a deduped
    // Float32Array (one entry per unique world-space position) and edges is
    // an array of { a, b } indexing into that deduped list.
    function buildInferenceGeo(positions, indices, faceGroups) {
        const keyToIdx = new Map();
        const uniquePositions = [];
        function getOrAddPos(x, y, z) {
            const k = posKey(x, y, z);
            let idx = keyToIdx.get(k);
            if (idx === undefined) {
                idx = uniquePositions.length / 3;
                keyToIdx.set(k, idx);
                uniquePositions.push(x, y, z);
            }
            return idx;
        }

        const triCount = indices.length / 3;
        const edgeMap = new Map();   // 'a,b' -> { a, b, groups, triCount }
        function edgeKey(a, b) { return a < b ? a + ',' + b : b + ',' + a; }

        for (let t = 0; t < triCount; t++) {
            const g = faceGroups.triToGroup[t];
            const ua = [0, 0, 0];
            for (let k = 0; k < 3; k++) {
                const vi = indices[t * 3 + k];
                ua[k] = getOrAddPos(
                    positions[vi * 3 + 0],
                    positions[vi * 3 + 1],
                    positions[vi * 3 + 2]);
            }
            for (let e = 0; e < 3; e++) {
                const a = ua[e], b = ua[(e + 1) % 3];
                if (a === b) continue;
                const k = edgeKey(a, b);
                let rec = edgeMap.get(k);
                if (!rec) {
                    rec = { a, b, groups: new Set(), triCount: 0 };
                    edgeMap.set(k, rec);
                }
                rec.groups.add(g);
                rec.triCount++;
            }
        }

        // Visible model edge: separates two distinct face groups, OR has
        // fewer than two triangle incidences (= mesh boundary).
        const edges = [];
        for (const rec of edgeMap.values()) {
            if (rec.groups.size > 1 || rec.triCount < 2) {
                edges.push({ a: rec.a, b: rec.b });
            }
        }

        return {
            positions: new Float32Array(uniquePositions),
            vertCount: uniquePositions.length / 3,
            edges,
        };
    }

    // Project a world point to screen pixels using orbitViewOpts-shape
    // camera options { position, target, up, fov }. Aspect is derived from
    // the supplied viewport dimensions — same source the engine uses when
    // building the projection matrix from the canvas FBO. Mirrors the basis
    // derivation in app.js#screenToRay so projection and unprojection
    // round-trip consistently.
    function worldToScreen(world, camOpts, width, height) {
        const fx = camOpts.target[0] - camOpts.position[0];
        const fy = camOpts.target[1] - camOpts.position[1];
        const fz = camOpts.target[2] - camOpts.position[2];
        const fl = Math.hypot(fx, fy, fz) || 1;
        const f  = [fx / fl, fy / fl, fz / fl];

        const up = camOpts.up;
        let rx = f[1] * up[2] - f[2] * up[1];
        let ry = f[2] * up[0] - f[0] * up[2];
        let rz = f[0] * up[1] - f[1] * up[0];
        const rl = Math.hypot(rx, ry, rz) || 1;
        rx /= rl; ry /= rl; rz /= rl;

        const ux = ry * f[2] - rz * f[1];
        const uy = rz * f[0] - rx * f[2];
        const uz = rx * f[1] - ry * f[0];

        const vx = world[0] - camOpts.position[0];
        const vy = world[1] - camOpts.position[1];
        const vz = world[2] - camOpts.position[2];

        const xc = vx * rx + vy * ry + vz * rz;
        const yc = vx * ux + vy * uy + vz * uz;
        const zc = vx * f[0] + vy * f[1] + vz * f[2];
        if (zc <= 1e-6) return { x: 0, y: 0, depth: zc, behind: true };

        const tanHalf = Math.tan(camOpts.fov * Math.PI / 180 * 0.5);
        const aspect = width / Math.max(1, height);
        const nx = (xc / zc) / (aspect * tanHalf);
        const ny = (yc / zc) / tanHalf;
        return {
            x: (nx + 1) * 0.5 * width,
            y: (1 - ny) * 0.5 * height,
            depth: zc,
            behind: false,
        };
    }

    // Closest point between segment a→b and ray (origin, dir, |dir|=1).
    // Solves the standard 2-parameter least-squares system; clamps the
    // segment parameter to [0,1] but does NOT clamp the ray parameter
    // (callers test cursor proximity in screen space, where rays behind
    // the camera are filtered by worldToScreen.behind).
    function closestPointOnSegmentToRay(a, b, ray) {
        const dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
        const wx = a[0] - ray.origin[0],
              wy = a[1] - ray.origin[1],
              wz = a[2] - ray.origin[2];
        const A = dx * dx + dy * dy + dz * dz;
        const B = dx * ray.dir[0] + dy * ray.dir[1] + dz * ray.dir[2];
        const C = dx * wx + dy * wy + dz * wz;
        const D = ray.dir[0] * wx + ray.dir[1] * wy + ray.dir[2] * wz;
        const denom = A - B * B;
        let t;
        if (Math.abs(denom) < 1e-9) t = 0;
        else t = (B * D - C) / denom;
        if (t < 0) t = 0; else if (t > 1) t = 1;
        return { point: [a[0] + t * dx, a[1] + t * dy, a[2] + t * dz], t };
    }

    const PRIORITY = { endpoint: 1, midpoint: 2, 'on-edge': 3, 'on-face': 4 };
    const COLOR    = {
        endpoint: '#2ecc71',
        midpoint: '#1abc9c',
        'on-edge': '#e74c3c',
        'on-face': '#3498db',
    };
    const LABEL    = {
        endpoint: 'Endpoint',
        midpoint: 'Midpoint',
        'on-edge': 'On Edge',
        'on-face': 'On Face',
    };

    // Find the best snap within `tol` pixels of (cursorX, cursorY).
    //
    // opts: {
    //   cursorX, cursorY,           // canvas-relative cursor pixels
    //   ray: { origin, dir },       // unit dir
    //   camOpts: orbitViewOpts shape,
    //   width, height,              // canvas pixels
    //   geo:  buildInferenceGeo result, OR
    //   geos: [buildInferenceGeo, ...] (one per primitive; scanned in order),
    //   tol = 12,                   // screen-space pixel tolerance
    //   onFaceHit = null,           // optional BVH hit for on-face fallback
    //   excludeTypes = null,        // Set/Array of type names to skip
    //                                // (e.g. ['on-edge'] for push/pull drag,
    //                                // where edge-projection-along-axis is noisy)
    // }
    //
    // Returns { type, position, color, label, screen, screenDist, priority,
    //          edge?, edgeT?, face? } or null if no candidate. With `geos`,
    // the returned snap may come from any primitive — priority + screen-dist
    // tiebreak run across the union of all feature sets.
    function findSnap(opts) {
        const tol = opts.tol != null ? opts.tol : 12;
        const cx = opts.cursorX, cy = opts.cursorY;
        const cam = opts.camOpts, w = opts.width, h = opts.height;
        const geos = opts.geos || (opts.geo ? [opts.geo] : []);

        const exclude = opts.excludeTypes
            ? (opts.excludeTypes instanceof Set
                ? opts.excludeTypes
                : new Set(opts.excludeTypes))
            : null;
        function isExcluded(t) { return exclude && exclude.has(t); }

        let best = null;
        function consider(cand) {
            if (!best ||
                cand.priority < best.priority ||
                (cand.priority === best.priority &&
                 cand.screenDist < best.screenDist)) {
                cand.color = COLOR[cand.type];
                cand.label = LABEL[cand.type];
                best = cand;
            }
        }

        const skipEndpoint = isExcluded('endpoint');
        const skipMid      = isExcluded('midpoint');
        const skipEdge     = isExcluded('on-edge');

        for (const geo of geos) {
            // 1) Endpoints.
            if (!skipEndpoint) {
                for (let vi = 0; vi < geo.vertCount; vi++) {
                    const pos = [
                        geo.positions[vi * 3 + 0],
                        geo.positions[vi * 3 + 1],
                        geo.positions[vi * 3 + 2],
                    ];
                    const sp = worldToScreen(pos, cam, w, h);
                    if (sp.behind) continue;
                    const dpx = Math.hypot(sp.x - cx, sp.y - cy);
                    if (dpx > tol) continue;
                    consider({
                        type: 'endpoint',
                        position: pos,
                        screen: { x: sp.x, y: sp.y },
                        screenDist: dpx,
                        priority: PRIORITY.endpoint,
                    });
                }
            }

            // 2) Midpoints + 3) on-edge.
            if (skipMid && skipEdge) continue;
            for (const e of geo.edges) {
                const a = [
                    geo.positions[e.a * 3 + 0],
                    geo.positions[e.a * 3 + 1],
                    geo.positions[e.a * 3 + 2],
                ];
                const b = [
                    geo.positions[e.b * 3 + 0],
                    geo.positions[e.b * 3 + 1],
                    geo.positions[e.b * 3 + 2],
                ];

                if (!skipMid) {
                    const mid = [
                        (a[0] + b[0]) * 0.5,
                        (a[1] + b[1]) * 0.5,
                        (a[2] + b[2]) * 0.5,
                    ];
                    const sm = worldToScreen(mid, cam, w, h);
                    if (!sm.behind) {
                        const dmid = Math.hypot(sm.x - cx, sm.y - cy);
                        if (dmid <= tol) {
                            consider({
                                type: 'midpoint',
                                position: mid,
                                screen: { x: sm.x, y: sm.y },
                                screenDist: dmid,
                                priority: PRIORITY.midpoint,
                                edge: { a, b },
                            });
                        }
                    }
                }

                if (!skipEdge) {
                    const cp = closestPointOnSegmentToRay(a, b, opts.ray);
                    const sp = worldToScreen(cp.point, cam, w, h);
                    if (sp.behind) continue;
                    const dseg = Math.hypot(sp.x - cx, sp.y - cy);
                    if (dseg > tol) continue;
                    consider({
                        type: 'on-edge',
                        position: cp.point,
                        screen: { x: sp.x, y: sp.y },
                        screenDist: dseg,
                        priority: PRIORITY['on-edge'],
                        edge: { a, b },
                        edgeT: cp.t,
                    });
                }
            }
        }

        // 4) On-face fallback (only if nothing higher-priority matched).
        if (!best && opts.onFaceHit && !isExcluded('on-face')) {
            const sp = worldToScreen(opts.onFaceHit.position, cam, w, h);
            if (!sp.behind) {
                consider({
                    type: 'on-face',
                    position: opts.onFaceHit.position.slice(),
                    screen: { x: sp.x, y: sp.y },
                    screenDist: 0,
                    priority: PRIORITY['on-face'],
                    face: opts.onFaceHit,
                });
            }
        }

        return best;
    }

    global.Inference = {
        buildInferenceGeo,
        worldToScreen,
        closestPointOnSegmentToRay,
        findSnap,
        // Exposed for diagnostics / theming.
        _PRIORITY: PRIORITY,
        _COLOR:    COLOR,
        _LABEL:    LABEL,
    };

})(typeof globalThis !== 'undefined' ? globalThis : this);
