// sim.js — headless, snapshot/restorable wrapper around the stompworld
// physics. Used both by the live game (when driven by an AI agent) and as
// the MCTS rollout substrate.
//
// Action space (7 discrete actions, indexed 0..6):
//   0 idle
//   1 left
//   2 right
//   3 jump (no horizontal)
//   4 jump-left
//   5 jump-right
//   6 fire arc beam   (pickup-gated; no-op pre-pickup or while cooling down)
//
// One env.step(action) advances FRAME_SKIP physics ticks at FIXED_DT_MS each.
// For movement actions, jumpHeld stays true across ticks for jump actions and
// jumpPressed pulses on the first tick (matches Platformer's tap semantics).
// The fire action suppresses player movement input for the decision and
// instead fires one beam shot per physics tick along an arc that sweeps
// from ARC_THETA0 to ARC_THETA1 relative to facing direction. Cooldown
// then locks fire for WEAPON_COOLDOWN_DECISIONS decisions.
//
// Pickup: a one-shot pickup tile at level col 115 grants the beam weapon
// when the player AABB first overlaps it. Pre-pickup, action 6 is idle.
//
// Rewards (per env.step):
//   + REW_PER_PIXEL  * pixelsCleared       terrain destroyed by beams
//   + REW_STOMP      * jumpKills           stompers killed by jumping on them
//   + REW_BEAM_STOMP * beamStompKills      stompers killed by beam
//   + REW_BEAM_FLYER * beamFlyerKills      flyers killed by beam (biggest)
//   + REW_PICKUP                           on pickup overlap (transient)
//   + REW_FLAG                             on flag    (terminal)
//   + REW_DEATH                            on death   (terminal)
//   + REW_TIMEOUT                          on timeout (terminal)
//   + (γ·Φ' − Φ)                           potential-based shaping bonus
//                                          (preserves optimal policy)
//
// Phase-aware potential Φ for shaping:
//   phase 0 (no weapon)              → Φ = −dist(player, pickup)  * PBRS_SCALE
//   phase 1 (has weapon, !cleared)   → Φ = −dist(player, spawn)   * PBRS_SCALE
//   phase 2 (cleared OR returned)    → Φ = −dist(player, flag)    * PBRS_SCALE
//
// "cleared" trips when cumulative pixelsDestroyed crosses DESTROY_PHASE_PX
// or when the player has come back at or left of CLEAR_RETURN_COL. The
// per-pixel reward provides the dense "destroy stuff" gradient on top.
//
// done becomes true on death, flag, timeout, or stall (timeLeft <= 0 /
// stall counter).

