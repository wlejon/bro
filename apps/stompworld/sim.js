// sim.js — headless, snapshot/restorable wrapper around the stompworld
// physics. Used both by the live game (when driven by an AI agent) and as
// the MCTS rollout substrate.
//
// Action space — single head, 6 movement actions:
//   0 idle, 1 left, 2 right, 3 jump, 4 jump-left, 5 jump-right
//
// Aiming is NOT a learned action. The agent chooses movement only. After
// each decision's physics ticks complete, the sim runs a scripted auto-fire
// pass: if hasWeapon and weaponCooldown <= 0, and there's either a live
// enemy whose center is within AUTO_FIRE_RANGE_PX or a destructible tile
// blocking the player's facing direction within ~3 tiles, fire one beam.
// Targeting picks the nearest enemy in range; otherwise fires straight
// along facing.
//
// One env.step(action) advances FRAME_SKIP physics ticks at FIXED_DT_MS each.
// jumpHeld stays true across ticks for jump actions and jumpPressed pulses
// on the first tick (matches Platformer's tap semantics).
//
// Pickup: a one-shot pickup tile at level col 115 grants the beam weapon
// when the player AABB first overlaps it.
//
// Rewards (per env.step):
//   + REW_PER_PIXEL  * pixelsCleared       terrain destroyed by auto-fire
//   + REW_STOMP      * jumpKills           stompers killed by jumping on them
//   + REW_BEAM_STOMP * beamStompKills      stompers killed by auto-fire
//   + REW_BEAM_FLYER * beamFlyerKills      flyers killed by auto-fire
//   + REW_PICKUP                           on pickup overlap (transient)
//   + REW_FLAG                             on flag    (terminal)
//   + REW_DEATH                            on death   (terminal)
//   + REW_TIMEOUT                          on timeout (terminal)
//   + (γ·Φ' − Φ)                           potential-based shaping bonus
//
// Single-phase potential Φ = −dist(player, flag) × PBRS_SCALE — one linear
// gradient end-to-end. The pickup is an event reward + auto-fire enabler;
// no backtrack arc.
//
// done becomes true on death, flag, timeout, or stall.

