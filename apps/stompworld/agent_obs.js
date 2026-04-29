// agent_obs.js — observation builder for the stompworld AI.
//
// Built on top of bro.ai.game.grid.createObsWindow (the brogameagent grid
// kit). The window rasters a fixed footprint around the player into a
// flat Float32Array; tile + entity layers are appended after the self
// block, in this order:
//
//   self block   (12 floats — see SELF layout below)
//   tile layer   (cols 13 × rows 9 × 2 channels = 234 floats)
//                  ch 0: original-layout solid (1 if any solid tile id)
//                  ch 1: destructible (1 if solid AND not the indestructible
//                        ground id). Per-pixel destruction state is not
//                        exposed in obs; the cumulative destruction count
//                        is captured in the self block instead.
//   stomper layer (cols 13 × rows 9 × 1 channel  = 117 floats)
//                  ch 0: valid alive stomper present in this cell (count-
//                        normalized to 1 in case multiples land in one cell)
//   flyer layer  (cols 13 × rows 9 × 1 channel  = 117 floats)
//                  ch 0: valid alive flyer
//   pickup layer (cols 13 × rows 9 × 1 channel  = 117 floats)
//                  ch 0: pickup present in this cell (zeroed once collected)
//
// Self block layout:
//   [0]  vx / runSpeed              clamp [-1, 1]
//   [1]  vy / maxFall               clamp [-1, 1]
//   [2]  onGround                   {0, 1}
//   [3]  facing                     {-1, +1}
//   [4]  coyote / coyoteTime        clamp [0, 1]
//   [5]  buffer / jumpBuffer        clamp [0, 1]
//   [6]  dx_to_pickup / 800         signed, clamp [-1, 1] (0 if collected)
//   [7]  dx_to_flag   / 800         signed, clamp [-1, 1]
//   [8]  hasWeapon                  {0, 1}
//   [9]  pickupCollected            {0, 1}
//   [10] weaponCooldown / cooldownMax  clamp [0, 1]
//   [11] phase / 2                  {0, 0.5, 1}
//   [12] dx_to_seen_left  / 800     signed (≤ 0), 0 if no remembered enemy left
//   [13] dx_to_seen_right / 800     signed (≥ 0), 0 if no remembered enemy right
//
// "Seen" = an enemy whose center has been within SEEN_RADIUS_PX of the
// player at any point this episode. Persisted on each entity until the
// episode ends. Lets the agent remember enemies it walked past pre-pickup
// and navigate back to engage them after picking up the gun.

