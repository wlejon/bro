// agent_obs.js — observation builder for the stompworld AI.
//
// Input: a SwSim instance (live state). Output: a fixed-size Float32Array
// matching SwAgentObs.OBS_DIM. Layout:
//
//   Self block (8 floats):
//     [0] vx / runSpeed                        in [-1, 1]
//     [1] vy / maxFall                         in [-1, 1]
//     [2] onGround                             {0, 1}
//     [3] facing                               {-1, +1}
//     [4] coyote / coyoteTime                  in [0, 1]
//     [5] buffer / jumpBuffer                  in [0, 1]
//     [6] dx_to_flag / 800   (clamped)         signed, capped at one screen
//     [7] dy_to_flag / 256   (clamped)         signed
//
//   Forward tile window (rows 5 × cols 8 = 40 floats), 1.0 = solid:
//     rows = playerRow-2 .. playerRow+2
//     cols = playerCol-1 .. playerCol+6   (1 behind, 6 ahead)
//     index = 8 + (rowOffset+2)*8 + (colOffset+1)
//
//   3 nearest stompers × 5 floats = 15 floats:
//     [valid, dx/300, dy/96, vxSign, alive]
//
//   3 nearest flyers × 5 floats = 15 floats:
//     [valid, dx/300, dy/192, vxSign, vySign]
//     (dy normalized over 192 because flyers live in the sky — full
//      screen-half range. vySign covers bobbing motion.)
//
// Total: 8 + 40 + 15 + 15 = 78 floats.

(function (global) {
    'use strict';

    const TILE = 32;
    const COLS_AHEAD  = 6;
    const COLS_BEHIND = 1;
    const ROWS_UP     = 2;
    const ROWS_DOWN   = 2;
    const N_STOMPERS  = 3;

    const TILE_COLS = COLS_BEHIND + 1 + COLS_AHEAD;          // 8
    const TILE_ROWS = ROWS_UP    + 1 + ROWS_DOWN;            // 5
    const TILE_BLOCK = TILE_COLS * TILE_ROWS;                // 40
    const SELF_BLOCK = 8;
    const N_FLYERS   = 3;
    const STOMPER_BLOCK = N_STOMPERS * 5;                    // 15
    const FLYER_BLOCK   = N_FLYERS   * 5;                    // 15
    const OBS_DIM = SELF_BLOCK + TILE_BLOCK + STOMPER_BLOCK + FLYER_BLOCK; // 78

    const RUN_SPEED   = 240;
    const MAX_FALL    = 900;
    const COYOTE_T    = 100;
    const JUMP_BUFFER = 120;

    const out = new Float32Array(OBS_DIM);

    function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }

    function build(sim) {
        out.fill(0);
        const p = sim.player;
        const tm = sim.tilemap;
        const flag = sim.flag;

        // Self block.
        out[0] = clamp(p.vx / RUN_SPEED, -1, 1);
        out[1] = clamp(p.vy / MAX_FALL,  -1, 1);
        out[2] = p.onGround ? 1 : 0;
        out[3] = p.facing < 0 ? -1 : 1;
        out[4] = clamp(p.coyote / COYOTE_T,    0, 1);
        out[5] = clamp(p.buffer / JUMP_BUFFER, 0, 1);
        if (flag) {
            out[6] = clamp((flag.x - p.x) / 800, -1, 1);
            out[7] = clamp((flag.y - p.y) / 256, -1, 1);
        }

        // Forward tile window.
        const pCol = Math.floor((p.x + p.w / 2) / TILE);
        const pRow = Math.floor((p.y + p.h / 2) / TILE);
        let idx = SELF_BLOCK;
        for (let dr = -ROWS_UP; dr <= ROWS_DOWN; dr++) {
            for (let dc = -COLS_BEHIND; dc <= COLS_AHEAD; dc++) {
                out[idx++] = tm.solidAt(pCol + dc, pRow + dr) ? 1 : 0;
            }
        }

        // 3 nearest stompers (alive or not, but invalid → all zeros).
        const stompers = sim.stompers;
        // Build a (dist², stomper) list, sort in-place by distance squared.
        // Avoid alloc each call by reusing a small scratch (n is small).
        const candidates = [];
        for (let i = 0; i < stompers.length; i++) {
            const s = stompers[i];
            // Discard squashed-and-disposed (squashTimer expired) — still reads
            // as 0s if "valid" stays false; but until timer ends we still
            // present them so the policy learns to pass over them.
            const dx = (s.x + s.w / 2) - (p.x + p.w / 2);
            const dy = (s.y + s.h / 2) - (p.y + p.h / 2);
            candidates.push({ d2: dx * dx + dy * dy, dx, dy, s });
        }
        candidates.sort((a, b) => a.d2 - b.d2);

        let off = SELF_BLOCK + TILE_BLOCK;
        for (let i = 0; i < N_STOMPERS; i++) {
            if (i < candidates.length) {
                const c = candidates[i];
                out[off + 0] = 1;                       // valid
                out[off + 1] = clamp(c.dx / 300, -1, 1);
                out[off + 2] = clamp(c.dy / 96,  -1, 1);
                out[off + 3] = c.s.vx > 0 ? 1 : (c.s.vx < 0 ? -1 : 0);
                out[off + 4] = c.s.alive ? 1 : 0;
            }
            off += 5;
        }

        // 3 nearest flyers.
        const flyers = sim.flyers || [];
        const fcands = [];
        for (let i = 0; i < flyers.length; i++) {
            const f = flyers[i];
            const dx = (f.x + f.w / 2) - (p.x + p.w / 2);
            const dy = (f.y + f.h / 2) - (p.y + p.h / 2);
            fcands.push({ d2: dx * dx + dy * dy, dx, dy, f });
        }
        fcands.sort((a, b) => a.d2 - b.d2);
        for (let i = 0; i < N_FLYERS; i++) {
            if (i < fcands.length) {
                const c = fcands[i];
                out[off + 0] = 1;                       // valid
                out[off + 1] = clamp(c.dx / 300, -1, 1);
                out[off + 2] = clamp(c.dy / 192, -1, 1);
                out[off + 3] = c.f.vx > 0 ? 1 : (c.f.vx < 0 ? -1 : 0);
                out[off + 4] = c.f.vy > 0 ? 1 : (c.f.vy < 0 ? -1 : 0);
            }
            off += 5;
        }
        return out;
    }

    global.SwAgentObs = { build, OBS_DIM };
})(typeof window !== 'undefined' ? window : globalThis);
