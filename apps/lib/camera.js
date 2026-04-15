// camera.js — shared 3D camera machinery for scene apps.
//
// Quaternion-based, so both fly and orbit cameras are gimbal-lock-free and
// can pitch fully over the top of the target. Extracted from the 6DOF fly
// camera in apps/terrain/app.js.
//
// Usage:
//   <script src="../lib/camera.js"></script>
//   const orbit = Camera.createOrbit({ target: [0,1,0], dist: 4 });
//   Camera.orbitLook(orbit, dx, dy);   // on mousemove (pixels)
//   orbit.dist = Math.max(0.5, orbit.dist + wheelStep);
//   scene.setCamera(Camera.orbitViewOpts(orbit, canvas));

(function (global) {
    'use strict';

    // --- Vector / quaternion helpers (quat = [x, y, z, w]) -------------------

    function v3add(a, b)   { return [a[0]+b[0], a[1]+b[1], a[2]+b[2]]; }
    function v3scale(a, s) { return [a[0]*s, a[1]*s, a[2]*s]; }

    function quatFromAxis(ax, ay, az, angle) {
        const s = Math.sin(angle * 0.5), c = Math.cos(angle * 0.5);
        return [ax * s, ay * s, az * s, c];
    }
    function quatMul(a, b) {
        return [
            a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1],
            a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0],
            a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3],
            a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2]
        ];
    }
    function quatNorm(q) {
        const len = Math.sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
        if (len < 1e-12) return [0, 0, 0, 1];
        return [q[0]/len, q[1]/len, q[2]/len, q[3]/len];
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
            vz + w*tz + (x*ty - y*tx)
        ];
    }

    // --- Orbit camera -------------------------------------------------------
    //
    // Camera sits on a sphere around `target` at radius `dist`. Orientation is
    // a quaternion so pitch can go fully over the top without gimbal lock.
    // The forward axis points at the target: position = target + rot*(0,0,dist).

    function createOrbit(opts) {
        opts = opts || {};
        return {
            target: opts.target ? opts.target.slice() : [0, 0, 0],
            dist:   opts.dist   != null ? opts.dist   : 4,
            fov:    opts.fov    != null ? opts.fov    : 45,
            near:   opts.near   != null ? opts.near   : 0.1,
            far:    opts.far    != null ? opts.far    : 1000,
            // Start with a slight downward tilt (look slightly from above).
            rot:    opts.rot    ? opts.rot.slice()
                                : quatFromAxis(1, 0, 0, -0.2),
            yawSpeed:   opts.yawSpeed   != null ? opts.yawSpeed   : 0.005,
            pitchSpeed: opts.pitchSpeed != null ? opts.pitchSpeed : 0.005,
        };
    }

    // Apply mouse-delta pixels to orbit rotation.
    //   dx > 0 → camera orbits right around the target.
    //   dy > 0 → camera orbits up (drag down → view from below).
    // Yaw is applied in world space (around world +Y), so "up" is always up.
    // Pitch is applied in camera-local space (around camera's right axis), so
    // pitching continues smoothly regardless of yaw.
    function orbitLook(cam, dx, dy) {
        const yaw   = -dx * cam.yawSpeed;
        const pitch = -dy * cam.pitchSpeed;
        // world-yaw * rot * local-pitch
        const qy = quatFromAxis(0, 1, 0, yaw);
        const qp = quatFromAxis(1, 0, 0, pitch);
        cam.rot = quatNorm(quatMul(quatMul(qy, cam.rot), qp));
    }

    function orbitPosition(cam) {
        // Local camera frame: forward = -Z (looking toward target), so the
        // camera sits at +Z * dist in local space.
        return v3add(cam.target, quatRotVec(cam.rot, [0, 0, cam.dist]));
    }

    function orbitUp(cam) {
        return quatRotVec(cam.rot, [0, 1, 0]);
    }

    function orbitViewOpts(cam, canvas) {
        return {
            fov: cam.fov, near: cam.near, far: cam.far,
            position: orbitPosition(cam),
            target: cam.target.slice(),
            up: orbitUp(cam),
            aspect: canvas.clientWidth / Math.max(1, canvas.clientHeight),
        };
    }

    // --- Fly camera ---------------------------------------------------------
    //
    // Free 6DOF camera: translates along its own basis, rotates via mouselook
    // and roll keys. Matches the camera in apps/terrain/app.js.

    function createFly(opts) {
        opts = opts || {};
        return {
            pos:   opts.pos   ? opts.pos.slice()   : [0, 0, 5],
            rot:   opts.rot   ? opts.rot.slice()   : [0, 0, 0, 1],
            vel:   [0, 0, 0],
            fov:   opts.fov   != null ? opts.fov   : 60,
            near:  opts.near  != null ? opts.near  : 0.1,
            far:   opts.far   != null ? opts.far   : 1000,
            accel:     opts.accel     != null ? opts.accel     : 12,
            damping:   opts.damping   != null ? opts.damping   : 6,
            rollSpeed: opts.rollSpeed != null ? opts.rollSpeed : 2.5,
            lookSpeed: opts.lookSpeed != null ? opts.lookSpeed : 0.002,
        };
    }

    function flyForward(cam) { return quatRotVec(cam.rot, [0, 0, -1]); }
    function flyRight(cam)   { return quatRotVec(cam.rot, [1, 0, 0]); }
    function flyUp(cam)      { return quatRotVec(cam.rot, [0, 1, 0]); }

    // Mouselook: yaw around world +Y, pitch around local right.
    function flyLook(cam, dx, dy) {
        const yaw   = -dx * cam.lookSpeed;
        const pitch = -dy * cam.lookSpeed;
        cam.rot = quatNorm(quatMul(
            quatMul(quatFromAxis(0, 1, 0, yaw), cam.rot),
            quatFromAxis(1, 0, 0, pitch)));
    }

    function flyRoll(cam, dt, dir) {
        cam.rot = quatNorm(quatMul(cam.rot,
            quatFromAxis(0, 0, 1, dir * cam.rollSpeed * dt)));
    }

    function flyViewOpts(cam, canvas) {
        return {
            fov: cam.fov, near: cam.near, far: cam.far,
            position: cam.pos.slice(),
            target: v3add(cam.pos, flyForward(cam)),
            up: flyUp(cam),
            aspect: canvas.clientWidth / Math.max(1, canvas.clientHeight),
        };
    }

    // --- Export -------------------------------------------------------------

    global.Camera = {
        // math
        v3add, v3scale,
        quatFromAxis, quatMul, quatNorm, quatRotVec,
        // orbit
        createOrbit, orbitLook, orbitPosition, orbitUp, orbitViewOpts,
        // fly
        createFly, flyLook, flyRoll, flyForward, flyRight, flyUp, flyViewOpts,
    };
})(typeof window !== 'undefined' ? window : globalThis);
