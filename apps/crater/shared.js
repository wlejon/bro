// shared.js — constants and pure helpers used by both client and server.
//
// Loaded as a <script> on the client and duplicated into server.js via the
// same prose (no dynamic load to keep the server's startup surface simple).
// If you edit this file, also refresh the block at the top of server.js.

(function (global) {
    'use strict';

    const C = {
        WORLD_W:      100,                // world X extent
        COLS:         200,                // heightmap column count
        MAX_H:        40,                 // max terrain height
        MIN_H:        0,
        GRAVITY:      40,                 // units/s², pulling -y
        MAX_SPEED:    55,                 // muzzle velocity at power=1
        TURN_TIMEOUT: 30000,              // ms — forfeit a turn past this
        BOT_DELAY:    1200,               // ms — bot aim delay before firing
        BLAST_RADIUS: 7,                  // damage falloff radius
        CRATER_RAD:   4.8,                // terrain dig radius
        MAX_DAMAGE:   55,                 // at impact origin
        TANK_W:       2.4,
        TANK_H:       1.2,
        HP_MAX:       100,
        COLORS: [
            '#e74c3c', '#3498db', '#2ecc71', '#f39c12',
            '#9b59b6', '#1abc9c', '#e67e22', '#e91e63',
        ],
    };
    C.COL_W = C.WORLD_W / C.COLS;

    // Mulberry32 — tiny deterministic PRNG.
    function rng(seed) {
        let a = seed | 0;
        return function () {
            a |= 0; a = (a + 0x6D2B79F5) | 0;
            let t = Math.imul(a ^ (a >>> 15), 1 | a);
            t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
            return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
        };
    }

    // Layered sine/cos heightmap from a seed. Avoids needing an FBm noise
    // lib on both sides — cheap, deterministic, and gives pleasing hills.
    function generateHeightmap(seed) {
        const r = rng(seed);
        // Random coefficients for 4 octaves.
        const octs = [];
        for (let o = 0; o < 4; o++) {
            octs.push({
                freq:  (1 + o * 2) * (0.25 + r() * 0.4),
                amp:   Math.pow(0.55, o) * (0.6 + r() * 0.5),
                phase: r() * Math.PI * 2,
            });
        }
        const hm = new Float32Array(C.COLS);
        for (let i = 0; i < C.COLS; i++) {
            const u = i / C.COLS;
            let h = 0;
            for (const o of octs) {
                h += Math.sin(u * o.freq * Math.PI * 2 + o.phase) * o.amp;
            }
            // Scale [-1, 1] → [minBase, maxBase].
            const norm = (h + 1.5) / 3.0;
            hm[i] = Math.max(4, Math.min(C.MAX_H - 4, 10 + norm * (C.MAX_H - 16)));
        }
        return hm;
    }

    // Sample heightmap at world X via linear interpolation.
    function heightAt(hm, x) {
        const t = (x / C.WORLD_W) * (C.COLS - 1);
        if (t <= 0) return hm[0];
        if (t >= C.COLS - 1) return hm[C.COLS - 1];
        const i = Math.floor(t);
        const f = t - i;
        return hm[i] * (1 - f) + hm[i + 1] * f;
    }

    // Integrate a projectile from (x, y) with initial velocity (vx, vy)
    // against the heightmap. Returns { hit:true, x, y, flightMs, path } on
    // impact, or { hit:false } if it leaves the arena.
    //
    // `path` is a polyline (flat [x0,y0,x1,y1,...]) recorded every `sample`
    // milliseconds — useful for client animation, skipped when opts.recordPath
    // is false. Server-side we only need impact + time, so pass recordPath:false.
    function simulateShot(hm, x, y, vx, vy, opts) {
        opts = opts || {};
        const recordPath = opts.recordPath !== false;
        const dt = 0.01;                           // 10ms physics tick
        const maxT = 20;                           // 20 s cap
        const path = recordPath ? [x, y] : null;
        let sampleT = 0;
        let t = 0;
        while (t < maxT) {
            x  += vx * dt;
            y  += vy * dt;
            vy -= C.GRAVITY * dt;
            t  += dt;
            if (recordPath) {
                sampleT += dt;
                if (sampleT >= 0.025) {           // 25 ms per sample
                    path.push(x, y);
                    sampleT = 0;
                }
            }
            // Out of arena?
            if (x < -5 || x > C.WORLD_W + 5 || y < -20) {
                return { hit: false, x, y, flightMs: t * 1000, path };
            }
            // Below terrain → hit.
            if (x >= 0 && x <= C.WORLD_W && y <= heightAt(hm, x)) {
                if (recordPath) path.push(x, y);
                return { hit: true, x, y, flightMs: t * 1000, path };
            }
        }
        return { hit: false, x, y, flightMs: t * 1000, path };
    }

    // Carve a circular crater into the heightmap. Returns [col, newH]
    // tuples for every column that actually changed — clients apply the
    // same diff without needing to recompute.
    function carveCrater(hm, cx, cy, radius) {
        const minCol = Math.max(0,         Math.floor((cx - radius) / C.COL_W));
        const maxCol = Math.min(C.COLS - 1, Math.ceil((cx + radius) / C.COL_W));
        const changes = [];
        for (let i = minCol; i <= maxCol; i++) {
            const wx = i * C.COL_W + C.COL_W * 0.5;
            const dx = wx - cx;
            const d2 = radius * radius - dx * dx;
            if (d2 <= 0) continue;
            const dyTop = Math.sqrt(d2);
            // If the crater's circle overlaps the column's surface,
            // lower the surface to cy - dyTop (the bottom of the arc).
            const newH = cy - dyTop;
            if (hm[i] > newH) {
                const clamped = Math.max(C.MIN_H, newH);
                hm[i] = clamped;
                changes.push([i, clamped]);
            }
        }
        return changes;
    }

    function applyCraterDiff(hm, changes) {
        for (const [i, h] of changes) hm[i] = h;
    }

    // Compute damage a tank at (tx, ty) takes from an explosion at (cx, cy).
    function blastDamage(cx, cy, tx, ty) {
        const dx = tx - cx, dy = ty - cy;
        const d  = Math.sqrt(dx * dx + dy * dy);
        if (d >= C.BLAST_RADIUS) return 0;
        const falloff = 1 - d / C.BLAST_RADIUS;
        return Math.round(C.MAX_DAMAGE * falloff * falloff);
    }

    global.CraterShared = {
        C, rng, generateHeightmap, heightAt,
        simulateShot, carveCrater, applyCraterDiff, blastDamage,
    };
})(typeof window !== 'undefined' ? window : globalThis);
