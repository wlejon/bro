// platformer.js — AABB body + tile-based collision + jump feel helpers.
//
// Body coordinates are world pixels; (x, y) is the top-left of the AABB.
// Collision is axis-separated. For non-destructible tilemaps we fast-path
// against the tile grid (2 cell checks per axis). For destructible tilemaps
// we walk the leading edge pixel-by-pixel against the per-pixel bitmask, so
// the body falls through holes carved by damageBeam/damageCircle.
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

    // Pixel-precise solid scan along a vertical or horizontal segment.
    // For destructible tilemaps, walks pixel-by-pixel against the bitmask.
    // For tile-grid tilemaps, scans the 1–2 tile rows/cols touched by the
    // body's edge (matches the legacy fast path).
    function anySolidVertical(tm, edgeX, yLo, yHi) {
        if (tm.destructible) {
            for (let y = yLo; y <= yHi; y++) {
                if (tm.solidAtPixel(edgeX, y)) return true;
            }
            return false;
        }
        const ts = tm.tileSize;
        const col = Math.floor(edgeX / ts);
        const r0  = Math.floor(yLo / ts);
        const r1  = Math.floor(yHi / ts);
        for (let r = r0; r <= r1; r++) {
            if (tm.solidAt(col, r)) return true;
        }
        return false;
    }
    function anySolidHorizontal(tm, edgeY, xLo, xHi) {
        if (tm.destructible) {
            for (let x = xLo; x <= xHi; x++) {
                if (tm.solidAtPixel(x, edgeY)) return true;
            }
            return false;
        }
        const ts = tm.tileSize;
        const row = Math.floor(edgeY / ts);
        const c0  = Math.floor(xLo / ts);
        const c1  = Math.floor(xHi / ts);
        for (let c = c0; c <= c1; c++) {
            if (tm.solidAt(c, row)) return true;
        }
        return false;
    }

    function moveX(b, dx, tm) {
        let hitWall = false;
        if (dx === 0) return hitWall;
        b.x += dx;
        const yLo = Math.floor(b.y);
        const yHi = Math.floor(b.y + b.h - 0.001);
        if (dx > 0) {
            const edge = Math.floor(b.x + b.w - 0.001);
            if (anySolidVertical(tm, edge, yLo, yHi)) {
                if (tm.destructible) {
                    // Walk left from the colliding edge until the AABB-tall
                    // strip is clear; that pixel column becomes the new
                    // right-edge resting place. resolveEdge ends at the
                    // first non-solid column; body's right edge sits there.
                    let resolveEdge = edge;
                    while (resolveEdge >= 0 && anySolidVertical(tm, resolveEdge, yLo, yHi)) resolveEdge--;
                    b.x = resolveEdge + 1 - b.w;
                } else {
                    const ts = tm.tileSize;
                    const col = Math.floor(edge / ts);
                    b.x = col * ts - b.w;
                }
                b.vx = 0; hitWall = true;
            }
        } else {
            const edge = Math.floor(b.x);
            if (anySolidVertical(tm, edge, yLo, yHi)) {
                if (tm.destructible) {
                    let resolveEdge = edge;
                    while (resolveEdge < tm.widthPx && anySolidVertical(tm, resolveEdge, yLo, yHi)) resolveEdge++;
                    b.x = resolveEdge;
                } else {
                    const ts = tm.tileSize;
                    const col = Math.floor(edge / ts);
                    b.x = (col + 1) * ts;
                }
                b.vx = 0; hitWall = true;
            }
        }
        return hitWall;
    }

    function moveY(b, dy, tm, ev) {
        if (dy === 0) return;
        b.y += dy;
        const xLo = Math.floor(b.x);
        const xHi = Math.floor(b.x + b.w - 0.001);
        if (dy > 0) {
            const edge = Math.floor(b.y + b.h - 0.001);
            if (anySolidHorizontal(tm, edge, xLo, xHi)) {
                if (tm.destructible) {
                    let resolveEdge = edge;
                    while (resolveEdge >= 0 && anySolidHorizontal(tm, resolveEdge, xLo, xHi)) resolveEdge--;
                    b.y = resolveEdge + 1 - b.h;
                } else {
                    const ts = tm.tileSize;
                    const row = Math.floor(edge / ts);
                    b.y = row * ts - b.h;
                }
                b.vy = 0;
                b.onGround = true;
                return;
            }
        } else {
            const edge = Math.floor(b.y);
            if (anySolidHorizontal(tm, edge, xLo, xHi)) {
                if (tm.destructible) {
                    let resolveEdge = edge;
                    while (resolveEdge < tm.heightPx && anySolidHorizontal(tm, resolveEdge, xLo, xHi)) resolveEdge++;
                    b.y = resolveEdge;
                } else {
                    const ts = tm.tileSize;
                    const row = Math.floor(edge / ts);
                    b.y = (row + 1) * ts;
                }
                b.vy = 0; ev.hitCeiling = true; return;
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

        // landed is the airborne→grounded transition: true only when the
        // body wasn't grounded coming in but is grounded after this step.
        // Using `wasOnGround` here avoids the "fires every frame while
        // standing still" bug — moveY can't tell the difference because
        // we cleared b.onGround above to detect the new collision.
        if (!wasOnGround && b.onGround) ev.landed = true;

        return ev;
    }

    global.Platformer = { createBody, step, DEFAULTS };
})(typeof window !== 'undefined' ? window : globalThis);