(function (global) {
    'use strict';

    const TILE         = 32;
    const FIXED_DT_MS  = 1000 / 60;     // 16.67 ms — matches the live engine
    const FRAME_SKIP   = 4;             // physics ticks per decision (~67 ms)

    // Single-head movement-only action space.
    const HEAD_SIZES        = [6];
    const HEAD_OFFSETS      = [0, 6];
    const PER_HEAD_TOTAL    = 6;
    const FLAT_NUM_ACTIONS  = 6;

    // Player stomp impulse; mirrors app.js handleStompers.
    const STOMP_BOUNCE_VY = -380;

    // Reward shaping.
    const REW_PER_PIXEL  = 0.005;
    const REW_STOMP      = 1.0;
    const REW_BEAM_STOMP = 1.0;
    const REW_BEAM_FLYER = 10.0;
    const REW_PICKUP     = 3.0;
    const REW_FLAG       = 1.5;
    const REW_DEATH      = -0.5;
    const REW_TIMEOUT    = -0.3;

    const PBRS_GAMMA = 0.99;
    const PBRS_SCALE = 0.01;

    const STOMP_GRAVITY  = 1800;
    const STOMP_MAX_FALL = 800;

    // Beam.
    const BEAM_LENGTH    = 600;
    const BEAM_THICKNESS = 8;
    const EXPLOSION_R    = 56;
    const WEAPON_COOLDOWN_DECISIONS = 4;          // ~270 ms between shots

    // Auto-fire tuning. Range is "any live enemy center within this radius
    // of the player center triggers a shot". A bit larger than one screen-
    // height so the agent can pick off flyers a couple tiles ahead.
    const AUTO_FIRE_RANGE_PX = 240;
    // How many tiles ahead we look for blocking destructible terrain when
    // there's no enemy in range. Short — we only fire to clear obstacles
    // we'd actually run into.
    const AUTO_FIRE_TERRAIN_LOOKAHEAD = 3;

    function actionToInput(a, isFirstTick, prevJumpHeld) {
        const left  = a === 1 || a === 4;
        const right = a === 2 || a === 5;
        const jump  = a === 3 || a === 4 || a === 5;
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

    // ── Beam hit test (AABB vs segment + circle, mirrors app.js entityHit) ─
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
        const stallEpsilonPx   = level.stallEpsilonPx   != null ? level.stallEpsilonPx   : 8;
        const FREE_BACKWALK_PX = level.freeBackwalkPx   != null ? level.freeBackwalkPx   : 160;

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
            prevJumpHeld: false,

            hasWeapon: false,
            pickupCollected: false,
            weaponCooldown: 0,
            pixelsDestroyed: 0,
            beamStompKillsTotal: 0,
            beamFlyerKillsTotal: 0,

            recentBeams: [],

            prevPhi: 0,

            stallBestScore: 0,
            stallSince: 0,
            stalledOut: false,
            peakX: 0,
        };

        function distToFlag(p) {
            if (!flag) return 0;
            const ax = p.x + p.w / 2, ay = p.y + p.h / 2;
            const bx = flag.x + flag.w / 2, by = flag.y + flag.h / 2;
            return (Math.abs(ax - bx) + Math.abs(ay - by)) / TILE;
        }
        function computePhi() {
            return -distToFlag(state.player) * PBRS_SCALE;
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
            state.recentBeams.length = 0;
            tilemap.resetDamage();
            state.prevPhi = computePhi();
            state.peakX = state.player.x;
            state.stallBestScore = state.player.x + state.score;
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
                prevPhi: state.prevPhi,
                stallBestScore: state.stallBestScore,
                stallSince: state.stallSince,
                stalledOut: state.stalledOut,
                peakX: state.peakX,
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
            state.prevPhi = snap.prevPhi != null ? snap.prevPhi : computePhi();
            state.stallBestScore = snap.stallBestScore != null
                ? snap.stallBestScore
                : (state.player.x + state.score);
            state.stallSince = snap.stallSince != null ? snap.stallSince : 0;
            state.peakX = snap.peakX != null ? snap.peakX : state.player.x;
            state.stalledOut = !!snap.stalledOut;
        }

        // Pick the live enemy whose center is nearest the player center,
        // within AUTO_FIRE_RANGE_PX. Returns {ux, uy} unit aim direction
        // or null if no target.
        function pickAutoTarget() {
            const p = state.player;
            const px = p.x + p.w * 0.5;
            const py = p.y + p.h * 0.5;
            const r2 = AUTO_FIRE_RANGE_PX * AUTO_FIRE_RANGE_PX;
            let bestD2 = Infinity;
            let bestDx = 0, bestDy = 0;
            for (const s of state.stompers) {
                if (!s.alive) continue;
                const dx = (s.x + s.w * 0.5) - px;
                const dy = (s.y + s.h * 0.5) - py;
                const d2 = dx * dx + dy * dy;
                if (d2 < bestD2 && d2 <= r2) { bestD2 = d2; bestDx = dx; bestDy = dy; }
            }
            for (const f of state.flyers) {
                if (!f.alive) continue;
                const dx = (f.x + f.w * 0.5) - px;
                const dy = (f.y + f.h * 0.5) - py;
                const d2 = dx * dx + dy * dy;
                if (d2 < bestD2 && d2 <= r2) { bestD2 = d2; bestDx = dx; bestDy = dy; }
            }
            if (bestD2 === Infinity) return null;
            const d = Math.sqrt(bestD2) || 1;
            return { ux: bestDx / d, uy: bestDy / d };
        }

        // Is there a destructible tile blocking the player's facing
        // direction within AUTO_FIRE_TERRAIN_LOOKAHEAD tiles? Looks at
        // the player's row and one row above (head-block).
        function destructibleAhead() {
            const p = state.player;
            const dir = p.facing < 0 ? -1 : 1;
            const pCol = Math.floor((p.x + p.w / 2) / TILE);
            const pRow = Math.floor((p.y + p.h / 2) / TILE);
            const tm = tilemap;
            for (let dc = 1; dc <= AUTO_FIRE_TERRAIN_LOOKAHEAD; dc++) {
                const c = pCol + dc * dir;
                if (c < 0 || c >= tm.cols) return false;
                for (const r of [pRow, pRow - 1]) {
                    if (r < 0 || r >= tm.rows) continue;
                    if (!tm.solidAt(c, r)) continue;
                    const id = tm.data[r * tm.cols + c];
                    if (id !== 0 && id !== 1) return true;   // ground (1) is indestructible
                }
            }
            return false;
        }

        // Fire one beam along (ux, uy). Mirrors fireWeapon in app.js, but
        // uses the tilemap overlay path (no bitmask carve until commitOverlays).
        function fireOneBeam(ux, uy) {
            const p = state.player;
            if (ux > 0) p.facing = 1;
            else if (ux < 0) p.facing = -1;
            const px = p.x + p.w / 2;
            const py = p.y + p.h / 2;
            const startOff = p.w / 2 + 2;
            const x0 = px + ux * startOff;
            const y0 = py + uy * startOff;
            const x1 = px + ux * BEAM_LENGTH;
            const y1 = py + uy * BEAM_LENGTH;
            const r = tilemap.traceBeam(x0, y0, x1, y1);
            const hx = r.hitX, hy = r.hitY;
            tilemap.pushOverlayBeam(x0, y0, hx, hy, BEAM_THICKNESS);
            const explosionR = r.hit ? EXPLOSION_R : 0;
            if (explosionR > 0) tilemap.pushOverlayCircle(hx, hy, explosionR);
            let cleared = (r.len * BEAM_THICKNESS) | 0;
            if (explosionR > 0) cleared += (Math.PI * explosionR * explosionR) | 0;
            state.recentBeams.push({ x0, y0, x1: hx, y1: hy });
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

        // Try the auto-fire pass once per decision (post-physics). Returns
        // {cleared, stompKills, flyerKills} (zeroed if no shot taken).
        function autoFire() {
            if (!state.hasWeapon || state.weaponCooldown > 0) {
                return { cleared: 0, stompKills: 0, flyerKills: 0, fired: false };
            }
            const target = pickAutoTarget();
            let dir = null;
            if (target) {
                dir = target;
            } else if (destructibleAhead()) {
                const f = state.player.facing < 0 ? -1 : 1;
                dir = { ux: f, uy: 0 };
            }
            if (!dir) return { cleared: 0, stompKills: 0, flyerKills: 0, fired: false };
            const r = fireOneBeam(dir.ux, dir.uy);
            state.weaponCooldown = WEAPON_COOLDOWN_DECISIONS;
            return { ...r, fired: true };
        }

        function runPhysicsStep(input, dt) {
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

        function checkPickup() {
            if (!pickup || state.pickupCollected) return false;
            const p = state.player;
            if (p.x + p.w <= pickup.x || p.x >= pickup.x + pickup.w) return false;
            if (p.y + p.h <= pickup.y || p.y >= pickup.y + pickup.h) return false;
            state.pickupCollected = true;
            state.hasWeapon = true;
            return true;
        }

        // Per-decision accumulators. Set up by beginDecision(), consumed by
        // endDecision(). Tracked outside `state` so they don't pollute the
        // snapshot — they're only meaningful between begin/end and a caller
        // that snapshots mid-decision is misusing the API.
        let curStompKills = 0;
        let curDied = false;
        let curWon  = false;
        let curPickupHit = false;
        let curOpen = false;

        // Open a new decision window. Call once before the first tickPhysics
        // for this decision. Resets per-decision state and the recent-beam
        // log (auto-fire in endDecision will repopulate it).
        function beginDecision() {
            state.recentBeams.length = 0;
            curStompKills = 0;
            curDied = false;
            curWon = false;
            curPickupHit = false;
            curOpen = true;
        }

        // Advance one physics tick (FIXED_DT_MS = 16.67 ms). `tickIdx` is
        // 0..FRAME_SKIP-1 within the current decision; the first tick pulses
        // jumpPressed (matching Platformer's tap semantics). Returns true if
        // this tick ended the decision early (death / flag / timeout) — the
        // caller should stop ticking and call endDecision().
        function tickPhysics(action, tickIdx) {
            if (!curOpen) return true;
            if (!state.alive || state.won) { curDied = !state.alive && !state.won; return true; }
            const a = action | 0;
            const input = actionToInput(a, tickIdx === 0, state.prevJumpHeld);
            const ev = runPhysicsStep(input, FIXED_DT_MS);
            state.prevJumpHeld = !!input.jumpHeld;
            state.tick++;
            state.timeLeft -= FIXED_DT_MS / 1000;
            curStompKills += ev.kills | 0;
            if (ev.killed) { curDied = true; return true; }
            if (checkPickup()) curPickupHit = true;
            if (flag && !state.won) {
                const p = state.player;
                if (p.x + p.w >= flag.x + 8 && p.x <= flag.x + flag.w - 8) {
                    state.won = true; curWon = true; return true;
                }
            }
            if (state.timeLeft <= 0) return true;
            return false;
        }

        // Close the decision: auto-fire pass, reward shaping, score, stall
        // check, terminal flags. Returns {reward, done}.
        function endDecision() {
            if (!curOpen) return { reward: 0, done: !state.alive || state.won };
            curOpen = false;

            const died = curDied;
            const won = curWon;
            const pickupHit = curPickupHit;
            const stompKills = curStompKills;

            let pixelsThis = 0, beamStomps = 0, beamFlyers = 0;
            if (!died && !won) {
                const af = autoFire();
                if (af.fired) {
                    pixelsThis = af.cleared;
                    beamStomps = af.stompKills;
                    beamFlyers = af.flyerKills;
                } else if (state.weaponCooldown > 0) {
                    state.weaponCooldown--;
                }
            }

            state.pixelsDestroyed += pixelsThis;
            state.beamStompKillsTotal += beamStomps;
            state.beamFlyerKillsTotal += beamFlyers;

            const phi = computePhi();
            const shapingBonus = PBRS_GAMMA * phi - state.prevPhi;
            state.prevPhi = phi;

            let reward = REW_PER_PIXEL * pixelsThis
                       + REW_STOMP * stompKills
                       + REW_BEAM_STOMP * beamStomps
                       + REW_BEAM_FLYER * beamFlyers
                       + (pickupHit ? REW_PICKUP : 0)
                       + shapingBonus;

            state.score += stompKills * 100 + beamStomps * 100 + beamFlyers * 200
                        + Math.floor(pixelsThis * 0.05)
                        + (pickupHit ? 300 : 0)
                        + (won ? 1000 : 0);

            if (state.player.x > state.peakX) state.peakX = state.player.x;
            const inFreeZone = state.player.x >= state.peakX - FREE_BACKWALK_PX;
            const effectiveX = inFreeZone ? state.peakX : state.player.x;
            const stallScore = effectiveX + state.score;
            if (stallScore > state.stallBestScore + stallEpsilonPx) {
                state.stallBestScore = stallScore;
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
            return { reward, done };
        }

        // Public step(action): the original decision-level API. One call =
        // FRAME_SKIP physics ticks + post-physics auto-fire / reward / stall.
        // Used by play_agent and the MCTS data workers. The tick-level API
        // (beginDecision / tickPhysics / endDecision) above lets a renderer
        // step physics one wall-clock tick at a time so the displayed world
        // moves smoothly between decision boundaries instead of jumping in
        // 4-tick chunks.
        function step(action) {
            if (!state.alive || state.won) return { reward: 0, done: true };
            beginDecision();
            for (let t = 0; t < FRAME_SKIP; t++) {
                if (tickPhysics(action, t)) break;
            }
            return endDecision();
        }

        const _legal = (() => {
            const out = new Int32Array(FLAT_NUM_ACTIONS);
            for (let i = 0; i < FLAT_NUM_ACTIONS; i++) out[i] = i;
            return out;
        })();
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
            get recentBeams()    { return state.recentBeams; },
            tile: TILE,
            frameSkip: FRAME_SKIP,
            numActions: FLAT_NUM_ACTIONS,
            headSizes: HEAD_SIZES,
            reset, snapshot, restore, step, legalActions, setSpawn,
            beginDecision, tickPhysics, endDecision,
        };
    }

    global.SwSim = {
        create,
        TILE, FIXED_DT_MS, FRAME_SKIP,
        REW_PER_PIXEL, REW_STOMP, REW_BEAM_STOMP, REW_BEAM_FLYER,
        REW_PICKUP, REW_FLAG, REW_DEATH, REW_TIMEOUT,
        BEAM_LENGTH, BEAM_THICKNESS, EXPLOSION_R,
        WEAPON_COOLDOWN_DECISIONS,
        AUTO_FIRE_RANGE_PX,
        HEAD_SIZES, HEAD_OFFSETS, PER_HEAD_TOTAL, FLAT_NUM_ACTIONS,
    };
})(typeof window !== 'undefined' ? window : globalThis);