(function (global) {
    'use strict';

    const TILE = 32;

    // Window footprint in tile cells.
    const COLS_BEHIND = 2;
    const COLS_AHEAD  = 10;
    const ROWS_UP     = 4;
    const ROWS_DOWN   = 4;

    const SELF_BLOCK_SIZE = 14;

    const RUN_SPEED   = 240;
    const MAX_FALL    = 900;
    const COYOTE_T    = 100;
    const JUMP_BUFFER = 120;

    function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // Bound to a sim instance the first time build(sim) is called. The
    // ObsWindow's tile/layer samplers close over this reference so they
    // see whatever sim is currently active. Single-sim per worker is the
    // norm here; if that ever changes we should switch to per-sim windows.
    let _sim = null;

    function makeWindow() {
        return bro.ai.game.grid.createObsWindow({
            spec: {
                colsBehind: COLS_BEHIND, colsAhead: COLS_AHEAD,
                rowsUp:     ROWS_UP,     rowsDown:  ROWS_DOWN,
                tileChannels: 2,
                selfBlockSize: SELF_BLOCK_SIZE,
            },
            tile: {
                normalize: new Float32Array([1, 1]),
                oob: new Float32Array([1, 0]),    // out-of-world reads as solid wall
                sample(c, r) {
                    const tm = _sim.tilemap;
                    if (c < 0 || c >= tm.cols || r < 0 || r >= tm.rows) return false;
                    const id = tm.data[r * tm.cols + c];
                    if (!id) return [0, 0];
                    if (!tm.solidAt(c, r)) return [0, 0];
                    return [1, id === 1 ? 0 : 1];   // ground (id 1) is indestructible
                },
            },
            layers: [
                {   // stompers
                    channels: 1,
                    enumerate() { return _sim.stompers.length; },
                    sample(i) {
                        const s = _sim.stompers[i];
                        if (!s.alive) return { col: -1, row: -1, value: 0 };
                        return {
                            col: Math.floor((s.x + s.w / 2) / TILE),
                            row: Math.floor((s.y + s.h / 2) / TILE),
                            value: 1,
                        };
                    },
                },
                {   // flyers
                    channels: 1,
                    enumerate() { return _sim.flyers.length; },
                    sample(i) {
                        const f = _sim.flyers[i];
                        if (!f.alive) return { col: -1, row: -1, value: 0 };
                        return {
                            col: Math.floor((f.x + f.w / 2) / TILE),
                            row: Math.floor((f.y + f.h / 2) / TILE),
                            value: 1,
                        };
                    },
                },
                {   // pickup (single-entity layer)
                    channels: 1,
                    enumerate() {
                        return (_sim.pickup && !_sim.pickupCollected) ? 1 : 0;
                    },
                    sample() {
                        const pk = _sim.pickup;
                        return {
                            col: Math.floor((pk.x + pk.w / 2) / TILE),
                            row: Math.floor((pk.y + pk.h / 2) / TILE),
                            value: 1,
                        };
                    },
                },
            ],
        });
    }

    let _win = null;
    let _selfBuf = null;

    function ensureWindow() {
        if (!_win) {
            _win = makeWindow();
            _selfBuf = new Float32Array(SELF_BLOCK_SIZE);
        }
    }

    function build(sim) {
        _sim = sim;
        ensureWindow();
        const p = sim.player;
        const tm = sim.tilemap;
        const flag = sim.flag;
        const pk   = sim.pickup;
        const cooldownMax = SwSim.WEAPON_COOLDOWN_DECISIONS || 1;

        _selfBuf[0] = clamp(p.vx / RUN_SPEED, -1, 1);
        _selfBuf[1] = clamp(p.vy / MAX_FALL,  -1, 1);
        _selfBuf[2] = p.onGround ? 1 : 0;
        _selfBuf[3] = p.facing < 0 ? -1 : 1;
        _selfBuf[4] = clamp(p.coyote / COYOTE_T,    0, 1);
        _selfBuf[5] = clamp(p.buffer / JUMP_BUFFER, 0, 1);
        _selfBuf[6] = (pk && !sim.pickupCollected)
            ? clamp((pk.x - p.x) / 800, -1, 1) : 0;
        _selfBuf[7] = flag ? clamp((flag.x - p.x) / 800, -1, 1) : 0;
        _selfBuf[8] = sim.hasWeapon ? 1 : 0;
        _selfBuf[9] = sim.pickupCollected ? 1 : 0;
        _selfBuf[10] = clamp(sim.weaponCooldown / cooldownMax, 0, 1);
        _selfBuf[11] = (sim.phase || 0) / 2;

        // Remembered-enemy compass. Scan every alive seen enemy and pick
        // the closest one on each side of the player. Encoded as signed
        // dx / 800 like the pickup/flag features so the network sees a
        // consistent gradient. 0 means "nothing remembered on that side."
        let nearestLeftDx  = 0;     // most-negative dx (closest left enemy)
        let nearestRightDx = 0;     // smallest-positive dx (closest right enemy)
        const px = p.x;
        for (const s of sim.stompers) {
            if (!s.alive || !s.seen) continue;
            const dx = (s.x + s.w / 2) - (px + p.w / 2);
            if (dx < 0 && (nearestLeftDx === 0 || dx > nearestLeftDx)) nearestLeftDx = dx;
            if (dx > 0 && (nearestRightDx === 0 || dx < nearestRightDx)) nearestRightDx = dx;
        }
        for (const f of sim.flyers) {
            if (!f.alive || !f.seen) continue;
            const dx = (f.x + f.w / 2) - (px + p.w / 2);
            if (dx < 0 && (nearestLeftDx === 0 || dx > nearestLeftDx)) nearestLeftDx = dx;
            if (dx > 0 && (nearestRightDx === 0 || dx < nearestRightDx)) nearestRightDx = dx;
        }
        _selfBuf[12] = clamp(nearestLeftDx  / 800, -1, 0);
        _selfBuf[13] = clamp(nearestRightDx / 800,  0, 1);

        const egoCol = Math.floor((p.x + p.w / 2) / TILE);
        const egoRow = Math.floor((p.y + p.h / 2) / TILE);
        return _win.build(egoCol, egoRow, _selfBuf);
    }

    // Lazy OBS_DIM: cannot know until the window is constructed (which
    // requires bro.ai.game.grid to be available). Workers + main both
    // import this module before any sim is built, so ensureWindow on
    // first OBS_DIM read is the simplest contract.
    Object.defineProperty(global, 'SwAgentObs', {
        value: {
            build,
            get OBS_DIM() { ensureWindow(); return _win.outDim; },
        },
        configurable: true,
        writable: true,
    });
})(typeof window !== 'undefined' ? window : globalThis);
