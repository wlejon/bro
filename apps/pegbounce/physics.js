// physics.js — Pegbounce physics, backed by Jolt via Physics.createWorldHandle.
//
// Migration note: this file used to host a hand-rolled 2D circle/box solver.
// It was retired in favour of the engine's Jolt physics. We keep the same
// public surface (Physics.createWorld / addPeg / launchBall / step / predict
// / sweepLit / hasActiveBall / countRemainingOrange / markLitFromEvents) so
// the rest of the app, levels.js, guides.js, and test.js do not need to know
// the underlying solver changed.
//
// Why sandbox worlds (Physics.createWorldHandle) instead of the default world:
//   - Two independent simulations are needed: the LIVE shot and the MIRAGE
//     prediction. Sandbox worlds give each its own body/event space.
//   - Slow-mo (Option C from the migration plan): the live world is stepped
//     manually with a scaled dt, which lets us slow time without poking the
//     engine's auto-stepped default world. See step() below.
//
// Coordinate system: Pegbounce thinks in canvas-style pixels (Y-down). Jolt
// is Y-up. We flip Y locally; bodies live at z=0 with `dofs:'2d'` so they
// can't drift off-plane.
//
// Units: world gravity is in pixels/sec^2. Tunables here are calibrated to
// approximate the feel of the previous solver (RESTITUTION ~0.74 ball, etc.)
// while giving Jolt's solver normal-shaped restitution and friction.

