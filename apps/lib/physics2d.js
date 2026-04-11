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
// Usage:
//   <script src="../lib/physics2d.js"></script>
//   Physics2D.init({ gravity: 980, width: 800, height: 600 });
//   var ball = Physics2D.createCircle(400, 300, 10, { restitution: 0.9 });
//   // in game loop:
//   Physics2D.step();
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
        // Gravity points down in canvas → negative Y in physics
        Physics.setGravity(0, -gravity, 0);
        bodies = {};
    }

    function createBox(x, y, w, h, opts) {
        opts = opts || {};
        var tag = Physics.createBody({
            shape: "box",
            position: { x: toPhysX(x), y: toPhysY(y), z: 0 },
            halfExtents: { x: w / 2, y: h / 2, z: 10 }, // z thick enough to collide
            static: !!opts.static,
            friction: opts.friction !== undefined ? opts.friction : 0.5,
            restitution: opts.restitution !== undefined ? opts.restitution : 0.3
        });
        bodies[tag] = { tag: tag, type: "box", width: w, height: h };
        return tag;
    }

    function createCircle(x, y, radius, opts) {
        opts = opts || {};
        var tag = Physics.createBody({
            shape: "sphere",
            position: { x: toPhysX(x), y: toPhysY(y), z: 0 },
            radius: radius,
            static: !!opts.static,
            friction: opts.friction !== undefined ? opts.friction : 0.5,
            restitution: opts.restitution !== undefined ? opts.restitution : 0.3
        });
        bodies[tag] = { tag: tag, type: "circle", radius: radius };
        return tag;
    }

    function destroyBody(tag) {
        Physics.destroyBody(tag);
        delete bodies[tag];
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
        // Convert direction to physics coords (flip Y)
        var hits = Physics.raycast(
            toPhysX(x1), toPhysY(y1), 0,
            dx / dist, -(dy / dist), 0,
            dist
        );
        // Convert hit positions back to canvas coords
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

    // Clamp all bodies to Z=0 plane to prevent drift.
    // Call once per frame.
    function step() {
        var transforms = Physics.getAllTransforms();
        if (!transforms || transforms.length === 0) return;
        // Format: [tag, x, y, z, rx, ry, rz, rw, ...]
        for (var i = 0; i < transforms.length; i += 8) {
            var tag = transforms[i];
            var z = transforms[i + 3];
            if (Math.abs(z) > 0.01) {
                Physics.setPosition(tag, transforms[i + 1], transforms[i + 2], 0);
            }
        }
    }

    return {
        init: init,
        createBox: createBox,
        createCircle: createCircle,
        destroyBody: destroyBody,
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
