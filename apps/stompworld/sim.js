// sim.js — headless, snapshot/restorable wrapper around the stompworld
// physics. Used both by the live game (when driven by an AI agent) and as
// the MCTS rollout substrate.
//
// Action space (6 discrete actions, indexed 0..5):
//   0 idle
//   1 left
//   2 right
//   3 jump (no horizontal)
//   4 jump-left
//   5 jump-right
//
// One env.step(action) advances FRAME_SKIP physics ticks at FIXED_DT_MS each.
// Across those ticks, jumpHeld stays true for jump actions; jumpPressed is
// true only on the first tick (matches how a tap registers in Platformer).
//
// Rewards (per env.step) — potential-based shaping via Manhattan distance to
// the flag (in tiles). Only the *change* in distance is paid each step, so
// the optimal policy is preserved (Ng et al. 1999) but the gradient toward
// the flag is dense and signed (going backwards costs).
//   + REW_PROGRESS_PER_TILE * (prevDist - currDist)   progress (signed)
//   + 0.5  per stomper killed
//   - 0.3  on death                 terminal (lowered from -1.0 so movement
//                                   followed by death is not strictly worse
//                                   than standing still and timing out)
//   + 5.0  on flag                  terminal
//   - 0.2  on timeout               terminal
//
// done becomes true on death, flag, or timeout (timeLeft <= 0).