'use strict';
(function (global) {

    // Engine-bound Jolt physics namespace. Captured BEFORE we overwrite
    // global.Physics with the pegbounce module surface below — otherwise
    // every Physics.createWorldHandle call would recurse into ourselves.
    const Jolt = global.Physics;
    if (!Jolt || typeof Jolt.createWorldHandle !== 'function') {
        throw new Error('pegbounce: engine Physics.createWorldHandle missing');
    }


    // ---------- Seedable RNG (kept; level layouts use it deterministically) -
    function rand(seed) {
        let s = (seed | 0) || 1;
        return function () {
            s = (s + 0x6D2B79F5) | 0;
            let t = s;
            t = Math.imul(t ^ (t >>> 15), t | 1);
            t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
            return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
        };
    }

    // ---------- Geom helper retained for level painter callers ---------------
    function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }
    function closestOnSeg(ax, ay, bx, by, px, py) {
        const dx = bx - ax, dy = by - ay;
        const l2 = dx * dx + dy * dy;
        if (l2 < 1e-6) return [ax, ay, 0];
        let t = ((px - ax) * dx + (py - ay) * dy) / l2;
        t = clamp(t, 0, 1);
        return [ax + dx * t, ay + dy * t, t];
    }

    // ---------- Tunables -----------------------------------------------------
    const PEG = { BLUE: 'blue', ORANGE: 'orange', GREEN: 'green', PURPLE: 'purple' };
    const PEG_RADIUS = 9;
    const BALL_RADIUS = 9;
    const GRAVITY = 1400;          // px/sec^2 (matches old feel)
    const MAX_SPEED = 1800;
    const BALL_RESTITUTION = 0.78;
    const BALL_FRICTION = 0.02;
    const BALL_LINEAR_DAMPING = 0.0;
    const PEG_RESTITUTION = 0.78;
    const PEG_FRICTION = 0.02;
    const WALL_RESTITUTION = 0.62;
    const WALL_FRICTION = 0.0;
    const CATCHBAR_RESTITUTION = 0.95;
    const CATCHBAR_FRICTION = 0.0;
    const LAUNCH_SPEED_CAP = 1200; // soft cap

    const FIELD_W = 1024;
    const FIELD_H = 768;
    const FIELD_TOP = 56;
    const FIELD_BOTTOM = FIELD_H;
    const CATCHBAR_Y = FIELD_H - 36;
    const CATCHBAR_H = 14;
    const CATCHBAR_HALFW = 72;

    // ---------- Coordinate flip ----------------------------------------------
    // Pegbounce uses canvas pixels (Y-down). Jolt uses Y-up. We treat the Jolt
    // world's Y origin as the bottom of the playfield.
    function pxY(canvasY) { return FIELD_H - canvasY; }
    function cyY(joltY)   { return FIELD_H - joltY; }

    // ---------- World container ----------------------------------------------
    // Each "world" is a JS object holding:
    //   handle        — Physics sandbox handle (may be omitted for predict
    //                    sandboxes that share the live one between rebuilds)
    //   pegs[]        — js peg records (x, y, type, lit, removed, kind, ...)
    //                    each with .body = Jolt tag (or 0 if removed)
    //   ball / extraBalls — js wrapper records pointing at jolt tags
    //   walls[]       — jolt body tags for the 3 static walls
    //   catchbar      — { x, vx, halfW, y, body }
    //   scoreEvents[] — queued events (peg-hit / wall-hit / catchbar-hit /
    //                    ball-exit) drained by app.js drainEvents().
    //
    // A unique numeric "userKey" per peg is stored as the body's userData.
    // getContacts() returns body tags; we look up bodyTag → js peg via a map.
    function createWorld(opts) {
        opts = opts || {};
        const handle = Jolt.createWorldHandle({
            maxBodies: opts.maxBodies || 1024,
            gravity: { x: 0, y: -GRAVITY, z: 0 },
        });

        const w = {
            handle: handle,
            pegs: [],
            ball: null,
            extraBalls: [],
            catchbar: null,
            walls: [],
            tagToPeg: new Map(),    // bodyTag -> js peg record
            ballTags: new Set(),    // tags of any ball body (live + splits)
            ballRecords: new Map(), // bodyTag -> ball record
            gravity: GRAVITY,
            time: 0,
            slowmo: 0,
            scoreEvents: [],
            pegRadius: PEG_RADIUS,
            ballRadius: BALL_RADIUS,
            fireRadius: 40,
            shotIndex: 0,
            rng: rand(1),
            destroyed: false,
            // Bodies pending destroy at end of step (we never destroy mid-iter
            // to keep contact-event semantics clean).
            pendingDestroy: [],
        };

        addWalls(w);
        addCatchbar(w);
        return w;
    }

    function addWalls(w) {
        // Three static walls: left, right, top. Bottom is open so the ball
        // exits and the shot ends.
        const create = (cx, cy, hw, hh) => {
            return w.handle.createBody({
                shape: 'box',
                static: true,
                position: { x: cx, y: pxY(cy), z: 0 },
                halfExtents: { x: hw, y: hh, z: 10 },
                friction: WALL_FRICTION,
                restitution: WALL_RESTITUTION,
            });
        };
        // Left wall (x:-10..0). Box sits at x=-10 center, half-width 10.
        w.walls.push(create(-10, FIELD_H / 2, 10, FIELD_H));
        // Right
        w.walls.push(create(FIELD_W + 10, FIELD_H / 2, 10, FIELD_H));
        // Top. Sits at the top of the canvas, NOT flush with FIELD_TOP — the
        // cannon (at y=40) launches the ball at y ≈ 40 + sin(angle)*30, which
        // is above FIELD_TOP=56 for shallow angles. Putting the wall body in
        // that range used to trap the ball (it spawned inside the wall and
        // jittered with no apparent gravity when aimed near horizontal).
        w.walls.push(create(FIELD_W / 2, 15, FIELD_W, 10));
    }

    function addCatchbar(w) {
        const cb = {
            x: FIELD_W * 0.5, vx: 180,
            y: CATCHBAR_Y, halfW: CATCHBAR_HALFW,
            body: 0,
        };
        // Kinematic body: dynamic bodies bounce off it but it isn't moved by
        // collisions. We drive its X position each frame in step().
        cb.body = w.handle.createBody({
            shape: 'box',
            position: { x: cb.x, y: pxY(cb.y + CATCHBAR_H / 2), z: 0 },
            halfExtents: { x: cb.halfW, y: CATCHBAR_H / 2, z: 10 },
            friction: CATCHBAR_FRICTION,
            restitution: CATCHBAR_RESTITUTION,
            dofs: '2d',
        });
        w.handle.setKinematic(cb.body);
        w.catchbar = cb;
    }

    // ---------- Peg add ------------------------------------------------------
    let s_userKeyCounter = 1;
    function nextUserKey() { return s_userKeyCounter++; }

    function addPeg(world, x, y, type) {
        const peg = {
            x: x, y: y, type: type,
            lit: false, removed: false,
            kind: 'static',
            phase: world.rng() * Math.PI * 2,
            body: 0,
            userKey: nextUserKey(),
        };
        peg.body = world.handle.createBody({
            shape: 'sphere',
            static: true,
            position: { x: x, y: pxY(y), z: 0 },
            radius: PEG_RADIUS,
            friction: PEG_FRICTION,
            restitution: PEG_RESTITUTION,
            userData: peg.userKey,
        });
        world.pegs.push(peg);
        world.tagToPeg.set(peg.body, peg);
    }

    function addMovingPeg(world, x, y, type, mode, params) {
        const peg = {
            x: x, y: y, type: type,
            lit: false, removed: false,
            kind: 'moving', mode: mode,
            ox: x, oy: y, params: params,
            phase: world.rng() * Math.PI * 2,
            body: 0,
            userKey: nextUserKey(),
        };
        // Kinematic so it pushes the ball but isn't pushed.
        peg.body = world.handle.createBody({
            shape: 'sphere',
            position: { x: x, y: pxY(y), z: 0 },
            radius: PEG_RADIUS,
            friction: PEG_FRICTION,
            restitution: PEG_RESTITUTION,
            userData: peg.userKey,
            dofs: '2d',
        });
        world.handle.setKinematic(peg.body);
        world.pegs.push(peg);
        world.tagToPeg.set(peg.body, peg);
    }

    // ---------- Ball lifecycle ----------------------------------------------
    function makeBallBody(world, x, y, vx, vy) {
        const tag = world.handle.createBody({
            shape: 'sphere',
            position: { x: x, y: pxY(y), z: 0 },
            radius: BALL_RADIUS,
            friction: BALL_FRICTION,
            restitution: BALL_RESTITUTION,
            linearDamping: BALL_LINEAR_DAMPING,
            // Jolt's default mMaxLinearVelocity is 500 (m/s). Pegbounce
            // talks in pixels/sec, so 500 is a hard cap that makes the
            // ball appear to decelerate after ~0.36s of free fall and
            // also clamps launch shots. Lift the cap above MAX_SPEED.
            maxLinearVelocity: MAX_SPEED + 200,
            dofs: '2d',
            ccd: true,
        });
        // velocity: vx is canvas-x, vy is canvas-down. Flip y for jolt.
        world.handle.setLinearVelocity(tag, vx, -vy, 0);
        return tag;
    }

    function destroyBallBody(world, tag) {
        if (!tag) return;
        world.handle.destroyBody(tag);
        world.ballTags.delete(tag);
        world.ballRecords.delete(tag);
    }

    function resetBall(world) {
        if (world.ball) destroyBallBody(world, world.ball.body);
        world.ball = null;
        for (const eb of world.extraBalls) destroyBallBody(world, eb.body);
        world.extraBalls.length = 0;
    }

    function launchBall(world, angleRad, speed, launchX, launchY) {
        // Tear down any prior ball bodies.
        if (world.ball) destroyBallBody(world, world.ball.body);
        for (const eb of world.extraBalls) destroyBallBody(world, eb.body);
        world.extraBalls.length = 0;

        const sp = Math.min(speed, LAUNCH_SPEED_CAP);
        const vx = Math.cos(angleRad) * sp;
        const vy = Math.sin(angleRad) * sp;
        const tag = makeBallBody(world, launchX, launchY, vx, vy);
        const rec = {
            x: launchX, y: launchY,
            vx: vx, vy: vy,
            active: true, radius: BALL_RADIUS,
            onFire: false,
            body: tag,
        };
        world.ball = rec;
        world.ballTags.add(tag);
        world.ballRecords.set(tag, rec);
        if (world.pulses) world.pulses.length = 0;
        world.time = 0;
        world.slowmo = 0;
        world.shotIndex++;
        world.feverBlasted = false;
        world.caughtThisShot = false;
    }

    function spawnSplitBalls(world) {
        if (!world.ball) return;
        const main = world.ball;
        const sp = Math.hypot(main.vx, main.vy);
        const baseAng = Math.atan2(main.vy, main.vx);
        for (let i = -1; i <= 1; i += 2) {
            const ang = baseAng + i * 0.35;
            const vx = Math.cos(ang) * sp;
            const vy = Math.sin(ang) * sp;
            const tag = makeBallBody(world, main.x, main.y, vx, vy);
            const rec = {
                x: main.x, y: main.y,
                vx: vx, vy: vy,
                active: true, radius: BALL_RADIUS,
                onFire: !!main.onFire,
                split: true, life: 1.2,
                body: tag,
            };
            world.extraBalls.push(rec);
            world.ballTags.add(tag);
            world.ballRecords.set(tag, rec);
        }
    }

    // ---------- Step (live simulation) --------------------------------------
    // dtSec is the wall-clock delta. Slow-mo scales it before stepping Jolt;
    // this is Option C from the migration plan (sandbox handle, manual step).
    function step(world, dtSec) {
        if (world.destroyed) return;
        if (world.slowmo > 0) {
            world.slowmo -= dtSec;
            dtSec *= 0.35;
        }
        world.time += dtSec;

        // Pulsewave shock fronts (gameplay only, not physics).
        if (world.pulses && world.pulses.length) {
            for (let i = world.pulses.length - 1; i >= 0; i--) {
                const pw = world.pulses[i];
                pw.age += dtSec;
                const t = Math.min(1, pw.age / pw.duration);
                const front = t * pw.R;
                while (pw.queue.length && pw.queue[0].dist <= front) {
                    const c = pw.queue.shift();
                    if (!c.peg.removed && !c.peg.lit) {
                        world.scoreEvents.push({ kind: 'peg-hit', peg: c.peg });
                    }
                }
                if (pw.age >= pw.duration && pw.queue.length === 0) {
                    world.pulses.splice(i, 1);
                }
            }
        }

        // Animate moving pegs (kinematic) toward their next pose.
        for (const p of world.pegs) {
            if (p.kind !== 'moving' || p.removed) continue;
            let nx = p.x, ny = p.y;
            if (p.mode === 'orbit') {
                const { radius, speed } = p.params;
                const t = world.time * speed + p.phase;
                nx = p.ox + Math.cos(t) * radius;
                ny = p.oy + Math.sin(t) * radius;
            } else if (p.mode === 'oscillate') {
                const { amp, axis, speed } = p.params;
                const t = world.time * speed + p.phase;
                if (axis === 'x') { nx = p.ox + Math.sin(t) * amp; ny = p.oy; }
                else              { nx = p.ox; ny = p.oy + Math.sin(t) * amp; }
            }
            p.x = nx; p.y = ny;
            // Drive kinematic body via moveKinematic so contacts are stable.
            world.handle.moveKinematic(p.body, nx, pxY(ny), 0, dtSec || 1/60);
        }

        // Drive the catchbar bouncing left/right (visual + physical).
        const cb = world.catchbar;
        if (cb) {
            cb.x += cb.vx * dtSec;
            if (cb.x - cb.halfW < 0) { cb.x = cb.halfW; cb.vx = Math.abs(cb.vx); }
            if (cb.x + cb.halfW > FIELD_W) { cb.x = FIELD_W - cb.halfW; cb.vx = -Math.abs(cb.vx); }
            world.handle.moveKinematic(cb.body, cb.x, pxY(cb.y + CATCHBAR_H / 2), 0, dtSec || 1/60);
        }

        // Step the world. Sub-step a couple of times if dt is bigger than
        // 1/120 to limit tunneling for fast balls (CCD also helps).
        const subs = Math.max(1, Math.ceil(dtSec / (1/120)));
        const sdt = dtSec / subs;
        for (let i = 0; i < subs; i++) {
            world.handle.step(sdt);
        }

        // Cap ball speed to keep solver stable.
        capBallSpeed(world);

        // Drain physics contact events into score events.
        drainContacts(world);

        // Sync ball record positions / velocities from Jolt for renderer.
        syncBallRecords(world);

        // Off-field?
        checkExits(world);

        // Extra balls life timer.
        for (let i = world.extraBalls.length - 1; i >= 0; i--) {
            const eb = world.extraBalls[i];
            eb.life -= dtSec;
            if (eb.life <= 0 || !eb.active) {
                if (eb.body) destroyBallBody(world, eb.body);
                world.extraBalls.splice(i, 1);
            }
        }

        // Terraflame: pegs within the fire radius of a fire ball get burned.
        // (Cheap O(n) per ball.)
        for (const ball of activeBalls(world)) {
            if (!ball.onFire) continue;
            const rr = world.fireRadius * world.fireRadius;
            for (const peg of world.pegs) {
                if (peg.removed || peg.lit) continue;
                const dx = peg.x - ball.x;
                const dy = peg.y - ball.y;
                if (dx * dx + dy * dy < rr) {
                    world.scoreEvents.push({ kind: 'peg-hit', peg, fire: true });
                }
            }
        }

        // Apply pending body destroys.
        if (world.pendingDestroy.length) {
            for (const tag of world.pendingDestroy) world.handle.destroyBody(tag);
            world.pendingDestroy.length = 0;
        }
    }

    function activeBalls(world) {
        const out = [];
        if (world.ball && world.ball.active) out.push(world.ball);
        for (const eb of world.extraBalls) if (eb.active) out.push(eb);
        return out;
    }

    function capBallSpeed(world) {
        const tags = [];
        if (world.ball && world.ball.active) tags.push(world.ball.body);
        for (const eb of world.extraBalls) if (eb.active) tags.push(eb.body);
        for (const tag of tags) {
            const v = world.handle.getVelocity ? world.handle.getVelocity(tag) : null;
            if (!v) continue;
            const sp = Math.hypot(v.linear.x, v.linear.y);
            if (sp > MAX_SPEED) {
                const k = MAX_SPEED / sp;
                world.handle.setLinearVelocity(tag, v.linear.x * k, v.linear.y * k, 0);
            }
        }
    }

    function syncBallRecords(world) {
        for (const ball of [world.ball, ...world.extraBalls]) {
            if (!ball || !ball.body) continue;
            const xf = world.handle.getTransform(ball.body);
            const v  = world.handle.getVelocity(ball.body);
            if (xf) {
                ball.x = xf.position.x;
                ball.y = cyY(xf.position.y);
            }
            if (v) {
                ball.vx = v.linear.x;
                ball.vy = -v.linear.y;
            }
        }
    }

    function drainContacts(world) {
        const evs = world.handle.getContacts();
        if (!evs.length) return;
        for (const e of evs) {
            if (e.type !== 'added') continue;
            const a = e.body1, b = e.body2;
            const aIsBall = world.ballTags.has(a);
            const bIsBall = world.ballTags.has(b);
            if (!aIsBall && !bIsBall) continue;
            const ballTag = aIsBall ? a : b;
            const otherTag = aIsBall ? b : a;
            const peg = world.tagToPeg.get(otherTag);
            if (peg) {
                if (!peg.removed) world.scoreEvents.push({ kind: 'peg-hit', peg });
                continue;
            }
            // Catchbar?
            if (world.catchbar && world.catchbar.body === otherTag) {
                world.scoreEvents.push({ kind: 'catchbar-hit' });
                continue;
            }
            // Walls
            if (world.walls.indexOf(otherTag) >= 0) {
                world.scoreEvents.push({ kind: 'wall-hit' });
                continue;
            }
        }
    }

    function checkExits(world) {
        // Ball exits below the field.
        const tryExit = (b) => {
            if (!b || !b.active) return;
            if (b.y - b.radius > FIELD_BOTTOM) {
                b.active = false;
                world.scoreEvents.push({ kind: 'ball-exit', ball: b });
                if (b.body) {
                    world.pendingDestroy.push(b.body);
                    world.ballTags.delete(b.body);
                    world.ballRecords.delete(b.body);
                    b.body = 0;
                }
            }
        };
        tryExit(world.ball);
        for (const eb of world.extraBalls) tryExit(eb);
    }

    // ---------- Mark & sweep -------------------------------------------------
    function markLitFromEvents(world, events) {
        for (const ev of events) {
            if (ev.kind === 'peg-hit' && ev.peg && !ev.peg.lit && !ev.peg.removed) {
                ev.peg.lit = true;
            }
        }
    }

    function sweepLit(world) {
        const removed = [];
        for (const p of world.pegs) {
            if (p.lit && !p.removed) {
                p.removed = true;
                removed.push(p);
                if (p.body) {
                    // Defer the destroy so any in-flight contact event isn't
                    // tied to a tag we just freed.
                    world.tagToPeg.delete(p.body);
                    if (world.handle && !world.destroyed) {
                        world.handle.destroyBody(p.body);
                    }
                    p.body = 0;
                }
            }
        }
        return removed;
    }

    function hasActiveBall(world) {
        if (world.ball && world.ball.active) return true;
        for (const b of world.extraBalls) if (b.active) return true;
        return false;
    }

    function countRemainingOrange(world) {
        let n = 0;
        for (const p of world.pegs) {
            if (p.type === PEG.ORANGE && !p.removed && !p.lit) n++;
        }
        return n;
    }

    // ---------- Predict (Mirage) --------------------------------------------
    // We keep a single shared sandbox handle for predictions, recreating its
    // contents from the live world each call. This is much cheaper than
    // creating/destroying a handle per call.
    let s_predictWorld = null;
    function getPredictWorld() {
        if (!s_predictWorld) {
            s_predictWorld = Jolt.createWorldHandle({
                maxBodies: 1024,
                gravity: { x: 0, y: -GRAVITY, z: 0 },
            });
        }
        return s_predictWorld;
    }

    // Doesn't mutate `world`. Pushes (x, y) samples into `pointsOut`.
    function predict(world, angleRad, speed, launchX, launchY, maxSeconds, pointsOut) {
        if (!world) return;
        const pw = getPredictWorld();
        pw.destroyAll();

        // Re-create static walls and pegs in the predict world.
        const wallSpec = [
            { cx: -10, cy: FIELD_H / 2, hw: 10, hh: FIELD_H },
            { cx: FIELD_W + 10, cy: FIELD_H / 2, hw: 10, hh: FIELD_H },
            { cx: FIELD_W / 2, cy: 15, hw: FIELD_W, hh: 10 },
        ];
        for (const s of wallSpec) {
            pw.createBody({
                shape: 'box', static: true,
                position: { x: s.cx, y: pxY(s.cy), z: 0 },
                halfExtents: { x: s.hw, y: s.hh, z: 10 },
                friction: WALL_FRICTION, restitution: WALL_RESTITUTION,
            });
        }
        for (const p of world.pegs) {
            if (p.removed) continue;
            pw.createBody({
                shape: 'sphere', static: true,
                position: { x: p.x, y: pxY(p.y), z: 0 },
                radius: PEG_RADIUS,
                friction: PEG_FRICTION, restitution: PEG_RESTITUTION,
            });
        }

        // Create the ghost ball.
        const sp = Math.min(speed, LAUNCH_SPEED_CAP);
        const vx = Math.cos(angleRad) * sp;
        const vy = Math.sin(angleRad) * sp;
        const ghost = pw.createBody({
            shape: 'sphere',
            position: { x: launchX, y: pxY(launchY), z: 0 },
            radius: BALL_RADIUS,
            friction: BALL_FRICTION, restitution: BALL_RESTITUTION,
            linearDamping: BALL_LINEAR_DAMPING,
            maxLinearVelocity: MAX_SPEED + 200,
            dofs: '2d', ccd: true,
        });
        pw.setLinearVelocity(ghost, vx, -vy, 0);

        const dt = 1 / 120;
        const totalSteps = Math.floor(maxSeconds / dt);
        const sampleStride = Math.max(1, Math.floor(totalSteps / 80));
        for (let i = 0; i < totalSteps; i++) {
            pw.step(dt);
            if ((i % sampleStride) === 0) {
                const xf = pw.getTransform(ghost);
                if (!xf) break;
                const cx = xf.position.x;
                const cy = cyY(xf.position.y);
                pointsOut.push({ x: cx, y: cy });
                if (cy - BALL_RADIUS > FIELD_BOTTOM) break;
            }
        }
    }

    // ---------- World destroy -----------------------------------------------
    function destroyWorld(world) {
        if (!world || world.destroyed) return;
        world.destroyed = true;
        try { world.handle.destroy(); } catch (e) {}
        world.handle = null;
        world.tagToPeg.clear();
        world.ballTags.clear();
        world.ballRecords.clear();
    }

    // Public API matches the legacy custom-solver surface.
    global.Physics = {
        createWorld, destroyWorld,
        addPeg, addMovingPeg,
        resetBall, launchBall, spawnSplitBalls,
        step, markLitFromEvents, sweepLit,
        hasActiveBall, countRemainingOrange,
        predict,
        PEG, PEG_RADIUS, BALL_RADIUS,
        FIELD_W, FIELD_H, FIELD_TOP, FIELD_BOTTOM,
        CATCHBAR_Y, CATCHBAR_H, CATCHBAR_HALFW,
        rand, closestOnSeg,
    };

})(typeof window !== 'undefined' ? window : globalThis);