(function (global) {
    'use strict';

    const TILE         = 32;
    const FIXED_DT_MS  = 1000 / 60;     // 16.67 ms — matches the live engine
    const FRAME_SKIP   = 4;             // physics ticks per decision (~67 ms)

    // Player stomp impulse; mirrors app.js handleStompers.
    const STOMP_BOUNCE_VY = -380;

    // Reward shaping.
    const REW_PER_PIXEL  = 0.0005;
    const REW_STOMP      = 1.0;
    const REW_BEAM_STOMP = 1.0;
    const REW_BEAM_FLYER = 3.0;
    const REW_PICKUP     = 3.0;
    const REW_FLAG       = 1.5;
    const REW_DEATH      = -0.5;
    const REW_TIMEOUT    = -0.3;

    // Potential shaping: small relative to event rewards so destruction +
    // flyer-kill incentives dominate the cumulative return. Inlined as
    // γ·Φ' − Φ math (the grid kit's createPotentialShaper would force
    // out-of-band state for snapshot/restore, which doesn't compose with
    // MCTS).
    const PBRS_GAMMA = 0.99;
    const PBRS_SCALE = 0.01;

    // Phase-1 → phase-2 trigger: enough destruction has accumulated, OR
    // the player has come back to within CLEAR_RETURN_COL of the spawn.
    // ~40 destructible tiles in the level × ~700 cleared px each ≈ 28k.
    const DESTROY_PHASE_PX = 25000;
    const CLEAR_RETURN_COL = 6;

    const STOMP_GRAVITY  = 1800;
    const STOMP_MAX_FALL = 800;

    // Beam / arc sweep.
    const BEAM_LENGTH    = 600;
    const BEAM_THICKNESS = 8;
    const EXPLOSION_R    = 56;
    const ARC_THETA0     = -75 * Math.PI / 180;   // up-and-forward
    const ARC_THETA1     =  30 * Math.PI / 180;   // slightly down-and-forward
    const WEAPON_COOLDOWN_DECISIONS = 4;          // ~270 ms between sweeps

    // Action → input flags. The fire action returns null inputs (player
    // is locked into idle for the duration of the sweep; gravity still
    // applies via the platformer step).
    function actionToInput(action, isFirstTick, prevJumpHeld) {
        if (action === 6) {
            return { left: false, right: false, jumpHeld: false, jumpPressed: false };
        }
        const left  = action === 1 || action === 4;
        const right = action === 2 || action === 5;
        const jump  = action === 3 || action === 4 || action === 5;
        return {
            left, right,
            jumpHeld:    jump,
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
    const FLY_SPEED = 80;
    function stepFlyer(f, dt) {
        if (!f.alive) return;
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
            if (!f.alive) continue;
            if (p.x + p.w <= f.x || p.x >= f.x + f.w) continue;
            if (p.y + p.h <= f.y || p.y >= f.y + f.h) continue;
            return true;
        }
        return false;
    }

    // ── Beam / arc sweep ───────────────────────────────────────────────────
    // Conservative AABB-vs-segment hit (Liang-Barsky against expanded box)
    // plus a circle-vs-AABB explosion test. Mirrors app.js entityHit so
    // sim and live game agree on what got killed.
    function entityHitBeam(e, x0, y0, x1, y1, half, hx, hy, r) {
        const ex0 = e.x, ex1 = e.x + e.w;
        const ey0 = e.y, ey1 = e.y + e.h;
        const cx = hx < ex0 ? ex0 : (hx > ex1 ? ex1 : hx);
        const cy = hy < ey0 ? ey0 : (hy > ey1 ? ey1 : hy);
        const ddx = cx - hx, ddy = cy - hy;
        if (ddx * ddx + ddy * ddy <= r * r) return true;
        const ax0 = ex0 - half, ay0 = ey0 - half;
        const ax1 = ex1 + half, ay1 = ey1 + half;
        const dx = x1 - x0, dy = y1 - y0;
        const ps = [-dx, dx, -dy, dy];
        const qs = [x0 - ax0, ax1 - x0, y0 - ay0, ay1 - y0];
        let t0 = 0, t1 = 1;
        for (let i = 0; i < 4; i++) {
            if (ps[i] === 0) {
                if (qs[i] < 0) return false;
            } else {
                const t = qs[i] / ps[i];
                if (ps[i] < 0) {
                    if (t > t1) return false;
                    if (t > t0) t0 = t;
                } else {
                    if (t < t0) return false;
                    if (t < t1) t1 = t;
                }
            }
        }
        return true;
    }

    // ── Public: Sim.create({ tilemap, spawn, stompers, flag, flyers, pickup, ...}) ─
    function create(level) {
        const tilemap = level.tilemap;
        const flag    = level.flag;
        const pickup  = level.pickup;   // {x, y, w, h} or null
        const timeLimit = level.timeLimit != null ? level.timeLimit : 600;
        const stallDecisions = level.stallDecisions != null ? level.stallDecisions : 0;
        const stallEpsilonPx = level.stallEpsilonPx != null ? level.stallEpsilonPx : 8;

        let spawnX = level.spawn.x;
        let spawnY = level.spawn.y - 4;
        const initialSpawnX = spawnX;
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
            prevJumpHeld: false,

            // Weapon / pickup state.
            hasWeapon: false,
            pickupCollected: false,
            weaponCooldown: 0,          // decisions remaining before next fire
            pixelsDestroyed: 0,         // cumulative across episode
            beamStompKillsTotal: 0,
            beamFlyerKillsTotal: 0,

            // Phase tracking & shaping.
            phase: 0,                    // 0 = pre-pickup, 1 = backtrack, 2 = flag-rush
            prevPhi: 0,                  // last potential value (for γΦ' − Φ)

            // Stall detection (kept inline rather than via grid kit's
            // StallDetector — that one has no snapshot/restore which the
            // MCTS environment contract requires).
            stallBestX: 0,
            stallSince: 0,
            stalledOut: false,
        };

        // ── Distances / phase / potential ──────────────────────────────────
        function distPxToPx(ax, ay, bx, by) {
            return Math.abs(ax - bx) + Math.abs(ay - by);
        }
        function distToPickup(p) {
            if (!pickup) return 0;
            return distPxToPx(
                p.x + p.w / 2, p.y + p.h / 2,
                pickup.x + pickup.w / 2, pickup.y + pickup.h / 2,
            ) / TILE;
        }
        function distToSpawn(p) {
            return distPxToPx(
                p.x + p.w / 2, p.y + p.h / 2,
                initialSpawnX + p.w / 2, spawnY + p.h / 2,
            ) / TILE;
        }
        function distToFlag(p) {
            if (!flag) return 0;
            return distPxToPx(
                p.x + p.w / 2, p.y + p.h / 2,
                flag.x + flag.w / 2, flag.y + flag.h / 2,
            ) / TILE;
        }
        function recomputePhase() {
            if (!state.hasWeapon) state.phase = 0;
            else if (state.pixelsDestroyed >= DESTROY_PHASE_PX
                  || (state.player && state.player.x / TILE <= CLEAR_RETURN_COL)) {
                state.phase = 2;
            } else {
                state.phase = 1;
            }
        }
        function computePhi() {
            const p = state.player;
            if (state.phase === 0) return -distToPickup(p) * PBRS_SCALE;
            if (state.phase === 1) return -distToSpawn(p)  * PBRS_SCALE;
            return -distToFlag(p) * PBRS_SCALE;
        }

        function reset() {
            state.player = Platformer.createBody({
                x: spawnX, y: spawnY, w: 24, h: 30, cfg: playerCfg,
            });
            state.player.facing = 1;
            state.stompers = stomperTemplates.map((s) => ({ ...s }));
            state.flyers   = flyerTemplates.map((f) => ({
                ...f, bobT: 0, animT: 0, alive: true,
            }));
            state.score = 0;
            state.alive = true;
            state.won = false;
            state.tick = 0;
            state.timeLeft = timeLimit;
            state.prevJumpHeld = false;
            state.hasWeapon = false;
            state.pickupCollected = false;
            state.weaponCooldown = 0;
            state.pixelsDestroyed = 0;
            state.beamStompKillsTotal = 0;
            state.beamFlyerKillsTotal = 0;
            tilemap.resetDamage();
            recomputePhase();
            state.prevPhi = computePhi();
            state.stallBestX = state.player.x;
            state.stallSince = 0;
            state.stalledOut = false;
        }
        reset();

        function setSpawn(x, y) {
            spawnX = x;
            if (y != null) spawnY = y;
        }

        function snapshot() {
            const p = state.player;
            const dmg = tilemap.damageDiff();
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
                    animT: f.animT, alive: f.alive,
                })),
                damageDiff: dmg ? new Int32Array(dmg) : null,
                score: state.score,
                alive: state.alive,
                won:   state.won,
                tick:  state.tick,
                timeLeft: state.timeLeft,
                prevJumpHeld: state.prevJumpHeld,
                hasWeapon: state.hasWeapon,
                pickupCollected: state.pickupCollected,
                weaponCooldown: state.weaponCooldown,
                pixelsDestroyed: state.pixelsDestroyed,
                beamStompKillsTotal: state.beamStompKillsTotal,
                beamFlyerKillsTotal: state.beamFlyerKillsTotal,
                phase: state.phase,
                prevPhi: state.prevPhi,
                stallBestX: state.stallBestX,
                stallSince: state.stallSince,
                stalledOut: state.stalledOut,
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
            const sLen = snap.stompers.length;
            state.stompers.length = sLen;
            for (let i = 0; i < sLen; i++) state.stompers[i] = { ...snap.stompers[i] };
            const fLen = snap.flyers ? snap.flyers.length : 0;
            state.flyers.length = fLen;
            for (let i = 0; i < fLen; i++) state.flyers[i] = { ...snap.flyers[i] };
            tilemap.applyDamageDiff(snap.damageDiff || null);
            state.score = snap.score;
            state.alive = snap.alive;
            state.won   = snap.won;
            state.tick  = snap.tick;
            state.timeLeft = snap.timeLeft;
            state.prevJumpHeld = snap.prevJumpHeld;
            state.hasWeapon = !!snap.hasWeapon;
            state.pickupCollected = !!snap.pickupCollected;
            state.weaponCooldown = snap.weaponCooldown | 0;
            state.pixelsDestroyed = snap.pixelsDestroyed | 0;
            state.beamStompKillsTotal = snap.beamStompKillsTotal | 0;
            state.beamFlyerKillsTotal = snap.beamFlyerKillsTotal | 0;
            state.phase = snap.phase | 0;
            state.prevPhi = snap.prevPhi != null ? snap.prevPhi : computePhi();
            state.stallBestX = snap.stallBestX != null ? snap.stallBestX : state.player.x;
            state.stallSince = snap.stallSince != null ? snap.stallSince : 0;
            state.stalledOut = !!snap.stalledOut;
        }

        // ── Beam fire (called per physics tick during a fire decision) ─────
        function fireOneBeam(angleRel) {
            const p = state.player;
            const facing = p.facing < 0 ? -1 : 1;
            // angleRel ∈ [ARC_THETA0, ARC_THETA1]; horizontal direction
            // mirrors with facing. Up = negative y, so keep angle as-is for
            // y component (cos/sin convention).
            const ux = Math.cos(angleRel) * facing;
            const uy = Math.sin(angleRel);
            const px = p.x + p.w / 2;
            const py = p.y + p.h / 2;
            const startOff = p.w / 2 + 2;
            const x0 = px + ux * startOff;
            const y0 = py + uy * startOff;
            const x1 = px + ux * BEAM_LENGTH;
            const y1 = py + uy * BEAM_LENGTH;
            const r = tilemap.damageBeam(x0, y0, x1, y1, BEAM_THICKNESS, true);
            const hx = r.hitX, hy = r.hitY;
            const explosionR = r.hit ? EXPLOSION_R : 0;
            let cleared = r.cleared | 0;
            if (explosionR > 0) cleared += tilemap.damageCircle(hx, hy, explosionR) | 0;
            const half = BEAM_THICKNESS / 2 + 2;
            let stompKills = 0, flyerKills = 0;
            for (const s of state.stompers) {
                if (!s.alive) continue;
                if (entityHitBeam(s, x0, y0, hx, hy, half, hx, hy, explosionR)) {
                    s.alive = false; s.squashTimer = 350; stompKills++;
                }
            }
            for (const f of state.flyers) {
                if (!f.alive) continue;
                if (entityHitBeam(f, x0, y0, hx, hy, half, hx, hy, explosionR)) {
                    f.alive = false; flyerKills++;
                }
            }
            return { cleared, stompKills, flyerKills };
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
            if (state.player.y > tilemap.heightPx + 64) ev.killed = true;
            return ev;
        }

        // Pickup overlap test (player AABB vs pickup AABB).
        function checkPickup() {
            if (!pickup || state.pickupCollected) return false;
            const p = state.player;
            if (p.x + p.w <= pickup.x || p.x >= pickup.x + pickup.w) return false;
            if (p.y + p.h <= pickup.y || p.y >= pickup.y + pickup.h) return false;
            state.pickupCollected = true;
            state.hasWeapon = true;
            return true;
        }

        // Public step(action). Advances FRAME_SKIP ticks. Returns {reward, done}.
        function step(action) {
            if (!state.alive || state.won) return { reward: 0, done: true };

            const isFire = (action === 6) && state.hasWeapon && state.weaponCooldown <= 0;
            let stompKills = 0;
            let died  = false;
            let won   = false;
            let pickupHit = false;
            let pixelsThis = 0;
            let beamStomps = 0;
            let beamFlyers = 0;

            for (let t = 0; t < FRAME_SKIP; t++) {
                const input = actionToInput(isFire ? 0 : action, t === 0, state.prevJumpHeld);
                const ev = tickPhysics(input, FIXED_DT_MS);
                state.prevJumpHeld = !!input.jumpHeld;
                state.tick++;
                state.timeLeft -= FIXED_DT_MS / 1000;
                stompKills += ev.kills | 0;
                if (ev.killed) { died = true; break; }
                if (isFire) {
                    // Sweep angle: linearly interpolated across the FRAME_SKIP
                    // ticks. With 4 ticks the four shots cover the full arc.
                    const u = FRAME_SKIP > 1 ? (t / (FRAME_SKIP - 1)) : 0.5;
                    const ang = ARC_THETA0 + (ARC_THETA1 - ARC_THETA0) * u;
                    const r = fireOneBeam(ang);
                    pixelsThis += r.cleared;
                    beamStomps += r.stompKills;
                    beamFlyers += r.flyerKills;
                }
                if (checkPickup()) pickupHit = true;
                if (flag && !state.won) {
                    const p = state.player;
                    if (p.x + p.w >= flag.x + 8 && p.x <= flag.x + flag.w - 8) {
                        state.won = true; won = true; break;
                    }
                }
                if (state.timeLeft <= 0) break;
            }

            state.pixelsDestroyed += pixelsThis;
            state.beamStompKillsTotal += beamStomps;
            state.beamFlyerKillsTotal += beamFlyers;
            if (isFire) state.weaponCooldown = WEAPON_COOLDOWN_DECISIONS;
            else if (state.weaponCooldown > 0) state.weaponCooldown--;

            // Phase + potential-based shaping bonus (γΦ' − Φ).
            recomputePhase();
            const phi = computePhi();
            const shapingBonus = PBRS_GAMMA * phi - state.prevPhi;
            state.prevPhi = phi;

            let reward = REW_PER_PIXEL * pixelsThis
                       + REW_STOMP * stompKills
                       + REW_BEAM_STOMP * beamStomps
                       + REW_BEAM_FLYER * beamFlyers
                       + (pickupHit ? REW_PICKUP : 0)
                       + shapingBonus;

            // Stall: re-using the inline detector (snapshot/restore-friendly).
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
            state.score += stompKills * 100 + beamStomps * 100 + beamFlyers * 200
                        + Math.floor(pixelsThis * 0.05)
                        + (pickupHit ? 300 : 0)
                        + (won ? 1000 : 0);
            return { reward, done };
        }

        // All 7 actions are technically legal; fire becomes a no-op pre-pickup
        // or while cooling down, but MCTS still benefits from seeing it as a
        // legal option (the policy net learns when it's worth a no-op slot).
        const _legal = new Int32Array([0, 1, 2, 3, 4, 5, 6]);
        function legalActions() { return _legal; }

        return {
            tilemap,
            flag,
            pickup,
            get player()         { return state.player; },
            get stompers()       { return state.stompers; },
            get flyers()         { return state.flyers; },
            get score()          { return state.score; },
            get alive()          { return state.alive; },
            get won()            { return state.won; },
            get tick()           { return state.tick; },
            get timeLeft()       { return state.timeLeft; },
            get stalledOut()     { return state.stalledOut; },
            get hasWeapon()      { return state.hasWeapon; },
            get pickupCollected(){ return state.pickupCollected; },
            get weaponCooldown() { return state.weaponCooldown; },
            get pixelsDestroyed(){ return state.pixelsDestroyed; },
            get phase()          { return state.phase; },
            tile: TILE,
            frameSkip: FRAME_SKIP,
            numActions: 7,
            reset, snapshot, restore, step, legalActions, setSpawn,
        };
    }

    global.SwSim = {
        create,
        TILE, FIXED_DT_MS, FRAME_SKIP,
        REW_PER_PIXEL, REW_STOMP, REW_BEAM_STOMP, REW_BEAM_FLYER,
        REW_PICKUP, REW_FLAG, REW_DEATH, REW_TIMEOUT,
        ARC_THETA0, ARC_THETA1, BEAM_LENGTH, BEAM_THICKNESS, EXPLOSION_R,
        WEAPON_COOLDOWN_DECISIONS,
    };
})(typeof window !== 'undefined' ? window : globalThis);