(function (global) {
    'use strict';

    const TILE         = 32;
    const FIXED_DT_MS  = 1000 / 60;     // 16.67 ms — matches the live engine
    const FRAME_SKIP   = 4;             // physics ticks per decision (~67 ms)

    // Player stomp impulse; mirrors app.js handleStompers.
    const STOMP_BOUNCE_VY = -380;

    // Reward shaping.
    const REW_PROGRESS_PER_TILE = 0.02;   // signed: + when distance to flag drops
    const REW_STOMP             =  0.5;
    const REW_DEATH             = -0.3;
    const REW_FLAG              =  5.0;
    const REW_TIMEOUT           = -0.2;

    const STOMP_GRAVITY  = 1800;
    const STOMP_MAX_FALL = 800;

    // Action → input flags.
    function actionToInput(action, isFirstTick, prevJumpHeld) {
        const left  = action === 1 || action === 4;
        const right = action === 2 || action === 5;
        const jump  = action === 3 || action === 4 || action === 5;
        return {
            left, right,
            jumpHeld:    jump,
            // jumpPressed must rise from false→true to fire a buffer event.
            // For per-decision actions, only pulse on the first tick.
            jumpPressed: jump && !prevJumpHeld && isFirstTick,
        };
    }

    // ── Stomper update (lifted from app.js, made pure on (s, tilemap)) ──────
    function stompMoveX(s, dx, tm) {
        s.x += dx;
        const r0 = Math.floor(s.y / TILE);
        const r1 = Math.floor((s.y + s.h - 0.001) / TILE);
        if (dx > 0) {
            const col = Math.floor((s.x + s.w - 0.001) / TILE);
            for (let r = r0; r <= r1; r++) {
                if (tm.solidAt(col, r)) { s.x = col * TILE - s.w; s.vx = -Math.abs(s.vx); return; }
            }
        } else if (dx < 0) {
            const col = Math.floor(s.x / TILE);
            for (let r = r0; r <= r1; r++) {
                if (tm.solidAt(col, r)) { s.x = (col + 1) * TILE; s.vx = Math.abs(s.vx); return; }
            }
        }
    }
    function stompMoveY(s, dy, tm) {
        s.y += dy;
        const c0 = Math.floor(s.x / TILE);
        const c1 = Math.floor((s.x + s.w - 0.001) / TILE);
        if (dy > 0) {
            const row = Math.floor((s.y + s.h - 0.001) / TILE);
            for (let c = c0; c <= c1; c++) {
                if (tm.solidAt(c, row)) { s.y = row * TILE - s.h; s.vy = 0; s.onGround = true; return; }
            }
        } else if (dy < 0) {
            const row = Math.floor(s.y / TILE);
            for (let c = c0; c <= c1; c++) {
                if (tm.solidAt(c, row)) { s.y = (row + 1) * TILE; s.vy = 0; return; }
            }
        }
    }
    function stepStomper(s, dt, tm) {
        if (!s.alive) { s.squashTimer -= dt; return; }
        s.animT += dt;
        const dts = dt / 1000;
        s.vy += STOMP_GRAVITY * dts;
        if (s.vy > STOMP_MAX_FALL) s.vy = STOMP_MAX_FALL;
        s.onGround = false;
        stompMoveX(s, s.vx * dts, tm);
        stompMoveY(s, s.vy * dts, tm);
        if (s.onGround) {
            const probeX = s.vx > 0 ? s.x + s.w + 1 : s.x - 1;
            const probeY = s.y + s.h + 2;
            if (!tm.solidAtPx(probeX, probeY)) s.vx = -s.vx;
        }
    }

    // ── Player ↔ stomper resolution ────────────────────────────────────────
    // Returns kills count (number of stompers squashed this tick) and flips
    // `state.alive=false` if a side hit happened.
    function resolvePlayerStompers(p, stompers) {
        let kills = 0;
        for (const s of stompers) {
            if (!s.alive) continue;
            if (p.x + p.w <= s.x || p.x >= s.x + s.w) continue;
            if (p.y + p.h <= s.y || p.y >= s.y + s.h) continue;
            const fromAbove = p.vy > 0 && (p.y + p.h - s.y) < 16;
            if (fromAbove) {
                s.alive = false;
                s.squashTimer = 350;
                p.vy = STOMP_BOUNCE_VY;
                kills++;
            } else {
                return { kills, killed: true };
            }
        }
        return { kills, killed: false };
    }

    // ── Flyer update + collision ──────────────────────────────────────────
    // Flyers patrol horizontally between [spawnX-patrolRange, spawnX+range]
    // and (optionally) bob vertically as a sine wave around spawnY. They
    // ignore the tilemap entirely (sky enemies). Any AABB contact with the
    // player kills — flyers are not stompable, so the only correct response
    // is to avoid them.
    const FLY_SPEED = 80;
    function stepFlyer(f, dt) {
        const dts = dt / 1000;
        f.x += f.vx * dts;
        if (f.x > f.spawnX + f.patrolRange) {
            f.x = f.spawnX + f.patrolRange;
            f.vx = -Math.abs(f.vx);
        } else if (f.x < f.spawnX - f.patrolRange) {
            f.x = f.spawnX - f.patrolRange;
            f.vx = Math.abs(f.vx);
        }
        if (f.bobAmp > 0) {
            f.bobT += dts;
            const newY = f.spawnY + Math.sin(f.bobT * f.bobFreq) * f.bobAmp;
            f.vy = (newY - f.y) / dts;
            f.y = newY;
        } else {
            f.vy = 0;
        }
        f.animT += dt;
    }
    function resolvePlayerFlyers(p, flyers) {
        for (const f of flyers) {
            if (p.x + p.w <= f.x || p.x >= f.x + f.w) continue;
            if (p.y + p.h <= f.y || p.y >= f.y + f.h) continue;
            return true;   // any contact kills
        }
        return false;
    }

    // ── Public: Sim.create({ tilemap, spawn, stompers, flag, timeLimit }) ─
    function create(level) {
        const tilemap = level.tilemap;
        const flag    = level.flag;
        const timeLimit = level.timeLimit != null ? level.timeLimit : 300;
        // Stall timeout: end the episode early if the player hasn't made
        // forward progress (new max-x by at least `stallEpsilonPx`) for
        // this many consecutive decisions. 0 = disabled. The deadband
        // matters: without it, a 1-pixel jitter against a pipe keeps
        // resetting the counter and the agent flails forever.
        const stallDecisions = level.stallDecisions != null ? level.stallDecisions : 0;
        const stallEpsilonPx = level.stallEpsilonPx != null ? level.stallEpsilonPx : 8;

        // Initial state captured for resets. Mutable so the trainer can
        // implement curriculum learning by re-spawning at later checkpoints.
        let spawnX = level.spawn.x;
        let spawnY = level.spawn.y - 4;
        const stomperTemplates = level.stompers.map((s) => ({ ...s }));
        const flyerTemplates = (level.flyers || []).map((f) => ({ ...f }));

        const playerCfg = {
            gravity:    2400, maxFall:    900,
            runSpeed:   240,  accel:      1800,
            airAccel:   1200, friction:   1800,
            jumpVel:    -850, jumpCutMul: 0.45,
            coyoteTime: 100,  jumpBuffer: 120,
        };

        const state = {
            player: null,
            stompers: null,
            flyers: null,
            score: 0,
            alive: true,
            won: false,
            tick: 0,
            timeLeft: timeLimit,
            // Keep a memory of the most-recent jump-hold flag so adjacent
            // step() calls don't re-pulse jumpPressed every decision.
            prevJumpHeld: false,
            // Manhattan distance to flag in tiles, for PBRS. Updated at the
            // start of each step() so reward = Δ(prev − curr).
            prevDist: 0,
            // Stall detection: max-x reached so far this episode + the
            // count of decisions since that record was last broken.
            stallBestX: 0,
            stallSince: 0,
        };

        // Manhattan distance from player center to flag center, in tiles.
        // Returns 0 if there's no flag (sim is degenerate but we don't crash).
        function distToFlag(p) {
            if (!flag) return 0;
            const px = p.x + p.w / 2;
            const py = p.y + p.h / 2;
            const fx = flag.x + flag.w / 2;
            const fy = flag.y + flag.h / 2;
            return (Math.abs(fx - px) + Math.abs(fy - py)) / TILE;
        }

        function reset() {
            state.player = Platformer.createBody({
                x: spawnX, y: spawnY, w: 24, h: 30, cfg: playerCfg,
            });
            state.player.facing = 1;
            state.stompers = stomperTemplates.map((s) => ({ ...s }));
            state.flyers   = flyerTemplates.map((f) => ({ ...f, bobT: 0, animT: 0 }));
            state.score = 0;
            state.alive = true;
            state.won = false;
            state.tick = 0;
            state.timeLeft = timeLimit;
            state.prevJumpHeld = false;
            state.prevDist = distToFlag(state.player);
            state.stallBestX = state.player.x;
            state.stallSince = 0;
            state.stalledOut = false;
        }
        reset();

        // Curriculum hook: re-spawn at an arbitrary world x. Y is held at
        // the original spawn row (level layout assumption). Caller is
        // responsible for invoking reset() after.
        function setSpawn(x, y) {
            spawnX = x;
            if (y != null) spawnY = y;
        }

        function snapshot() {
            // Deep-copy the parts that mutate. Tilemap and flag are static.
            const p = state.player;
            return {
                player: {
                    x: p.x, y: p.y, w: p.w, h: p.h,
                    vx: p.vx, vy: p.vy,
                    onGround: p.onGround, facing: p.facing,
                    coyote: p.coyote, buffer: p.buffer,
                },
                stompers: state.stompers.map((s) => ({
                    x: s.x, y: s.y, w: s.w, h: s.h,
                    vx: s.vx, vy: s.vy, onGround: s.onGround,
                    alive: s.alive, squashTimer: s.squashTimer, animT: s.animT,
                })),
                flyers: state.flyers.map((f) => ({
                    x: f.x, y: f.y, w: f.w, h: f.h,
                    vx: f.vx, vy: f.vy,
                    spawnX: f.spawnX, spawnY: f.spawnY,
                    patrolRange: f.patrolRange,
                    bobAmp: f.bobAmp, bobFreq: f.bobFreq, bobT: f.bobT,
                    animT: f.animT,
                })),
                score: state.score,
                alive: state.alive,
                won:   state.won,
                tick:  state.tick,
                timeLeft: state.timeLeft,
                prevJumpHeld: state.prevJumpHeld,
                prevDist: state.prevDist,
                stallBestX: state.stallBestX,
                stallSince: state.stallSince,
            };
        }

        function restore(snap) {
            const p = state.player;
            const sp = snap.player;
            p.x = sp.x; p.y = sp.y; p.w = sp.w; p.h = sp.h;
            p.vx = sp.vx; p.vy = sp.vy;
            p.onGround = sp.onGround; p.facing = sp.facing;
            p.coyote = sp.coyote; p.buffer = sp.buffer;
            p.cfg = playerCfg;
            // Replace stompers in-place to preserve array identity for the
            // live game's draw code (which holds a reference).
            const sLen = snap.stompers.length;
            state.stompers.length = sLen;
            for (let i = 0; i < sLen; i++) {
                const src = snap.stompers[i];
                state.stompers[i] = { ...src };
            }
            const fLen = snap.flyers ? snap.flyers.length : 0;
            state.flyers.length = fLen;
            for (let i = 0; i < fLen; i++) {
                state.flyers[i] = { ...snap.flyers[i] };
            }
            state.score = snap.score;
            state.alive = snap.alive;
            state.won   = snap.won;
            state.tick  = snap.tick;
            state.timeLeft = snap.timeLeft;
            state.prevJumpHeld = snap.prevJumpHeld;
            state.prevDist = snap.prevDist != null ? snap.prevDist : distToFlag(state.player);
            state.stallBestX = snap.stallBestX != null ? snap.stallBestX : state.player.x;
            state.stallSince = snap.stallSince != null ? snap.stallSince : 0;
        }

        // Run one physics tick with the supplied input. Returns events.
        function tickPhysics(input, dt) {
            const ev = Platformer.step(state.player, input, tilemap, dt);
            for (const s of state.stompers) stepStomper(s, dt, tilemap);
            for (const f of state.flyers) stepFlyer(f, dt);

            const r = resolvePlayerStompers(state.player, state.stompers);
            ev.kills = r.kills;
            ev.killed = r.killed;
            if (!ev.killed && resolvePlayerFlyers(state.player, state.flyers)) {
                ev.killed = true;
            }
            // Fall out of world.
            if (state.player.y > tilemap.heightPx + 64) ev.killed = true;
            return ev;
        }

        // Public step(action). Advances FRAME_SKIP ticks. Returns {reward, done}.
        function step(action) {
            if (!state.alive || state.won) return { reward: 0, done: true };

            let kills = 0;
            let died  = false;
            let won   = false;

            for (let t = 0; t < FRAME_SKIP; t++) {
                const input = actionToInput(action, t === 0, state.prevJumpHeld);
                const ev = tickPhysics(input, FIXED_DT_MS);
                state.prevJumpHeld = !!input.jumpHeld;
                state.tick++;
                state.timeLeft -= FIXED_DT_MS / 1000;
                kills += ev.kills | 0;
                if (ev.killed) { died = true; break; }
                // Flag check (cheap AABB on flag column).
                if (flag && !state.won) {
                    const p = state.player;
                    if (p.x + p.w >= flag.x + 8 && p.x <= flag.x + flag.w - 8) {
                        state.won = true; won = true; break;
                    }
                }
                if (state.timeLeft <= 0) break;
            }

            // Potential-based shaping: reward the *signed* drop in Manhattan
            // distance to the flag (in tiles) over the decision. Backwards
            // motion costs symmetrically, which suppresses oscillation.
            const currDist = distToFlag(state.player);
            const dDist = state.prevDist - currDist;
            state.prevDist = currDist;
            let reward = REW_PROGRESS_PER_TILE * dDist + REW_STOMP * kills;

            // Stall detection: a new max-x resets the counter; otherwise
            // count decisions since last progress. Crossing the threshold
            // cuts the episode short with the timeout penalty so the
            // trainer doesn't burn the full timeLimit on stuck behavior.
            if (state.player.x > state.stallBestX + stallEpsilonPx) {
                state.stallBestX = state.player.x;
                state.stallSince = 0;
            } else {
                state.stallSince++;
            }
            const stalled = stallDecisions > 0 && state.stallSince >= stallDecisions;

            let done = false;
            if (won)            { reward += REW_FLAG;    done = true; }
            else if (died)      { reward += REW_DEATH;   done = true; state.alive = false; }
            else if (state.timeLeft <= 0) { reward += REW_TIMEOUT; done = true; state.alive = false; }
            else if (stalled)             { reward += REW_TIMEOUT; done = true; state.alive = false; state.stalledOut = true; }
            state.score += kills * 100 + (won ? 1000 : 0);
            return { reward, done };
        }

        // All 6 actions are always legal in stompworld.
        const _legal = new Int32Array([0, 1, 2, 3, 4, 5]);
        function legalActions() { return _legal; }

        return {
            // Read-only handles for renderers.
            tilemap,
            flag,
            get player()   { return state.player; },
            get stompers() { return state.stompers; },
            get flyers()   { return state.flyers; },
            get score()    { return state.score; },
            get alive()    { return state.alive; },
            get won()      { return state.won; },
            get tick()     { return state.tick; },
            get timeLeft() { return state.timeLeft; },
            get stalledOut() { return state.stalledOut; },
            // Constants the agent peeks at for observation building.
            tile: TILE,
            frameSkip: FRAME_SKIP,
            // Env interface.
            numActions: 6,
            reset, snapshot, restore, step, legalActions, setSpawn,
        };
    }

    global.SwSim = {
        create,
        TILE, FIXED_DT_MS, FRAME_SKIP,
        REW_PROGRESS_PER_TILE, REW_STOMP, REW_DEATH, REW_FLAG, REW_TIMEOUT,
    };
})(typeof window !== 'undefined' ? window : globalThis);
