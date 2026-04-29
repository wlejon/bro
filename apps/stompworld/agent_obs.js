// agent_obs.js — observation builder for the stompworld AI.
//
// Built on top of bro.ai.game.grid.createObsWindow (the brogameagent grid
// kit). The window rasters a fixed footprint around the player into a
// flat Float32Array; tile + entity layers are appended after the self
// block, in this order:
//
//   self block   (9 floats — see SELF layout below)
//   tile layer   (cols 13 × rows 9 × 2 channels = 234 floats)
//                  ch 0: original-layout solid (1 if any solid tile id)
//                  ch 1: destructible (1 if solid AND not the indestructible
//                        ground id).
//   stomper layer (cols 13 × rows 9 × 1 channel  = 117 floats)
//   flyer layer  (cols 13 × rows 9 × 1 channel  = 117 floats)
//   pickup layer (cols 13 × rows 9 × 1 channel  = 117 floats)
//
// Self block layout:
//   [0] vx / runSpeed              clamp [-1, 1]
//   [1] vy / maxFall               clamp [-1, 1]
//   [2] onGround                   {0, 1}
//   [3] facing                     {-1, +1}
//   [4] coyote / coyoteTime        clamp [0, 1]
//   [5] buffer / jumpBuffer        clamp [0, 1]
//   [6] dx_to_pickup / 800         signed (0 if collected)
//   [7] dx_to_flag   / 800         signed
//   [8] hasWeapon                  {0, 1}
//
// Aiming is scripted (not a learned action), so the observation no longer
// exposes weaponCooldown, phase, or remembered-enemy compass features.

(function (global) {
    'use strict';

    const TILE = 32;

    const COLS_BEHIND = 2;
    const COLS_AHEAD  = 10;
    const ROWS_UP     = 4;
    const ROWS_DOWN   = 4;

    const SELF_BLOCK_SIZE = 9;

    const RUN_SPEED   = 240;
    const MAX_FALL    = 900;
    const COYOTE_T    = 100;
    const JUMP_BUFFER = 120;

    function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }

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
                oob: new Float32Array([1, 0]),
                sample(c, r) {
                    const tm = _sim.tilemap;
                    if (c < 0 || c >= tm.cols || r < 0 || r >= tm.rows) return false;
                    const id = tm.data[r * tm.cols + c];
                    if (!id) return [0, 0];
                    if (!tm.solidAt(c, r)) return [0, 0];
                    return [1, id === 1 ? 0 : 1];
                },
            },
            layers: [
                {
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
                {
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
                {
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
        const flag = sim.flag;
        const pk   = sim.pickup;

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

        const egoCol = Math.floor((p.x + p.w / 2) / TILE);
        const egoRow = Math.floor((p.y + p.h / 2) / TILE);
        return _win.build(egoCol, egoRow, _selfBuf);
    }

    Object.defineProperty(global, 'SwAgentObs', {
        value: {
            build,
            get OBS_DIM() { ensureWindow(); return _win.outDim; },
        },
        configurable: true,
        writable: true,
    });
})(typeof window !== 'undefined' ? window : globalThis);
