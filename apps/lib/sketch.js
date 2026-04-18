// sketch.js — planar math for drawing tools.
//
// Stateless helpers for the click-to-place tool family (line, rectangle,
// circle, polygon). Three recurring motifs:
//
//   1. "User clicked into the viewport — where does that hit the sketch
//      plane?" → rayToPlane
//   2. "We have a 3D polygon on a plane, triangulate it" →
//      planeBasis + project3Dto2D, then hand to Mesh.polygon3D
//   3. "Shift is held — constrain this drag to an axis" → axisLock,
//      pickClosestAxis
//
// Plus a few primitive generators (circlePolyline, rectFromCorners) and
// measurement helpers (polygonArea2D, polylineLength3D) used everywhere
// tools emit preview geometry or show VCB lengths.
//
// No tool state machine here — the existing Move/Rotate/Scale tools have
// very different state shapes and a generic base would obscure more than
// it abstracts. Each new drawing tool stays in the scene-editor app and
// composes from this math.
//
// Usage:
//   <script src="../lib/sketch.js"></script>
//   const hit = Sketch.rayToPlane(ray, planePt, planeNormal);
//   const {u, v} = Sketch.planeBasis(planeNormal);
//   const ccw = Sketch.polygonArea2D(points2d) > 0;
//   const mesh = Mesh.polygon3D(Sketch.flatten3D(ccw ? pts : pts.reverse()),
//                               [], planeNormal);

