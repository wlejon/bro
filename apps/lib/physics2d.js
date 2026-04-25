// physics2d.js — Thin 2D wrapper over the 3D Jolt Physics API
//
// Coordinate convention:
//   Canvas: X-right, Y-down (origin top-left)
//   Physics: X-right, Y-up  (origin bottom-left of canvas)
//
// The wrapper converts between them automatically. All positions passed
// to and returned from Physics2D are in canvas coordinates (Y-down).
// Angles are in radians, clockwise-positive (matching canvas rotation).
//
// 2D bodies use Jolt's native 2D DOF lock (Plane2D) — they translate only
// in X/Y and rotate only around Z. No per-frame Z clamp needed.
//
// Usage:
//   <script src="../lib/physics2d.js"></script>
//   Physics2D.init({ gravity: 980, width: 800, height: 600 });
//   var ball = Physics2D.createCircle(400, 300, 10, { restitution: 0.9 });
//   // in game loop:
//   var pos = Physics2D.getPosition(ball);  // {x, y} canvas coords
//   var angle = Physics2D.getAngle(ball);   // radians, CW positive

var Physics2D = (function() {
    "use strict";

    var bodies = {};    // tag → { tag, type, width, height, radius }
    var canvasH = 600;  // canvas height for Y-flip

    // --- coordinate helpers ---

    function toPhysX(cx) { return cx; }
    function toPhysY(cy) { return canvasH - cy; }
    function toCanvasX(px) { return px; }
    function toCanvasY(py) { return canvasH - py; }

    // Extract Z-axis rotation from quaternion (radians, CW positive for canvas)
    function quatToAngle(r) {
        return -Math.atan2(2 * (r.w * r.z + r.x * r.y),
                           1 - 2 * (r.y * r.y + r.z * r.z));
    }

    // --- public API ---

    function init(opts) {
        opts = opts || {};
        canvasH = opts.height || 600;
        var gravity = opts.gravity !== undefined ? opts.gravity : 980; // pixels/s²
        Physics.createWorld(opts.maxBodies ? { maxBodies: opts.maxBodies } : undefined);
        Physics.setGravity(0, -gravity, 0);
        bodies = {};
    }

    function commonOpts(opts) {
        opts = opts || {};
        var o = {
            static: !!opts.static,
            sensor: !!opts.sensor,
            ccd: !!opts.ccd,
            friction: opts.friction !== undefined ? opts.friction : 0.5,
            restitution: opts.restitution !== undefined ? opts.restitution : 0.3,
            dofs: '2d',
        };
        if (opts.layer) o.layer = opts.layer;
        if (opts.userData !== undefined) o.userData = opts.userData;
        if (opts.gravityFactor !== undefined) o.gravityFactor = opts.gravityFactor;
        return o;
    }

    function createBox(x, y, w, h, opts) {
        var bo = commonOpts(opts);
        bo.shape = "box";
        bo.position = { x: toPhysX(x), y: toPhysY(y), z: 0 };
        bo.halfExtents = { x: w / 2, y: h / 2, z: 10 };
        var tag = Physics.createBody(bo);
        bodies[tag] = { tag: tag, type: "box", width: w, height: h };
        return tag;
    }

    function createCircle(x, y, radius, opts) {
        var bo = commonOpts(opts);
        bo.shape = "sphere";
        bo.position = { x: toPhysX(x), y: toPhysY(y), z: 0 };
        bo.radius = radius;
        var tag = Physics.createBody(bo);
        bodies[tag] = { tag: tag, type: "circle", radius: radius };
        return tag;
    }

    // Create a static collider from a 2D polyline. Each segment becomes a
    // thin static capsule rotated to align with the segment, which is two-
    // sided collision out of the box (no mesh winding tricks). Returns an
    // ARRAY of body tags — one per segment — so callers can destroy them
    // together. (Earlier versions returned a single mesh tag with double-
    // wound triangles; that bloated the BVH and assumed a winding choice.)
    //
    // The wrapper records the segment list under tag[0] for backward-compat
    // calls like getBody(tag) — destroyBody(tag) accepts the array directly.
    function createPolyline(points, opts) {
        opts = opts || {};
        var thickness = opts.thickness !== undefined ? opts.thickness : 4;
        var halfT = thickness / 2;
        var n = points.length;
        if (n < 2) return [];
        var tags = [];
        var fric = opts.friction !== undefined ? opts.friction : 0.5;
        var rest = opts.restitution !== undefined ? opts.restitution : 0.0;
        for (var i = 0; i + 1 < n; i++) {
            var a = points[i], b = points[i + 1];
            var ax = toPhysX(a.x), ay = toPhysY(a.y);
            var bx = toPhysX(b.x), by = toPhysY(b.y);
            var dx = bx - ax, dy = by - ay;
            var len = Math.sqrt(dx * dx + dy * dy);
            if (len < 1e-3) continue;
            // Capsule's local long axis is +Y. We want to align that with the
            // segment direction (dx, dy). Compute Z-axis quaternion that
            // rotates +Y onto (dx/len, dy/len).
            var ang = Math.atan2(dy, dx) - Math.PI / 2;
            var qz = Math.sin(ang / 2);
            var qw = Math.cos(ang / 2);
            // halfHeight = half the cylinder portion (excludes hemisphere caps).
            // Use halfT as the radius and (len/2 - halfT) as halfHeight; clamp
            // to >0 to avoid zero-height capsules on tiny segments.
            var halfH = Math.max(0.001, len * 0.5 - halfT);
            var tag = Physics.createBody({
                shape: 'capsule',
                static: true,
                halfHeight: halfH,
                radius: halfT,
                position: { x: (ax + bx) * 0.5, y: (ay + by) * 0.5, z: 0 },
                rotation: { x: 0, y: 0, z: qz, w: qw },
                friction: fric,
                restitution: rest,
                layer: opts.layer,
            });
            tags.push(tag);
            bodies[tag] = { tag: tag, type: 'polyline-segment' };
        }
        if (tags.length === 0) return [];
        // Stash full segment list on the first tag's record for getBody() compat.
        if (bodies[tags[0]]) bodies[tags[0]].points = points;
        return tags;
    }

    function createSensor(x, y, w, h, opts) {
        opts = Object.assign({}, opts || {}, { sensor: true, static: true });
        return createBox(x, y, w, h, opts);
    }

    // --- Constraints ---
    //
    // A 2D distance constraint binds two bodies at the given pair of canvas
    // points; min/max in canvas pixels.
    function createDistanceConstraint(tagA, tagB, ax, ay, bx, by, opts) {
        opts = opts || {};
        return Physics.createConstraint({
            type: 'distance',
            body1: tagA, body2: tagB,
            point1: { x: toPhysX(ax), y: toPhysY(ay), z: 0 },
            point2: { x: toPhysX(bx), y: toPhysY(by), z: 0 },
            minDistance: opts.minDistance !== undefined ? opts.minDistance : -1,
            maxDistance: opts.maxDistance !== undefined ? opts.maxDistance : -1,
        });
    }

    function createPinConstraint(tagA, tagB, x, y) {
        return Physics.createConstraint({
            type: 'point',
            body1: tagA, body2: tagB,
            point1: { x: toPhysX(x), y: toPhysY(y), z: 0 },
            point2: { x: toPhysX(x), y: toPhysY(y), z: 0 },
        });
    }

    function createHingeConstraint(tagA, tagB, x, y) {
        return Physics.createConstraint({
            type: 'hinge',
            body1: tagA, body2: tagB,
            point1: { x: toPhysX(x), y: toPhysY(y), z: 0 },
            point2: { x: toPhysX(x), y: toPhysY(y), z: 0 },
            axis: { x: 0, y: 0, z: 1 },  // 2D hinges spin around Z
        });
    }

    function destroyConstraint(handle) {
        Physics.destroyConstraint(handle);
    }

    function setLayer(tag, name) {
        // Backed by Physics.setLayer → Jolt BodyInterface::SetObjectLayer
        // (broadphase notification handled by Jolt internally; cost ~ remove+
        // add of one body in the broadphase, cheap).
        Physics.setLayer(tag, name);
    }

    // --- Kinematic bodies (driven by velocity, not affected by gravity) ---
    //
    // Use createKinematic for bars/platforms that move under script control
    // and need to push dynamic bodies on contact. setKinematicTarget
    // computes the velocity that reaches (x,y) from the current position in
    // dt seconds — Jolt integrates that for one step, giving stable contact
    // forces against dynamic bodies (vs. teleporting with setPosition,
    // which produces unphysical impulses).
    function createKinematic(opts) {
        opts = opts || {};
        var bo = commonOpts(opts);
        var tag;
        if (opts.shape === 'circle') {
            bo.shape = 'sphere';
            bo.position = { x: toPhysX(opts.x), y: toPhysY(opts.y), z: 0 };
            bo.radius = opts.radius;
            tag = Physics.createBody(bo);
            bodies[tag] = { tag: tag, type: 'kinematic-circle', radius: opts.radius };
        } else {
            // box default
            bo.shape = 'box';
            bo.position = { x: toPhysX(opts.x), y: toPhysY(opts.y), z: 0 };
            bo.halfExtents = { x: opts.w / 2, y: opts.h / 2, z: 10 };
            tag = Physics.createBody(bo);
            bodies[tag] = { tag: tag, type: 'kinematic-box', width: opts.w, height: opts.h };
        }
        Physics.setKinematic(tag);
        return tag;
    }

    function setKinematicTarget(tag, x, y, dt) {
        if (dt === undefined || dt <= 0) {
            Physics.setPosition(tag, toPhysX(x), toPhysY(y), 0);
            return;
        }
        Physics.moveKinematic(tag, toPhysX(x), toPhysY(y), 0, dt);
    }

    // --- Standard accessors ---

    function destroyBody(tag) {
        // Accept arrays (e.g. createPolyline returns one) for ergonomic cleanup.
        if (Array.isArray(tag)) {
            for (var i = 0; i < tag.length; i++) {
                Physics.destroyBody(tag[i]);
                delete bodies[tag[i]];
            }
            return;
        }
        Physics.destroyBody(tag);
        delete bodies[tag];
    }

    function destroyAll() {
        Physics.destroyAll();
        bodies = {};
    }

    function getPosition(tag) {
        var t = Physics.getTransform(tag);
        if (!t) return { x: 0, y: 0 };
        return { x: toCanvasX(t.position.x), y: toCanvasY(t.position.y) };
    }

    function getAngle(tag) {
        var t = Physics.getTransform(tag);
        if (!t) return 0;
        return quatToAngle(t.rotation);
    }

    function getTransform(tag) {
        var t = Physics.getTransform(tag);
        if (!t) return { x: 0, y: 0, angle: 0 };
        return {
            x: toCanvasX(t.position.x),
            y: toCanvasY(t.position.y),
            angle: quatToAngle(t.rotation)
        };
    }

    function getVelocity(tag) {
        var v = Physics.getVelocity(tag);
        if (!v) return { x: 0, y: 0 };
        return { x: v.linear.x, y: -v.linear.y };
    }

    function setPosition(tag, x, y) {
        Physics.setPosition(tag, toPhysX(x), toPhysY(y), 0);
    }

    function setVelocity(tag, vx, vy) {
        Physics.setLinearVelocity(tag, vx, -vy, 0);
    }

    function addForce(tag, fx, fy) {
        Physics.addForce(tag, fx, -fy, 0);
    }

    function addImpulse(tag, ix, iy) {
        Physics.addImpulse(tag, ix, -iy, 0);
    }

    function activate(tag) {
        Physics.activate(tag);
    }

    function isActive(tag) {
        return Physics.isActive(tag);
    }

    function setGravity(gx, gy) {
        Physics.setGravity(gx, -gy, 0);
    }

    function getContacts() {
        return Physics.getContacts();
    }

    function raycast(x1, y1, x2, y2) {
        var dx = x2 - x1;
        var dy = y2 - y1;
        var dist = Math.sqrt(dx * dx + dy * dy);
        if (dist === 0) return [];
        var hits = Physics.raycast(
            toPhysX(x1), toPhysY(y1), 0,
            dx / dist, -(dy / dist), 0,
            dist
        );
        for (var i = 0; i < hits.length; i++) {
            hits[i].position = {
                x: toCanvasX(hits[i].position.x),
                y: toCanvasY(hits[i].position.y)
            };
        }
        return hits;
    }

    function getBody(tag) {
        return bodies[tag] || null;
    }

    // No-op kept for API compatibility (DOF lock removes the need for Z clamping).
    function step() {}

    return {
        init: init,
        createBox: createBox,
        createCircle: createCircle,
        createPolyline: createPolyline,
        createSensor: createSensor,
        createDistanceConstraint: createDistanceConstraint,
        createPinConstraint: createPinConstraint,
        createHingeConstraint: createHingeConstraint,
        destroyConstraint: destroyConstraint,
        setLayer: setLayer,
        createKinematic: createKinematic,
        setKinematicTarget: setKinematicTarget,
        destroyBody: destroyBody,
        destroyAll: destroyAll,
        getPosition: getPosition,
        getAngle: getAngle,
        getTransform: getTransform,
        getVelocity: getVelocity,
        setPosition: setPosition,
        setVelocity: setVelocity,
        addForce: addForce,
        addImpulse: addImpulse,
        activate: activate,
        isActive: isActive,
        setGravity: setGravity,
        getContacts: getContacts,
        raycast: raycast,
        getBody: getBody,
        step: step
    };
})();
