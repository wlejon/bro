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
        rayToPlane, planeBasis, project3Dto2D, unproject2Dto3D,
        // constraints
        axisLock, pickClosestAxis,
        // shapes
        rectFromCorners, circlePolyline,
        // measurement
        polygonArea2D, polylineLength3D,
        // helpers
        flatten3D,
    };
})(typeof window !== 'undefined' ? window : globalThis);