(function (global) {
    'use strict';

    // --- 3D vector helpers --------------------------------------------------

    function v3add(a, b)   { return [a[0]+b[0], a[1]+b[1], a[2]+b[2]]; }
    function v3sub(a, b)   { return [a[0]-b[0], a[1]-b[1], a[2]-b[2]]; }
    function v3scale(a, s) { return [a[0]*s, a[1]*s, a[2]*s]; }
    function v3dot(a, b)   { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
    function v3cross(a, b) {
        return [
            a[1]*b[2] - a[2]*b[1],
            a[2]*b[0] - a[0]*b[2],
            a[0]*b[1] - a[1]*b[0],
        ];
    }
    function v3len(a) { return Math.sqrt(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]); }
    function v3norm(a) {
        const L = v3len(a);
        return L > 1e-12 ? [a[0]/L, a[1]/L, a[2]/L] : [0, 0, 0];
    }
    function v3dist(a, b) {
        const dx = a[0]-b[0], dy = a[1]-b[1], dz = a[2]-b[2];
        return Math.sqrt(dx*dx + dy*dy + dz*dz);
    }

    // --- Ray ↔ plane intersection ------------------------------------------
    //
    // Returns the 3D point where the ray hits the plane, or null when the
    // ray is parallel or the intersection is behind the origin.
    // `ray`   — { origin: [x,y,z], dir: [x,y,z] } (dir need not be unit).
    // `planePt`, `planeNormal` — any point on the plane + its normal (also
    //                            need not be unit-length, but usually is).

    function rayToPlane(ray, planePt, planeNormal) {
        const denom = v3dot(ray.dir, planeNormal);
        if (Math.abs(denom) < 1e-9) return null;
        const w = v3sub(planePt, ray.origin);
        const t = v3dot(w, planeNormal) / denom;
        if (t < 0) return null;
        return v3add(ray.origin, v3scale(ray.dir, t));
    }

    // --- Plane basis --------------------------------------------------------
    //
    // Given a unit-length normal `n`, produce two orthonormal vectors (u, v)
    // that span the plane. u = normalize(n × ref) where ref is the world
    // axis least parallel to n; v = n × u. Mirrors bromesh's C++ helper so
    // round-tripping through Mesh.polygon3D stays stable.

    function planeBasis(n) {
        const ax = Math.abs(n[0]);
        const ay = Math.abs(n[1]);
        const az = Math.abs(n[2]);
        let ref;
        if (ax <= ay && ax <= az)      ref = [1, 0, 0];
        else if (ay <= az)             ref = [0, 1, 0];
        else                           ref = [0, 0, 1];
        let u = v3cross(n, ref);
        const lu = v3len(u);
        u = lu > 1e-12 ? [u[0]/lu, u[1]/lu, u[2]/lu] : [1, 0, 0];
        const v = v3cross(n, u);
        return { u, v };
    }

    // Like planeBasis, but when `n` is aligned with a world axis picks u/v
    // along the OTHER two world axes (signs chosen so u × v = n). Drawing
    // tools use this so a rectangle on the ground plane aligns to world
    // X/Z instead of planeBasis's arbitrary rotation. Non-axis-aligned
    // normals fall through to planeBasis.
    //
    // World-axis cases (signs pre-derived from u × v = n):
    //   +Y → u=+X, v=-Z      -Y → u=+X, v=+Z
    //   +X → u=+Z, v=-Y      -X → u=+Z, v=+Y
    //   +Z → u=+X, v=+Y      -Z → u=+X, v=-Y
    function worldAxisBasis(n) {
        const ax = Math.abs(n[0]);
        const ay = Math.abs(n[1]);
        const az = Math.abs(n[2]);
        const eps = 1e-4;
        if (ay > 1 - eps) {
            return { u: [1, 0, 0], v: [0, 0, n[1] > 0 ? -1 : 1] };
        }
        if (ax > 1 - eps) {
            return { u: [0, 0, 1], v: [0, n[0] > 0 ? -1 : 1, 0] };
        }
        if (az > 1 - eps) {
            return { u: [1, 0, 0], v: [0, n[2] > 0 ? 1 : -1, 0] };
        }
        return planeBasis(n);
    }

    // --- Project / unproject 3D ↔ plane 2D ---------------------------------
    //
    // 3D point `p` ↔ 2D (a, b) in plane basis (u, v) anchored at `origin`.
    // Paired with Mesh.polygon3D: collect 3D clicks on the sketch plane,
    // project to 2D for triangulation bookkeeping, unproject to place the
    // final 3D face vertices.

    function project3Dto2D(p, origin, u, v) {
        const d = v3sub(p, origin);
        return [v3dot(d, u), v3dot(d, v)];
    }
    function unproject2Dto3D(uv, origin, u, v) {
        return [
            origin[0] + uv[0]*u[0] + uv[1]*v[0],
            origin[1] + uv[0]*u[1] + uv[1]*v[1],
            origin[2] + uv[0]*u[2] + uv[1]*v[2],
        ];
    }

    // --- Axis-lock constraint -----------------------------------------------
    //
    // Project `to` onto the line through `from` along `axis`. Standard
    // shift-to-constrain behavior: once the user commits to an axis, each
    // mouse move slides along that axis rather than freely.

    function axisLock(from, to, axis) {
        const al = v3len(axis);
        if (al < 1e-9) return to.slice();
        const a = [axis[0]/al, axis[1]/al, axis[2]/al];
        const d = v3sub(to, from);
        const t = v3dot(d, a);
        return [from[0] + a[0]*t, from[1] + a[1]*t, from[2] + a[2]*t];
    }

    // Pick the axis from `axes` whose direction best aligns with (to-from).
    // Returns `{axis, index, alignment}` or null when the drag is shorter
    // than `minLen`. `alignment` = |dot(drag, axis)| ∈ [0,1]. Useful for
    // SketchUp-style "red/green/blue axis inference" during a drag.

    function pickClosestAxis(from, to, axes, minLen) {
        if (minLen == null) minLen = 1e-6;
        const d = v3sub(to, from);
        const dl = v3len(d);
        if (dl < minLen) return null;
        const dn = [d[0]/dl, d[1]/dl, d[2]/dl];
        let bestIdx = -1, bestAlign = -1;
        for (let i = 0; i < axes.length; i++) {
            const a = v3norm(axes[i]);
            const align = Math.abs(v3dot(dn, a));
            if (align > bestAlign) { bestAlign = align; bestIdx = i; }
        }
        if (bestIdx < 0) return null;
        return { axis: axes[bestIdx], index: bestIdx, alignment: bestAlign };
    }

    // --- Rectangle from two opposite corners --------------------------------
    //
    // Given corners p0 and p2 on a plane with basis (u, v), produce the
    // 4-corner rectangle axis-aligned in (u, v). Returned CCW as seen from
    // +normal (where normal = u × v) — so Mesh.polygon3D emits a front-
    // facing quad toward the camera.
    //
    //     p0 ──── p1
    //     │       │       p0, p2 are the input corners;
    //     p3 ──── p2       p1, p3 are constructed.

    function rectFromCorners(p0, p2, u, v) {
        const uv2 = project3Dto2D(p2, p0, u, v);
        const p1  = unproject2Dto3D([uv2[0], 0], p0, u, v);
        const p3  = unproject2Dto3D([0, uv2[1]], p0, u, v);
        // Wind CCW in (u, v): if the (u, v) signed area is negative,
        // swap p1 ↔ p3 to flip orientation.
        const area = uv2[0] * uv2[1];   // (u_b - u_a) * (v_b - v_a), both a=0
        if (area >= 0) return [p0, p1, p2, p3];
        return [p0, p3, p2, p1];
    }

    // --- Circle polyline ----------------------------------------------------
    //
    // `segments` equidistant 3D points on the plane through `center` with
    // the given `normal`. Wound CCW as seen from +normal. The first point
    // sits along the plane's u-axis so successive circles share a start
    // direction (useful for polygon-tool "flat side on top" preview).

    function circlePolyline(center, radius, normal, segments) {
        if (segments == null) segments = 32;
        const n = v3norm(normal);
        const { u, v } = planeBasis(n);
        const out = new Array(segments);
        const twoPi = Math.PI * 2;
        for (let i = 0; i < segments; i++) {
            const ang = twoPi * (i / segments);
            const ca = Math.cos(ang) * radius;
            const sa = Math.sin(ang) * radius;
            out[i] = [
                center[0] + u[0]*ca + v[0]*sa,
                center[1] + u[1]*ca + v[1]*sa,
                center[2] + u[2]*ca + v[2]*sa,
            ];
        }
        return out;
    }

    // --- Arc polyline (2-point + bulge) -------------------------------------
    //
    // SketchUp's classic Arc tool: two endpoints + a bulge offset that
    // determines the arc's radius and sweep. `bulgePoint` is any point near
    // the arc (the 3rd click); only its perpendicular distance from the chord
    // matters. Returns `segments + 1` points sampled along the arc, ordered
    // start → end. Returns null when degenerate (zero chord, zero bulge).
    //
    //   start, end   — chord endpoints (3D, on the sketch plane)
    //   bulgePoint   — any 3D point; signed perpendicular distance from the
    //                  chord midpoint defines the arc height
    //   normal       — sketch-plane normal (unit)
    //   segments     — number of straight sub-segments (default 16)
    //
    // Geometry: with chord length L and bulge height h,
    //   r = (h² + (L/2)²) / (2|h|)        (signed: bulge direction preserved)
    //   sweep = 2·asin( (L/2) / r )
    //   center = midpoint − sign(h) · (r − |h|) · chordPerp
    function arcPolyline(start, end, bulgePoint, normal, segments) {
        if (segments == null) segments = 16;
        const cx = end[0] - start[0];
        const cy = end[1] - start[1];
        const cz = end[2] - start[2];
        const L  = Math.sqrt(cx*cx + cy*cy + cz*cz);
        if (L < 1e-9) return null;
        const chord = [cx / L, cy / L, cz / L];
        const n = v3norm(normal);
        // Raw perpendicular to chord, in the sketch plane (normal × chord).
        let perp = v3cross(n, chord);
        if (v3len(perp) < 1e-9) return null;
        perp = v3norm(perp);
        const mid = [
            (start[0] + end[0]) * 0.5,
            (start[1] + end[1]) * 0.5,
            (start[2] + end[2]) * 0.5,
        ];
        // Signed bulge distance from mid along raw perp.
        const dx = bulgePoint[0] - mid[0];
        const dy = bulgePoint[1] - mid[1];
        const dz = bulgePoint[2] - mid[2];
        let h = dx * perp[0] + dy * perp[1] + dz * perp[2];
        if (Math.abs(h) < 1e-9) {
            // No bulge: return the straight chord as 2 points.
            return [start.slice(), end.slice()];
        }
        // Flip perp so it points TOWARD the bulge — h becomes positive and the
        // rest of the math doesn't have to track the sign.
        if (h < 0) {
            perp = [-perp[0], -perp[1], -perp[2]];
            h = -h;
        }
        const r = (h * h + (L * 0.5) * (L * 0.5)) / (2 * h);
        // Minor arc: center on the OPPOSITE side of the chord from the bulge
        // (cdist > 0). Major arc (h > L/2): center crosses to the bulge side
        // (cdist < 0) so the chord subtends >π at the centre.
        const cdist = r - h;
        const center = [
            mid[0] - cdist * perp[0],
            mid[1] - cdist * perp[1],
            mid[2] - cdist * perp[2],
        ];
        // Basis at the centre: radial-to-start + tangent. The raw tangent
        // (n × radial) is one of two perpendicular candidates; pick the one
        // with positive chord-direction component so dir=+1 always advances
        // start → end on the minor arc, and dir=-1 on the major arc.
        const rx = (start[0] - center[0]) / r;
        const ry = (start[1] - center[1]) / r;
        const rz = (start[2] - center[2]) / r;
        let tan = v3cross(n, [rx, ry, rz]);
        const tanDotChord = tan[0]*chord[0] + tan[1]*chord[1] + tan[2]*chord[2];
        if (tanDotChord < 0) tan = [-tan[0], -tan[1], -tan[2]];
        // Minor-arc sweep: 2·asin((L/2)/r). Major arc adds the rest of the
        // circle, and reverses the initial tangent direction so the sweep
        // wraps the long way around.
        const halfChord = Math.min(L * 0.5 / r, 1);
        const minor = 2 * Math.asin(halfChord);
        const major = h > L * 0.5;
        const total = major ? (2 * Math.PI - minor) : minor;
        const dir   = major ? -1 : 1;
        const out = new Array(segments + 1);
        for (let i = 0; i <= segments; i++) {
            const a = (total * i) / segments * dir;
            const ca = Math.cos(a);
            const sa = Math.sin(a);
            out[i] = [
                center[0] + r * (ca * rx + sa * tan[0]),
                center[1] + r * (ca * ry + sa * tan[1]),
                center[2] + r * (ca * rz + sa * tan[2]),
            ];
        }
        return out;
    }

    // --- Sweep profile along path (Follow-Me) -------------------------------
    //
    // Sweep a planar polygon `profile3D` along `pathPts`, producing a
    // triangulated tube. At each path vertex the profile is oriented
    // perpendicular to the path: the bisector of incoming and outgoing
    // segments at interior vertices, the segment direction itself at the
    // endpoints. Rotation is propagated via min-rotation between successive
    // ring directions to avoid twist.
    //
    //   profile3D     — closed CCW polygon (viewed from +profileNormal),
    //                   array of [x,y,z]; need not be centered.
    //   profileNormal — unit normal of the profile plane.
    //   pathPts       — array of [x,y,z], length >= 2.
    //
    // Returns { positions: Float32Array, indices: Uint32Array,
    //          normals: Float32Array } or null when the inputs are
    // degenerate. Cap faces are NOT emitted — caller can triangulate
    // profile separately if needed.
    function sweepProfile(profile3D, profileNormal, pathPts) {
        const N = profile3D.length;
        const K = pathPts.length;
        if (N < 3 || K < 2) return null;
        const pn = v3norm(profileNormal);
        // Centre the profile so rotation is around its centroid.
        let cx = 0, cy = 0, cz = 0;
        for (const p of profile3D) { cx += p[0]; cy += p[1]; cz += p[2]; }
        cx /= N; cy /= N; cz /= N;
        const centered = profile3D.map(p => [p[0]-cx, p[1]-cy, p[2]-cz]);
        // Initial alignment: profile normal → first-segment direction.
        const fwd0 = v3norm(v3sub(pathPts[1], pathPts[0]));
        if (v3len(fwd0) < 1e-9) return null;
        let oriented = _rotateAll(centered, _rotBetween(pn, fwd0));
        let prevDir = fwd0;
        // First ring at pathPts[0].
        const rings = [];
        rings.push(oriented.map(p => [
            p[0] + pathPts[0][0], p[1] + pathPts[0][1], p[2] + pathPts[0][2]]));
        // Subsequent rings.
        for (let i = 1; i < K; i++) {
            const seg = v3norm(v3sub(pathPts[i], pathPts[i-1]));
            if (v3len(seg) < 1e-9) return null;
            // Interior: bisector of incoming + outgoing. Endpoint: segment
            // itself. Bisector for path corners gives a clean miter quad.
            let ringDir = seg;
            if (i < K - 1) {
                const next = v3norm(v3sub(pathPts[i+1], pathPts[i]));
                const bx = seg[0] + next[0];
                const by = seg[1] + next[1];
                const bz = seg[2] + next[2];
                const bl = Math.sqrt(bx*bx + by*by + bz*bz);
                if (bl > 1e-9) ringDir = [bx/bl, by/bl, bz/bl];
            }
            const Rstep = _rotBetween(prevDir, ringDir);
            oriented = _rotateAll(oriented, Rstep);
            rings.push(oriented.map(p => [
                p[0] + pathPts[i][0],
                p[1] + pathPts[i][1],
                p[2] + pathPts[i][2]]));
            prevDir = ringDir;
        }
        // Stitch quads between consecutive rings → 2 tris per quad.
        const positions = new Float32Array(K * N * 3);
        for (let i = 0; i < K; i++) {
            for (let j = 0; j < N; j++) {
                const k = (i * N + j) * 3;
                positions[k]     = rings[i][j][0];
                positions[k + 1] = rings[i][j][1];
                positions[k + 2] = rings[i][j][2];
            }
        }
        const indices = new Uint32Array((K - 1) * N * 6);
        let w = 0;
        for (let i = 0; i < K - 1; i++) {
            for (let j = 0; j < N; j++) {
                const jn = (j + 1) % N;
                const a = i * N + j;
                const b = i * N + jn;
                const c = (i + 1) * N + j;
                const d = (i + 1) * N + jn;
                // Wind so cross((b-a), (c-a)) faces OUT of the tube. For a CCW
                // profile (viewed from +pn) and forward = pn-aligned at start,
                // the order (a, c, d) + (a, d, b) gives outward.
                indices[w++] = a; indices[w++] = c; indices[w++] = d;
                indices[w++] = a; indices[w++] = d; indices[w++] = b;
            }
        }
        // Per-vertex face normals (averaged over the two adjacent faces in
        // the same ring slot — good enough for shading without per-tri
        // duplication).
        const normals = _computeAveragedNormals(positions, indices);
        return { positions, indices, normals };
    }

    // Rotation matrix (3x3 row-major) that takes unit vector `a` to unit `b`.
    // Special-cases the parallel and antiparallel limits so Rodrigues never
    // divides by zero. Antiparallel uses an arbitrary perpendicular axis.
    function _rotBetween(a, b) {
        const dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
        if (dot > 1 - 1e-9) {
            return [1,0,0, 0,1,0, 0,0,1];
        }
        if (dot < -1 + 1e-9) {
            // 180°: pick any axis perpendicular to a.
            const ax = Math.abs(a[0]) < 0.9 ? [1,0,0] : [0,1,0];
            const k = v3norm(v3cross(a, ax));
            return _rodrigues(k, Math.PI);
        }
        const k = v3norm(v3cross(a, b));
        const angle = Math.acos(Math.max(-1, Math.min(1, dot)));
        return _rodrigues(k, angle);
    }

    function _rodrigues(k, angle) {
        const c = Math.cos(angle), s = Math.sin(angle), C = 1 - c;
        const x = k[0], y = k[1], z = k[2];
        return [
            c + x*x*C,     x*y*C - z*s,   x*z*C + y*s,
            y*x*C + z*s,   c + y*y*C,     y*z*C - x*s,
            z*x*C - y*s,   z*y*C + x*s,   c + z*z*C,
        ];
    }

    function _rotateAll(pts, R) {
        return pts.map(p => [
            R[0]*p[0] + R[1]*p[1] + R[2]*p[2],
            R[3]*p[0] + R[4]*p[1] + R[5]*p[2],
            R[6]*p[0] + R[7]*p[1] + R[8]*p[2],
        ]);
    }

    function _computeAveragedNormals(positions, indices) {
        const out = new Float32Array(positions.length);
        for (let t = 0; t < indices.length; t += 3) {
            const i0 = indices[t]*3, i1 = indices[t+1]*3, i2 = indices[t+2]*3;
            const ax = positions[i1]   - positions[i0];
            const ay = positions[i1+1] - positions[i0+1];
            const az = positions[i1+2] - positions[i0+2];
            const bx = positions[i2]   - positions[i0];
            const by = positions[i2+1] - positions[i0+1];
            const bz = positions[i2+2] - positions[i0+2];
            const nx = ay*bz - az*by;
            const ny = az*bx - ax*bz;
            const nz = ax*by - ay*bx;
            out[i0]+=nx; out[i0+1]+=ny; out[i0+2]+=nz;
            out[i1]+=nx; out[i1+1]+=ny; out[i1+2]+=nz;
            out[i2]+=nx; out[i2+1]+=ny; out[i2+2]+=nz;
        }
        for (let i = 0; i < out.length; i += 3) {
            const L = Math.hypot(out[i], out[i+1], out[i+2]);
            if (L > 1e-12) { out[i] /= L; out[i+1] /= L; out[i+2] /= L; }
        }
        return out;
    }

    // --- Polygon offset (2D, per-edge parallel) -----------------------------
    //
    // Inset/expand a simple closed polygon by `distance`. Sign convention for
    // a CCW polygon: positive = outward (expand), negative = inward (inset).
    // Sign reverses for a CW input. Returns null when the offset collapses
    // the polygon (a typical inset failure mode).
    //
    // Algorithm: translate each edge along its outward unit normal by
    // `distance`, then intersect each pair of adjacent translated edges to
    // get the new vertex. Reasonable for convex polygons and simple concave
    // shapes; pathological self-intersections from large insets are not
    // detected (caller should sanity-check with polygonArea2D).
    //
    //   loop2D — array of [x, y]; CCW for positive=outward.
    function offsetPolygon2D(loop2D, distance) {
        const n = loop2D.length;
        if (n < 3 || distance === 0) return loop2D.map(p => p.slice());
        // Detect winding so the sign convention works regardless of input.
        const ccw = polygonArea2D(loop2D) >= 0;
        const sgn = ccw ? 1 : -1;
        // Per-edge unit normal (pointing OUT of the polygon for CCW input).
        const normals = new Array(n);
        for (let i = 0; i < n; i++) {
            const a = loop2D[i];
            const b = loop2D[(i + 1) % n];
            const ex = b[0] - a[0];
            const ey = b[1] - a[1];
            const L = Math.hypot(ex, ey);
            if (L < 1e-12) return null;
            // Right-hand 90° rotation gives the outward normal for CCW.
            normals[i] = [sgn * ey / L, -sgn * ex / L];
        }
        // For each polygon vertex i, intersect translated edge (i-1) with
        // translated edge (i). The translated edges are parallel to the
        // originals; closed-form intersection avoids parameter-clipping
        // issues at sharp angles.
        const out = new Array(n);
        for (let i = 0; i < n; i++) {
            const ip = (i + n - 1) % n;
            const a0 = loop2D[ip];
            const b0 = loop2D[i];
            const a1 = loop2D[i];
            const b1 = loop2D[(i + 1) % n];
            const np = normals[ip];
            const nn = normals[i];
            // Translated endpoints.
            const A0 = [a0[0] + distance * np[0], a0[1] + distance * np[1]];
            const B0 = [b0[0] + distance * np[0], b0[1] + distance * np[1]];
            const A1 = [a1[0] + distance * nn[0], a1[1] + distance * nn[1]];
            const B1 = [b1[0] + distance * nn[0], b1[1] + distance * nn[1]];
            const ix = lineIntersect(A0, B0, A1, B1);
            if (ix) {
                out[i] = ix;
                continue;
            }
            // Parallel translated edges (collinear neighbours): the new
            // vertex sits at the shared endpoint translated along its normal.
            out[i] = [b0[0] + distance * np[0], b0[1] + distance * np[1]];
        }
        // Collapse check. An inset that crosses the medial axis flips one or
        // more edges relative to the original loop — even though the overall
        // signed area stays positive (the polygon just becomes a smaller,
        // re-CCW'd copy of itself). Detect by per-edge direction sign vs the
        // original edge.
        for (let i = 0; i < n; i++) {
            const a0 = loop2D[i], b0 = loop2D[(i + 1) % n];
            const a1 = out[i],    b1 = out[(i + 1) % n];
            const ox = b0[0] - a0[0], oy = b0[1] - a0[1];
            const nx = b1[0] - a1[0], ny = b1[1] - a1[1];
            if (ox * nx + oy * ny < 0) return null;
        }
        const newArea = polygonArea2D(out);
        if (sgn * newArea <= 1e-12) return null;
        return out;
    }

    // Infinite-line intersection. Returns [x, y] or null when parallel.
    function lineIntersect(p0, p1, p2, p3) {
        const r0 = p1[0] - p0[0], r1 = p1[1] - p0[1];
        const s0 = p3[0] - p2[0], s1 = p3[1] - p2[1];
        const rxs = r0 * s1 - r1 * s0;
        if (Math.abs(rxs) < 1e-12) return null;
        const qp0 = p2[0] - p0[0], qp1 = p2[1] - p0[1];
        const t = (qp0 * s1 - qp1 * s0) / rxs;
        return [p0[0] + t * r0, p0[1] + t * r1];
    }

    // --- Polygon area (2D, shoelace) ----------------------------------------
    //
    // Accepts either an array of [x, y] pairs or a flat [x,y,x,y,...]
    // array. Returns signed area: positive = CCW, negative = CW. Useful for
    // determining if the user drew a polygon that front-faces the plane's
    // +normal (→ CCW) or should be flipped.

    function polygonArea2D(points) {
        if (!points || points.length === 0) return 0;
        const flat = typeof points[0] === 'number';
        const n = flat ? (points.length / 2) : points.length;
        if (n < 3) return 0;
        let sum = 0;
        const xi = flat ? (i => points[i*2])     : (i => points[i][0]);
        const yi = flat ? (i => points[i*2 + 1]) : (i => points[i][1]);
        for (let i = 0; i < n; i++) {
            const j = (i + 1) % n;
            sum += xi(i) * yi(j) - xi(j) * yi(i);
        }
        return 0.5 * sum;
    }

    // --- Polyline length (3D) ------------------------------------------------
    //
    // Sum of segment lengths in a 3D polyline. `closed=true` adds the
    // closing segment back to the first point.

    function polylineLength3D(points, closed) {
        if (!points || points.length < 2) return 0;
        let L = 0;
        for (let i = 1; i < points.length; i++) L += v3dist(points[i-1], points[i]);
        if (closed) L += v3dist(points[points.length-1], points[0]);
        return L;
    }

    // --- Planar self-intersection split -------------------------------------
    //
    // Takes a closed 2D polyline as an array of [x, y] points and returns an
    // array of simple (non-self-intersecting) sub-polygon loops that cover
    // the same area. Each loop is CCW and ≥3 vertices.
    //
    // For a self-intersecting input (figure-8, bowtie), we compute all
    // pairwise segment intersections, insert them as nodes in a planar
    // graph, and extract every bounded face using the standard half-edge
    // face-walk: at each node, outgoing edges are angle-sorted; an incoming
    // edge's "next-in-face" is the immediately CW-preceding outgoing
    // neighbour of its reverse.
    //
    // For a simple input, the output is `[points]` (one loop, same verts).
    // Degenerate colinear edges that generate collinear "intersections" are
    // handled by quantizing node positions.
    //
    // This lets Line-drawn polygons containing crossings split into the
    // natural set of faces a user drew, without surfacing an error.
    function splitSelfIntersectingPolygon(points) {
        const n = points.length;
        if (n < 3) return [];
        // Fast path: simple polygon — no self-intersections.
        if (!hasSelfIntersections(points)) {
            return [polygonArea2D(points) >= 0 ? points.slice() :
                    points.slice().reverse()];
        }
        // Per-segment record with ordered t-values of intersections along it.
        const segs = [];
        for (let i = 0; i < n; i++) {
            segs.push({
                a: points[i],
                b: points[(i + 1) % n],
                splits: [],
            });
        }
        for (let i = 0; i < n; i++) {
            for (let j = i + 1; j < n; j++) {
                if (segsAdjacent(i, j, n)) continue;
                const ix = segIntersect(
                    segs[i].a, segs[i].b, segs[j].a, segs[j].b);
                if (ix) {
                    segs[i].splits.push({ t: ix.ti, pt: ix.pt });
                    segs[j].splits.push({ t: ix.tj, pt: ix.pt });
                }
            }
        }
        // Canonical nodes by quantized position.
        const nodes = [];
        const nodeByKey = new Map();
        const Q = 1e5;
        function nodeOf(pt) {
            const k = Math.round(pt[0] * Q) + ',' + Math.round(pt[1] * Q);
            let id = nodeByKey.get(k);
            if (id == null) {
                id = nodes.length;
                nodes.push({ x: pt[0], y: pt[1], out: [] });
                nodeByKey.set(k, id);
            }
            return id;
        }
        // Build directed half-edges by walking each segment through its
        // in-order splits.
        const halves = [];
        function addHalves(fromId, toId) {
            if (fromId === toId) return;
            const e1 = { from: fromId, to: toId, twin: null, next: null, seen: false };
            const e2 = { from: toId,  to: fromId, twin: null, next: null, seen: false };
            e1.twin = e2; e2.twin = e1;
            halves.push(e1, e2);
            nodes[fromId].out.push(e1);
            nodes[toId].out.push(e2);
        }
        for (let i = 0; i < n; i++) {
            const seg = segs[i];
            seg.splits.sort((a, b) => a.t - b.t);
            let prev = nodeOf(seg.a);
            for (const s of seg.splits) {
                const cur = nodeOf(s.pt);
                addHalves(prev, cur);
                prev = cur;
            }
            addHalves(prev, nodeOf(seg.b));
        }
        // Dedup parallel half-edges (tangential crossings, colinear joins):
        // keep one per (from, to) pair.
        for (const node of nodes) {
            const uniq = new Map();
            for (const e of node.out) {
                const k = e.from + '>' + e.to;
                if (!uniq.has(k)) uniq.set(k, e);
            }
            node.out = Array.from(uniq.values());
        }
        // Angular sort of outgoing edges at each node.
        for (const node of nodes) {
            node.out.sort((a, b) => {
                const aa = Math.atan2(nodes[a.to].y - node.y,
                                      nodes[a.to].x - node.x);
                const bb = Math.atan2(nodes[b.to].y - node.y,
                                      nodes[b.to].x - node.x);
                return aa - bb;
            });
        }
        // Face-walk: at `to`, the next-in-face edge is the CW-preceding
        // outgoing neighbour of the reverse (to→from) edge.
        for (const e of halves) {
            const to = nodes[e.to];
            const idx = to.out.findIndex(x => x.to === e.from);
            if (idx < 0) continue;
            const prev = (idx - 1 + to.out.length) % to.out.length;
            e.next = to.out[prev];
        }
        // Collect face cycles.
        const loops = [];
        for (const start of halves) {
            if (start.seen) continue;
            const loop = [];
            let cur = start;
            for (let guard = 0; guard < halves.length + 1; guard++) {
                if (!cur || cur.seen) break;
                cur.seen = true;
                loop.push([nodes[cur.from].x, nodes[cur.from].y]);
                cur = cur.next;
                if (cur === start) break;
            }
            if (loop.length >= 3) loops.push(loop);
        }
        // Keep only bounded (CCW) loops with a meaningful area. The
        // unbounded "outside" loop comes out CW.
        const out = [];
        for (const loop of loops) {
            const a = polygonArea2D(loop);
            if (a > 1e-10) out.push(loop);
        }
        return out;
    }

    // Returns { ti, tj, pt } if the open segments (a→b) and (c→d) cross
    // strictly in their interiors (t values in (ε, 1-ε)). Endpoint-touches
    // are ignored — those are legitimate polygon corners, not crossings.
    function segIntersect(a, b, c, d) {
        const eps = 1e-9;
        const r0 = b[0] - a[0], r1 = b[1] - a[1];
        const s0 = d[0] - c[0], s1 = d[1] - c[1];
        const rxs = r0 * s1 - r1 * s0;
        if (Math.abs(rxs) < eps) return null;   // parallel or colinear
        const qp0 = c[0] - a[0], qp1 = c[1] - a[1];
        const ti = (qp0 * s1 - qp1 * s0) / rxs;
        const tj = (qp0 * r1 - qp1 * r0) / rxs;
        if (ti < eps || ti > 1 - eps) return null;
        if (tj < eps || tj > 1 - eps) return null;
        return { ti, tj, pt: [a[0] + ti * r0, a[1] + ti * r1] };
    }

    function segsAdjacent(i, j, n) {
        return Math.abs(i - j) <= 1 || (i === 0 && j === n - 1) ||
               (j === 0 && i === n - 1);
    }

    function hasSelfIntersections(points) {
        const n = points.length;
        for (let i = 0; i < n; i++) {
            for (let j = i + 1; j < n; j++) {
                if (segsAdjacent(i, j, n)) continue;
                const ix = segIntersect(
                    points[i], points[(i + 1) % n],
                    points[j], points[(j + 1) % n]);
                if (ix) return true;
            }
        }
        return false;
    }

    // --- Flatten an array of 3D points to a flat array ----------------------
    //
    // Pairs naturally with Mesh.polygon3D which wants a flat [x,y,z,...].

    function flatten3D(points) {
        const out = new Float32Array(points.length * 3);
        for (let i = 0; i < points.length; i++) {
            out[i*3]     = points[i][0];
            out[i*3 + 1] = points[i][1];
            out[i*3 + 2] = points[i][2];
        }
        return out;
    }

    global.Sketch = {
        // 3D math
        v3add, v3sub, v3scale, v3dot, v3cross, v3len, v3norm, v3dist,
        // plane / projection
        rayToPlane, planeBasis, worldAxisBasis, project3Dto2D, unproject2Dto3D,
        // constraints
        axisLock, pickClosestAxis,
        // shapes
        rectFromCorners, circlePolyline, arcPolyline,
        // 2D polygon ops
        offsetPolygon2D,
        // 3D sweep
        sweepProfile,
        // measurement
        polygonArea2D, polylineLength3D,
        // polygon cleanup
        splitSelfIntersectingPolygon, hasSelfIntersections,
        // helpers
        flatten3D,
    };
})(typeof window !== 'undefined' ? window : globalThis);
