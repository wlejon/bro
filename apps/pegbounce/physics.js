// physics.js — 2D ball vs circle/box physics for Pegbounce.
//
// Chose 2D canvas + custom collision over Jolt because:
//   - Gameplay is fundamentally 2D; there's no reason to pay 3D setup cost.
//   - A small swept-circle vs static-circle solver is trivial to make
//     deterministic and test-friendly, which the test harness needs.
//   - Keeps the whole app free of external dependencies beyond apps/lib.
//
// Units are pixels. World gravity is pixels/sec^2. Time is ms in the outer
// loop but substepped internally in seconds.
//
// Exposes:
//   Physics.World — simulation container with pegs, ball, catch bar, walls.
//   Physics.rand(seed) — seedable RNG so headless tests are stable.
//   Physics.distPointSeg — helpers used by level painter too.

'use strict';
(function (global) {

    // ------------------------- Seedable RNG ------------------------------
    // Mulberry32 is dead simple and fine for our needs.
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

    // ------------------------- Math helpers ------------------------------
    function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // Closest point on segment ab to point p; returns [px, py, t].
    function closestOnSeg(ax, ay, bx, by, px, py) {
        const dx = bx - ax, dy = by - ay;
        const l2 = dx * dx + dy * dy;
        if (l2 < 1e-6) return [ax, ay, 0];
        let t = ((px - ax) * dx + (py - ay) * dy) / l2;
        t = clamp(t, 0, 1);
        return [ax + dx * t, ay + dy * t, t];
    }

    // ------------------------- Peg types --------------------------------
    const PEG = {
        BLUE:   'blue',
        ORANGE: 'orange',
        GREEN:  'green',
        PURPLE: 'purple',
    };

    const PEG_RADIUS = 9;
    const BALL_RADIUS = 9;
    const RESTITUTION = 0.74;
    const FRICTION_TAN = 0.02;       // tangential energy loss on impact
    const GRAVITY = 1400;            // px/sec^2
    const MAX_SPEED = 1650;          // hard cap to keep solver stable
    const WALL_RESTITUTION = 0.62;
    const CATCHBAR_RESTITUTION = 0.35;

    // Playfield logical size. App canvas may scale; physics uses these.
    const FIELD_W = 1024;
    const FIELD_H = 768;
    const FIELD_TOP = 56;             // cannon band height
    const FIELD_BOTTOM = FIELD_H;     // ball exits past this
    const CATCHBAR_Y = FIELD_H - 36;
    const CATCHBAR_H = 14;
    const CATCHBAR_HALFW = 72;

    // ------------------------- World ------------------------------------
    function createWorld() {
        return {
            pegs:          [],     // {x, y, type, lit, removed, vx?, vy?, kind?}
            ball:          null,   // {x,y,vx,vy,active,radius,onFire,splitOf}
            extraBalls:    [],     // Orbital split-ball copies
            catchbar:      { x: FIELD_W * 0.5, vx: 180, y: CATCHBAR_Y, halfW: CATCHBAR_HALFW },
            walls: [
                // axis-aligned boxes: left, right, top
                { x1: -20,        y1: FIELD_TOP, x2: 0,         y2: FIELD_H + 40 },
                { x1: FIELD_W,    y1: FIELD_TOP, x2: FIELD_W + 20, y2: FIELD_H + 40 },
                { x1: -20,        y1: FIELD_TOP - 20, x2: FIELD_W + 20, y2: FIELD_TOP },
            ],
            gravity: GRAVITY,
            time: 0,                  // seconds elapsed in current shot
            slowmo: 0,                // seconds remaining of slowmo
            scoreEvents: [],          // queued per-hit events for scoring code
            pegRadius: PEG_RADIUS,
            ballRadius: BALL_RADIUS,
            fireRadius: 40,           // Terraflame radius
            shotIndex: 0,
            rng: rand(1),
        };
    }

    function addPeg(world, x, y, type) {
        world.pegs.push({
            x, y, type,
            lit: false,
            removed: false,
            kind: 'static',
            phase: world.rng() * Math.PI * 2,
        });
    }

    function addMovingPeg(world, x, y, type, mode, params) {
        const p = {
            x, y, type,
            lit: false, removed: false,
            kind: 'moving',
            mode: mode,          // 'orbit' | 'oscillate'
            ox: x, oy: y,
            params: params,
            phase: world.rng() * Math.PI * 2,
        };
        world.pegs.push(p);
    }

    function resetBall(world) {
        world.ball = null;
        world.extraBalls.length = 0;
    }

    function launchBall(world, angleRad, speed, launchX, launchY) {
        world.ball = {
            x: launchX, y: launchY,
            vx: Math.cos(angleRad) * speed,
            vy: Math.sin(angleRad) * speed,
            active: true,
            radius: BALL_RADIUS,
            onFire: false,
        };
        world.extraBalls.length = 0;
        world.time = 0;
        world.slowmo = 0;
        world.shotIndex++;
        // Purge pegs that were lit in a previous shot (signature delayed-remove
        // happens on ball exit — see finishShot).
    }

    // Append extra balls (Orbital).
    function spawnSplitBalls(world) {
        if (!world.ball) return;
        const main = world.ball;
        for (let i = -1; i <= 1; i += 2) {
            const ang = Math.atan2(main.vy, main.vx) + i * 0.35;
            const sp  = Math.hypot(main.vx, main.vy);
            world.extraBalls.push({
                x: main.x, y: main.y,
                vx: Math.cos(ang) * sp,
                vy: Math.sin(ang) * sp,
                active: true,
                radius: BALL_RADIUS,
                split: true,
                life: 1.2,
            });
        }
    }

    // -------------------- Ball vs peg resolution ------------------------
    function resolveBallPeg(ball, peg, events, ballRadius, pegRadius) {
        const dx = ball.x - peg.x;
        const dy = ball.y - peg.y;
        const d2 = dx * dx + dy * dy;
        const rr = ballRadius + pegRadius;
        if (d2 >= rr * rr) return false;
        const d = Math.sqrt(Math.max(d2, 1e-6));
        const nx = dx / d, ny = dy / d;
        // Push ball out along normal
        const pen = rr - d + 0.01;
        ball.x += nx * pen;
        ball.y += ny * pen;
        // Reflect velocity
        const vdotn = ball.vx * nx + ball.vy * ny;
        if (vdotn < 0) {
            ball.vx -= (1 + RESTITUTION) * vdotn * nx;
            ball.vy -= (1 + RESTITUTION) * vdotn * ny;
            // Tangential friction
            const tx = -ny, ty = nx;
            const vt = ball.vx * tx + ball.vy * ty;
            ball.vx -= FRICTION_TAN * vt * tx;
            ball.vy -= FRICTION_TAN * vt * ty;
        }
        events.push({ kind: 'peg-hit', peg });
        return true;
    }

    // Ball vs axis-aligned box wall. Uses a simple swept-circle approach:
    // find closest point on the box, resolve as with a circle.
    function resolveBallBox(ball, box, events, ballRadius, restitution) {
        const cx = clamp(ball.x, box.x1, box.x2);
        const cy = clamp(ball.y, box.y1, box.y2);
        const dx = ball.x - cx;
        const dy = ball.y - cy;
        const d2 = dx * dx + dy * dy;
        if (d2 >= ballRadius * ballRadius) return false;
        let nx, ny;
        if (d2 < 1e-6) {
            // Inside the box — push out along axis of shallowest penetration.
            const leftPen   = ball.x - box.x1;
            const rightPen  = box.x2 - ball.x;
            const topPen    = ball.y - box.y1;
            const bottomPen = box.y2 - ball.y;
            const minPen = Math.min(leftPen, rightPen, topPen, bottomPen);
            if (minPen === leftPen)       { nx = -1; ny =  0; ball.x = box.x1 - ballRadius; }
            else if (minPen === rightPen) { nx =  1; ny =  0; ball.x = box.x2 + ballRadius; }
            else if (minPen === topPen)   { nx =  0; ny = -1; ball.y = box.y1 - ballRadius; }
            else                          { nx =  0; ny =  1; ball.y = box.y2 + ballRadius; }
        } else {
            const d = Math.sqrt(d2);
            nx = dx / d; ny = dy / d;
            const pen = ballRadius - d + 0.01;
            ball.x += nx * pen;
            ball.y += ny * pen;
        }
        const vdotn = ball.vx * nx + ball.vy * ny;
        if (vdotn < 0) {
            ball.vx -= (1 + restitution) * vdotn * nx;
            ball.vy -= (1 + restitution) * vdotn * ny;
        }
        events.push({ kind: 'wall-hit' });
        return true;
    }

    // --------------------- Integration step -----------------------------
    // Substeps based on speed so the ball can't tunnel through a peg row.
    function step(world, dtSec) {
        if (world.slowmo > 0) {
            world.slowmo -= dtSec;
            dtSec *= 0.35;
        }
        world.time += dtSec;

        // Moving pegs (kinematic animation).
        for (const p of world.pegs) {
            if (p.kind !== 'moving' || p.removed) continue;
            if (p.mode === 'orbit') {
                const { radius, speed } = p.params;
                const t = world.time * speed + p.phase;
                p.x = p.ox + Math.cos(t) * radius;
                p.y = p.oy + Math.sin(t) * radius;
            } else if (p.mode === 'oscillate') {
                const { amp, axis, speed } = p.params;
                const t = world.time * speed + p.phase;
                if (axis === 'x') { p.x = p.ox + Math.sin(t) * amp; p.y = p.oy; }
                else              { p.x = p.ox;                       p.y = p.oy + Math.sin(t) * amp; }
            }
        }

        // Catch bar: bounce off left/right walls.
        const cb = world.catchbar;
        cb.x += cb.vx * dtSec;
        if (cb.x - cb.halfW < 0) { cb.x = cb.halfW; cb.vx = Math.abs(cb.vx); }
        if (cb.x + cb.halfW > FIELD_W) { cb.x = FIELD_W - cb.halfW; cb.vx = -Math.abs(cb.vx); }

        if (!world.ball) return;

        stepBall(world, world.ball, dtSec);
        for (let i = world.extraBalls.length - 1; i >= 0; i--) {
            const eb = world.extraBalls[i];
            eb.life -= dtSec;
            if (eb.life <= 0) { world.extraBalls.splice(i, 1); continue; }
            stepBall(world, eb, dtSec);
            if (!eb.active) world.extraBalls.splice(i, 1);
        }
    }

    function stepBall(world, ball, dtSec) {
        if (!ball || !ball.active) return;
        ball.vy += world.gravity * dtSec;
        const sp = Math.hypot(ball.vx, ball.vy);
        if (sp > MAX_SPEED) {
            ball.vx *= MAX_SPEED / sp;
            ball.vy *= MAX_SPEED / sp;
        }
        // Choose substep count so max movement per step < peg radius.
        const moveLen = Math.hypot(ball.vx, ball.vy) * dtSec;
        const maxStep = world.pegRadius * 0.5;
        const steps = Math.max(1, Math.ceil(moveLen / maxStep));
        const sdt = dtSec / steps;

        for (let s = 0; s < steps; s++) {
            ball.x += ball.vx * sdt;
            ball.y += ball.vy * sdt;

            // Walls
            for (const box of world.walls) {
                resolveBallBox(ball, box, world.scoreEvents, ball.radius, WALL_RESTITUTION);
            }

            // Pegs (O(n) per step; n is small).
            for (const peg of world.pegs) {
                if (peg.removed) continue;
                resolveBallPeg(ball, peg, world.scoreEvents, ball.radius, world.pegRadius);
            }

            // Terraflame: burn pegs within radius without needing contact.
            if (ball.onFire) {
                for (const peg of world.pegs) {
                    if (peg.removed || peg.lit) continue;
                    const dx = peg.x - ball.x;
                    const dy = peg.y - ball.y;
                    if (dx * dx + dy * dy < world.fireRadius * world.fireRadius) {
                        world.scoreEvents.push({ kind: 'peg-hit', peg, fire: true });
                    }
                }
            }

            // Catch bar — a box of width halfW*2 and height CATCHBAR_H.
            const cbBox = {
                x1: world.catchbar.x - world.catchbar.halfW,
                y1: world.catchbar.y,
                x2: world.catchbar.x + world.catchbar.halfW,
                y2: world.catchbar.y + CATCHBAR_H,
            };
            if (resolveBallBox(ball, cbBox, world.scoreEvents, ball.radius, CATCHBAR_RESTITUTION)) {
                world.scoreEvents.push({ kind: 'catchbar-hit' });
            }

            // Exited play field below? Ball is done.
            if (ball.y - ball.radius > FIELD_BOTTOM) {
                ball.active = false;
                world.scoreEvents.push({ kind: 'ball-exit', ball });
                return;
            }
        }
    }

    // Mark events as lit in the peg list. The caller decides scoring.
    function markLitFromEvents(world, events) {
        for (const ev of events) {
            if (ev.kind === 'peg-hit' && ev.peg && !ev.peg.lit && !ev.peg.removed) {
                ev.peg.lit = true;
            }
        }
    }

    // Remove all lit pegs (called when ball exits play field).
    function sweepLit(world) {
        const removed = [];
        for (const p of world.pegs) {
            if (p.lit && !p.removed) {
                p.removed = true;
                removed.push(p);
            }
        }
        return removed;
    }

    // Query: does any active ball exist in the world?
    function hasActiveBall(world) {
        if (world.ball && world.ball.active) return true;
        for (const b of world.extraBalls) if (b.active) return true;
        return false;
    }

    function countRemainingOrange(world) {
        let n = 0;
        for (const p of world.pegs) if (p.type === PEG.ORANGE && !p.removed) n++;
        return n;
    }

    // Predict trajectory points (Mirage guide). Doesn't mutate world.
    function predict(world, angleRad, speed, launchX, launchY, maxSeconds, pointsOut) {
        // Clone ball state minimally; reuse same peg list (they won't be
        // removed inside prediction because we don't touch peg.removed).
        const ghost = {
            x: launchX, y: launchY,
            vx: Math.cos(angleRad) * speed,
            vy: Math.sin(angleRad) * speed,
            active: true,
            radius: BALL_RADIUS,
        };
        const tmpEvents = [];
        const fakeWorld = {
            pegs: world.pegs,
            walls: world.walls,
            gravity: world.gravity,
            pegRadius: world.pegRadius,
            scoreEvents: tmpEvents,
            catchbar: world.catchbar,
            fireRadius: world.fireRadius,
        };
        const dt = 1 / 120;
        const samples = Math.floor(maxSeconds * 40);
        const every = Math.floor((maxSeconds / dt) / samples);
        let step = 0;
        for (let t = 0; t < maxSeconds; t += dt) {
            stepBallPredict(fakeWorld, ghost, dt);
            if (!ghost.active) break;
            if ((step++ % every) === 0) {
                pointsOut.push({ x: ghost.x, y: ghost.y });
                if (pointsOut.length >= samples) break;
            }
        }
    }

    // Predict loop is a hot path but short; trimmed copy of stepBall that
    // doesn't touch lit/removed state.
    function stepBallPredict(world, ball, dtSec) {
        ball.vy += world.gravity * dtSec;
        const moveLen = Math.hypot(ball.vx, ball.vy) * dtSec;
        const maxStep = world.pegRadius * 0.5;
        const steps = Math.max(1, Math.ceil(moveLen / maxStep));
        const sdt = dtSec / steps;
        for (let s = 0; s < steps; s++) {
            ball.x += ball.vx * sdt;
            ball.y += ball.vy * sdt;
            for (const box of world.walls) {
                const cx = clamp(ball.x, box.x1, box.x2);
                const cy = clamp(ball.y, box.y1, box.y2);
                const dx = ball.x - cx, dy = ball.y - cy;
                const d2 = dx * dx + dy * dy;
                if (d2 < ball.radius * ball.radius && d2 > 1e-6) {
                    const d = Math.sqrt(d2);
                    const nx = dx / d, ny = dy / d;
                    const pen = ball.radius - d + 0.01;
                    ball.x += nx * pen; ball.y += ny * pen;
                    const vd = ball.vx * nx + ball.vy * ny;
                    if (vd < 0) {
                        ball.vx -= (1 + WALL_RESTITUTION) * vd * nx;
                        ball.vy -= (1 + WALL_RESTITUTION) * vd * ny;
                    }
                }
            }
            for (const peg of world.pegs) {
                if (peg.removed) continue;
                const dx = ball.x - peg.x, dy = ball.y - peg.y;
                const rr = ball.radius + world.pegRadius;
                const d2 = dx * dx + dy * dy;
                if (d2 < rr * rr && d2 > 1e-6) {
                    const d = Math.sqrt(d2);
                    const nx = dx / d, ny = dy / d;
                    const pen = rr - d + 0.01;
                    ball.x += nx * pen; ball.y += ny * pen;
                    const vd = ball.vx * nx + ball.vy * ny;
                    if (vd < 0) {
                        ball.vx -= (1 + RESTITUTION) * vd * nx;
                        ball.vy -= (1 + RESTITUTION) * vd * ny;
                    }
                }
            }
            if (ball.y - ball.radius > FIELD_BOTTOM) { ball.active = false; return; }
        }
    }

    global.Physics = {
        createWorld, addPeg, addMovingPeg,
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
