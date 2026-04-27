// platformer.js — AABB body + tile-based collision + jump feel helpers.
//
// Body coordinates are world pixels; (x, y) is the top-left of the AABB.
// Collision is axis-separated and uses Tilemap.solidAtPx for solidity, so any
// data source can drive it as long as it implements solidAtPx(px, py).
//
// Jump feel:
//   - coyoteTime: ms after leaving ground during which jump still works
//   - jumpBuffer: ms before landing during which a queued jump fires on touch
//   - jumpCutMul: when input.jumpHeld is released mid-rise, vy *= this
//
// Usage:
//   <script src="../lib/platformer.js"></script>
//   const body = Platformer.createBody({ x: 64, y: 0, w: 24, h: 28 });
//   // each frame:
//   const events = Platformer.step(body, {
//       left: Input.down('left'),
//       right: Input.down('right'),
//       jumpHeld: Input.down('primary'),
//       jumpPressed: Input.pressed('primary'),
//   }, tilemap, dt);
//   if (events.landed) SFX.land();

(function (global) {
    'use strict';

    const DEFAULTS = {
        gravity:    2400,    // px/s²
        maxFall:    900,     // px/s
        runSpeed:   220,     // px/s
        accel:      1800,    // px/s² (ground)
        airAccel:   1200,    // px/s² (air control)
        friction:   2000,    // px/s² (ground, no input)
        jumpVel:    -640,    // px/s (negative = up)
        jumpCutMul: 0.45,
        coyoteTime: 100,     // ms
        jumpBuffer: 120,     // ms
    };

    function createBody(opts) {
        opts = opts || {};
        const b = {
            x: opts.x || 0,
            y: opts.y || 0,
            w: opts.w || 24,
            h: opts.h || 28,
            vx: 0, vy: 0,
            onGround: false,
            facing: 1,
            // timers (ms)
            coyote: 0,
            buffer: 0,
            // tunables (overridable per-body)
            cfg: Object.assign({}, DEFAULTS, opts.cfg || {}),
        };
        return b;
    }

    function moveX(b, dx, tm) {
        let hitWall = false;
        if (dx === 0) return hitWall;
        b.x += dx;
        if (dx > 0) {
            const edge = b.x + b.w - 0.001;
            const col  = Math.floor(edge / tm.tileSize);
            const r0   = Math.floor(b.y / tm.tileSize);
            const r1   = Math.floor((b.y + b.h - 0.001) / tm.tileSize);
            for (let r = r0; r <= r1; r++) {
                if (tm.solidAt(col, r)) {
                    b.x = col * tm.tileSize - b.w;
                    b.vx = 0; hitWall = true; break;
                }
            }
        } else {
            const col = Math.floor(b.x / tm.tileSize);
            const r0  = Math.floor(b.y / tm.tileSize);
            const r1  = Math.floor((b.y + b.h - 0.001) / tm.tileSize);
            for (let r = r0; r <= r1; r++) {
                if (tm.solidAt(col, r)) {
                    b.x = (col + 1) * tm.tileSize;
                    b.vx = 0; hitWall = true; break;
                }
            }
        }
        return hitWall;
    }

    function moveY(b, dy, tm, ev) {
        if (dy === 0) return;
        b.y += dy;
        if (dy > 0) {
            const edge = b.y + b.h - 0.001;
            const row  = Math.floor(edge / tm.tileSize);
            const c0   = Math.floor(b.x / tm.tileSize);
            const c1   = Math.floor((b.x + b.w - 0.001) / tm.tileSize);
            for (let c = c0; c <= c1; c++) {
                if (tm.solidAt(c, row)) {
                    b.y = row * tm.tileSize - b.h;
                    b.vy = 0;
                    if (!b.onGround) ev.landed = true;
                    b.onGround = true;
                    return;
                }
            }
        } else {
            const row = Math.floor(b.y / tm.tileSize);
            const c0  = Math.floor(b.x / tm.tileSize);
            const c1  = Math.floor((b.x + b.w - 0.001) / tm.tileSize);
            for (let c = c0; c <= c1; c++) {
                if (tm.solidAt(c, row)) {
                    b.y = (row + 1) * tm.tileSize;
                    b.vy = 0; ev.hitCeiling = true; return;
                }
            }
        }
    }

    function step(b, input, tm, dtMs) {
        const dt = dtMs / 1000;
        const c  = b.cfg;
        const ev = { landed: false, hitCeiling: false, hitWall: false, jumped: false };

        // Horizontal control.
        const wantLeft  = !!input.left;
        const wantRight = !!input.right;
        const want = (wantRight ? 1 : 0) - (wantLeft ? 1 : 0);
        const accel = b.onGround ? c.accel : c.airAccel;
        if (want !== 0) {
            b.vx += want * accel * dt;
            if (b.vx >  c.runSpeed) b.vx =  c.runSpeed;
            if (b.vx < -c.runSpeed) b.vx = -c.runSpeed;
            b.facing = want;
        } else if (b.onGround) {
            // Ground friction: pull vx toward zero.
            const dec = c.friction * dt;
            if (b.vx >  dec) b.vx -= dec;
            else if (b.vx < -dec) b.vx += dec;
            else b.vx = 0;
        }

        // Jump buffering / coyote.
        if (input.jumpPressed) b.buffer = c.jumpBuffer;
        else if (b.buffer > 0) b.buffer = Math.max(0, b.buffer - dtMs);

        const wasOnGround = b.onGround;
        if (wasOnGround) b.coyote = c.coyoteTime;
        else if (b.coyote > 0) b.coyote = Math.max(0, b.coyote - dtMs);

        if (b.buffer > 0 && b.coyote > 0) {
            b.vy = c.jumpVel;
            b.onGround = false;
            b.coyote = 0; b.buffer = 0;
            ev.jumped = true;
        }

        // Variable jump height: cut velocity if jump released mid-rise.
        if (!input.jumpHeld && b.vy < 0) b.vy *= c.jumpCutMul;

        // Gravity.
        b.vy += c.gravity * dt;
        if (b.vy > c.maxFall) b.vy = c.maxFall;

        // Move + collide. Reset onGround so we only re-set it on a downward hit.
        b.onGround = false;
        ev.hitWall = moveX(b, b.vx * dt, tm);
        moveY(b, b.vy * dt, tm, ev);

        return ev;
    }

    global.Platformer = { createBody, step, DEFAULTS };
})(typeof window !== 'undefined' ? window : globalThis);
