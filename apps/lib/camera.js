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
    // Separates rotation pivot from camera position so panning doesn't shift
    // what rotation orbits around:
    //   - `pivot` is the fixed rotation origin (set at load time from the
    //     model's bbox, for example).
    //   - `pos` is the absolute camera position.
    //   - `rot` is the camera orientation (quaternion; gimbal-lock-free).
    // Mouselook rotates `pos` around `pivot` while preserving the camera's
    // pivot-local offset. Panning translates `pos` along screen axes without
    // touching `pivot`.

    function createOrbit(opts) {
        opts = opts || {};
        const pivot = (opts.pivot || opts.target) ?
            (opts.pivot || opts.target).slice() : [0, 0, 0];
        // Slight downward tilt by default — a "look from above" starting pose.
        const rot = opts.rot ? opts.rot.slice() : quatFromAxis(1, 0, 0, -0.2);
        const dist = opts.dist != null ? opts.dist : 4;
        const pos = opts.pos ? opts.pos.slice()
            : v3add(pivot, quatRotVec(rot, [0, 0, dist]));
        const cam = {
            pivot, pos, rot,
            fov:    opts.fov    != null ? opts.fov    : 45,
            near:   opts.near   != null ? opts.near   : 0.1,
            far:    opts.far    != null ? opts.far    : 1000,
            yawSpeed:   opts.yawSpeed   != null ? opts.yawSpeed   : 0.005,
            pitchSpeed: opts.pitchSpeed != null ? opts.pitchSpeed : 0.005,
            // Pan scales with `dist` so a drag covers a similar fraction of
            // the view at any zoom. ~0.001 ≈ 1 px per 0.1% of radius,
            // close to cursor-follows-content at a 45° FOV.
            panSpeed:   opts.panSpeed   != null ? opts.panSpeed   : 0.001,
        };
        // `dist` = distance from pivot to camera. Read as current radius;
        // assign to rescale the pivot→pos offset while preserving direction.
        // The setter lets existing `cam.dist = cam.dist * factor` zoom code
        // keep working without needing to know about `pos`.
        Object.defineProperty(cam, 'dist', {
            enumerable: true,
            get() {
                const dx = this.pos[0] - this.pivot[0];
                const dy = this.pos[1] - this.pivot[1];
                const dz = this.pos[2] - this.pivot[2];
                return Math.sqrt(dx*dx + dy*dy + dz*dz);
            },
            set(v) {
                const dx = this.pos[0] - this.pivot[0];
                const dy = this.pos[1] - this.pivot[1];
                const dz = this.pos[2] - this.pivot[2];
                const L = Math.sqrt(dx*dx + dy*dy + dz*dz);
                if (L < 1e-9) {
                    const off = quatRotVec(this.rot, [0, 0, v]);
                    this.pos = [this.pivot[0]+off[0], this.pivot[1]+off[1], this.pivot[2]+off[2]];
                } else {
                    const s = v / L;
                    this.pos = [this.pivot[0]+dx*s, this.pivot[1]+dy*s, this.pivot[2]+dz*s];
                }
            },
        });
        return cam;
    }

    // Re-frame the camera around a new pivot at a given radius. Preserves the
    // current orientation so the view angle is consistent across model loads.
    function orbitReframe(cam, pivot, dist) {
        cam.pivot = pivot.slice();
        const off = quatRotVec(cam.rot, [0, 0, dist]);
        cam.pos = [pivot[0]+off[0], pivot[1]+off[1], pivot[2]+off[2]];
    }

    // Apply mouse-delta pixels to orbit rotation around `pivot`.
    //   dx > 0 → camera orbits right around the pivot.
    //   dy > 0 → camera orbits up (drag down → view from below).
    // Yaw is applied in world space (around world +Y); pitch around the
    // camera's current right axis. The pivot-local offset is preserved, so
    // if the user has panned the camera off-axis, rotation still orbits the
    // original pivot rather than drifting around the panned view center.
    function orbitLook(cam, dx, dy) {
        const yaw   = -dx * cam.yawSpeed;
        const pitch = -dy * cam.pitchSpeed;
        const qy = quatFromAxis(0, 1, 0, yaw);
        const qp = quatFromAxis(1, 0, 0, pitch);
        const rotNew = quatNorm(quatMul(quatMul(qy, cam.rot), qp));
        // Camera's offset from pivot expressed in its local frame is
        // invariant under pure rotation. Move it from old frame to new one.
        const ox = cam.pos[0] - cam.pivot[0];
        const oy = cam.pos[1] - cam.pivot[1];
        const oz = cam.pos[2] - cam.pivot[2];
        const rotInv = [-cam.rot[0], -cam.rot[1], -cam.rot[2], cam.rot[3]];
        const local = quatRotVec(rotInv, [ox, oy, oz]);
        const off = quatRotVec(rotNew, local);
        cam.pos = [cam.pivot[0]+off[0], cam.pivot[1]+off[1], cam.pivot[2]+off[2]];
        cam.rot = rotNew;
    }

    // Pan the camera along the camera's screen-space axes. Content follows
    // the cursor (drag right → scene moves right). Rate scales with the
    // current pivot radius so panning feels consistent across zooms. Pivot
    // does NOT move — so a later rotate still orbits the original pivot.
    function orbitPan(cam, dx, dy) {
        const right = quatRotVec(cam.rot, [1, 0, 0]);
        const up    = quatRotVec(cam.rot, [0, 1, 0]);
        const k = cam.dist * cam.panSpeed;
        cam.pos[0] += (-dx * right[0] + dy * up[0]) * k;
        cam.pos[1] += (-dx * right[1] + dy * up[1]) * k;
        cam.pos[2] += (-dx * right[2] + dy * up[2]) * k;
    }

    function orbitPosition(cam) {
        return cam.pos.slice();
    }

    function orbitUp(cam) {
        return quatRotVec(cam.rot, [0, 1, 0]);
    }

    // View options submitted to scene.setCamera. The look-at target is one
    // unit along the camera's forward axis — so after panning, the camera
    // looks straight ahead (not back at the pivot).
    function orbitViewOpts(cam, canvas) {
        const fwd = quatRotVec(cam.rot, [0, 0, -1]);
        // Omit `aspect`: the engine derives it from the scene graph's own
        // canvas/FBO dimensions (layoutBox content rect), which is the same
        // source the viewport uses. Passing clientWidth/clientHeight here can
        // disagree with the engine's view of the canvas (e.g. before layout
        // has settled on the first frame, or when canvas intrinsic sizing
        // interacts with CSS), producing squished cubes.
        return {
            fov: cam.fov, near: cam.near, far: cam.far,
            position: cam.pos.slice(),
            target: [cam.pos[0]+fwd[0], cam.pos[1]+fwd[1], cam.pos[2]+fwd[2]],
            up: quatRotVec(cam.rot, [0, 1, 0]),
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

    // Roll around the camera's forward axis. dir = +1 rolls counterclockwise
    // when looking along forward (matches Q=left in terrain).
    function flyRoll(cam, dt, dir) {
        const f = flyForward(cam);
        cam.rot = quatNorm(quatMul(
            quatFromAxis(f[0], f[1], f[2], dir * cam.rollSpeed * dt),
            cam.rot));
    }

    // Build a world-space thrust vector from a { key: bool } map using the
    // canonical WASD + Space/Ctrl layout (camera-local up/down, not world).
    // Returns a unit vector, or [0,0,0] when no keys are active.
    function flyThrustFromKeys(cam, keys) {
        const f = flyForward(cam);
        const r = flyRight(cam);
        const u = flyUp(cam);
        let x = 0, y = 0, z = 0;
        if (keys['w']) { x += f[0]; y += f[1]; z += f[2]; }
        if (keys['s']) { x -= f[0]; y -= f[1]; z -= f[2]; }
        if (keys['d']) { x += r[0]; y += r[1]; z += r[2]; }
        if (keys['a']) { x -= r[0]; y -= r[1]; z -= r[2]; }
        if (keys[' '])       { x += u[0]; y += u[1]; z += u[2]; }
        if (keys['control']) { x -= u[0]; y -= u[1]; z -= u[2]; }
        const len = Math.sqrt(x*x + y*y + z*z);
        if (len < 1e-6) return [0, 0, 0];
        const inv = 1 / len;
        return [x * inv, y * inv, z * inv];
    }

    // Velocity-integrated movement with exponential-smoothed accel/damping.
    // `thrust` is a world-space direction (typically unit-length, but any
    // magnitude works — zero means coast). `speed` is target velocity.
    function flyIntegrate(cam, thrust, dt, speed) {
        const thrustLen = Math.sqrt(
            thrust[0]*thrust[0] + thrust[1]*thrust[1] + thrust[2]*thrust[2]);
        const accelBlend   = 1 - Math.exp(-cam.accel   * dt);
        const dampingBlend = 1 - Math.exp(-cam.damping * dt);
        if (thrustLen > 1e-6) {
            cam.vel[0] += (thrust[0] * speed - cam.vel[0]) * accelBlend;
            cam.vel[1] += (thrust[1] * speed - cam.vel[1]) * accelBlend;
            cam.vel[2] += (thrust[2] * speed - cam.vel[2]) * accelBlend;
        } else {
            cam.vel[0] *= (1 - dampingBlend);
            cam.vel[1] *= (1 - dampingBlend);
            cam.vel[2] *= (1 - dampingBlend);
        }
        cam.pos[0] += cam.vel[0] * dt;
        cam.pos[1] += cam.vel[1] * dt;
        cam.pos[2] += cam.vel[2] * dt;
    }

    // target+up submission — broad engine compatibility.
    function flyViewOpts(cam, canvas) {
        return {
            fov: cam.fov, near: cam.near, far: cam.far,
            position: cam.pos.slice(),
            target: v3add(cam.pos, flyForward(cam)),
            up: flyUp(cam),
            aspect: canvas.clientWidth / Math.max(1, canvas.clientHeight),
        };
    }

    // position+quaternion submission — preserves full 6DOF roll exactly.
    function flyViewOptsQuat(cam, canvas) {
        return {
            fov: cam.fov, near: cam.near, far: cam.far,
            position: cam.pos.slice(),
            quaternion: cam.rot.slice(),
            aspect: canvas.clientWidth / Math.max(1, canvas.clientHeight),
        };
    }

    // --- Export -------------------------------------------------------------

    global.Camera = {
        // math
        v3add, v3scale,
        quatFromAxis, quatMul, quatNorm, quatRotVec,
        // orbit
        createOrbit, orbitReframe, orbitLook, orbitPan,
        orbitPosition, orbitUp, orbitViewOpts,
        // fly
        createFly, flyLook, flyRoll, flyThrustFromKeys, flyIntegrate,
        flyForward, flyRight, flyUp, flyViewOpts, flyViewOptsQuat,
    };
})(typeof window !== 'undefined' ? window : globalThis);
