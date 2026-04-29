// bc_warmup.js — behavioral-cloning warm start for the stompworld agent.
//
// Generates demonstration episodes from a hand-coded heuristic and pushes
// (obs, one-hot policy, discounted return) tuples directly into the
// agent's replay buffer. Breaks the cold-start trap where a randomly
// initialized policy never stumbles into reward and so learns "stand
// still" as the optimum.
//
// The heuristic reads sim state directly (player, tilemap, mobs, pickup,
// weapon flags). Behavior:
//
//   Phase 0 (no weapon):   walk right toward the pickup; jump pits, walls,
//                          and stompers; jump over body-level flyers; avoid
//                          jumping into apex-lethal flyers.
//   Phase 1 (has weapon, not yet cleared):
//                          fire on close flyers and obstructing destructible
//                          walls; otherwise walk toward the spawn (left).
//   Phase 2 (cleared):     walk right toward the flag; opportunistic fire.
//
// We do NOT clone exactly — the agent's replay buffer just needs a few
// good trajectories that beat "stand still" numerically.

(function (global) {
    'use strict';

    const TILE = 32;

    function tileSolid(sim, c, r) {
        const tm = sim.tilemap;
        if (c < 0 || c >= tm.cols || r < 0 || r >= tm.rows) return false;
        return !!tm.solidAt(c, r);
    }

    function tileDestructible(sim, c, r) {
        const tm = sim.tilemap;
        if (c < 0 || c >= tm.cols || r < 0 || r >= tm.rows) return false;
        const id = tm.data[r * tm.cols + c];
        return id !== 0 && id !== 1 && tm.solidAt(c, r);
    }

    function nearestStomperAhead(sim, dir) {
        const p = sim.player;
        const px = p.x + p.w / 2;
        let best = null;
        for (const s of sim.stompers) {
            if (!s.alive) continue;
            const dx = (s.x + s.w / 2) - px;
            if (dx * dir <= 0) continue;
            if (Math.abs(dx) > 250) continue;
            if (Math.abs((s.y + s.h / 2) - (p.y + p.h / 2)) > 100) continue;
            if (!best || Math.abs(dx) < Math.abs(best.dx)) best = { s, dx };
        }
        return best;
    }

    function classifyFlyersAhead(sim, dir) {
        // Returns { bodyLevel, apexLethal, beamTargetable } booleans.
        // bodyLevel: needs jumping over (row 15 / dy ≈ 0).
        // apexLethal: lethal at jump apex (row 11 / dy ≈ -100..-200).
        // beamTargetable: alive flyer in front, anywhere within ~5 tiles.
        const p = sim.player;
        const pcx = p.x + p.w / 2;
        const pcy = p.y + p.h / 2;
        let bodyLevel = false, apexLethal = false, beamTargetable = false;
        for (const f of sim.flyers) {
            if (!f.alive) continue;
            const dx = (f.x + f.w / 2) - pcx;
            const dy = (f.y + f.h / 2) - pcy;
            if (dx * dir <= 0) continue;
            const adx = Math.abs(dx);
            if (adx < 175) {
                if (Math.abs(dy) < 30) bodyLevel = true;
                else if (dy > -200 && dy < -50) apexLethal = true;
            }
            if (adx < 200) beamTargetable = true;
        }
        return { bodyLevel, apexLethal, beamTargetable };
    }

    function destructibleWallAhead(sim, dir) {
        // Look 1-2 cols ahead at the player's row for a destructible wall.
        const p = sim.player;
        const pCol = Math.floor((p.x + p.w / 2) / TILE);
        const pRow = Math.floor((p.y + p.h / 2) / TILE);
        for (let dc = 1; dc <= 2; dc++) {
            const c = pCol + dc * dir;
            if (tileDestructible(sim, c, pRow) ||
                tileDestructible(sim, c, pRow - 1)) return true;
        }
        return false;
    }

    function pitAhead(sim, dir) {
        // 1 tile ahead at the row directly under the player's feet.
        const p = sim.player;
        const pCol = Math.floor((p.x + p.w / 2) / TILE);
        const footRow = Math.floor((p.y + p.h + 2) / TILE);
        return !tileSolid(sim, pCol + dir, footRow);
    }

    function wallAhead(sim, dir) {
        const p = sim.player;
        const pCol = Math.floor((p.x + p.w / 2) / TILE);
        const pRow = Math.floor((p.y + p.h / 2) / TILE);
        return tileSolid(sim, pCol + dir, pRow);
    }

    function headBlock(sim, dir) {
        const p = sim.player;
        const pCol = Math.floor((p.x + p.w / 2) / TILE);
        const pRow = Math.floor((p.y + p.h / 2) / TILE);
        return tileSolid(sim, pCol + dir, pRow - 1);
    }

    function chooseDirection(sim) {
        // Direction the heuristic wants to move: right toward pickup pre-
        // collection, left toward spawn after pickup until destruction is
        // "enough", then right toward flag.
        if (sim.phase === 0) return 1;
        if (sim.phase === 2) return 1;
        return -1;
    }

    // Movement actions canonicalize to (h0, 0, 0) → flat = h0 * 117. Fire
    // picks a target tile from the nearest live enemy (or 1-tile-ahead
    // along facing for terrain) and encodes (6, h1, h2). Returns a flat
    // action index in [0, 819).
    const STRIDE_H0 = 13 * 9;           // mirrors sim.js
    const STRIDE_H1 = 9;
    const ACT_FIRE  = 6;
    const FIRE_COL_CENTER = 6;
    const FIRE_ROW_CENTER = 4;

    function moveFlat(h0) { return h0 * STRIDE_H0; }

    function pickFireFlat(sim) {
        // Aim at nearest live enemy; fall back to a 1-tile-forward target
        // when none are nearby (carves terrain along facing).
        const p = sim.player;
        const px = p.x + p.w / 2;
        const py = p.y + p.h / 2;
        const pCol = Math.floor(px / TILE);
        const pRow = Math.floor(py / TILE);
        let best = null;
        const consider = (e) => {
            if (!e.alive) return;
            const dx = (e.x + e.w / 2) - px;
            const dy = (e.y + e.h / 2) - py;
            const d2 = dx * dx + dy * dy;
            if (!best || d2 < best.d2) {
                const tCol = Math.floor((e.x + e.w / 2) / TILE);
                const tRow = Math.floor((e.y + e.h / 2) / TILE);
                best = { d2, dCol: tCol - pCol, dRow: tRow - pRow };
            }
        };
        for (const s of sim.stompers) consider(s);
        for (const f of sim.flyers) consider(f);
        let dCol, dRow;
        if (best) { dCol = best.dCol; dRow = best.dRow; }
        else      { dCol = (p.facing < 0 ? -1 : 1); dRow = 0; }
        // Clamp to head ranges (h1 ∈ [-6,+6], h2 ∈ [-4,+4]).
        if (dCol < -6) dCol = -6; else if (dCol > 6) dCol = 6;
        if (dRow < -4) dRow = -4; else if (dRow > 4) dRow = 4;
        const h1 = dCol + FIRE_COL_CENTER;
        const h2 = dRow + FIRE_ROW_CENTER;
        return ACT_FIRE * STRIDE_H0 + h1 * STRIDE_H1 + h2;
    }

    function heuristicAction(sim, rng) {
        const p = sim.player;
        const onGround = !!p.onGround;
        const coyoteHot = (p.coyote || 0) > 10;
        const dir = chooseDirection(sim);
        const facing = p.facing < 0 ? -1 : 1;

        // Tiny exploration noise — uniform over the 6 movement actions only
        // (firing requires aim and isn't a useful random exploration).
        const r = rng();
        if (r < 0.02) return moveFlat((rng() * 6) | 0);

        // Beam logic. Fire when (a) we have the weapon, (b) cooldown is up,
        // and (c) there's something worth shooting in our facing arc.
        if (sim.hasWeapon && sim.weaponCooldown <= 0) {
            const flyers = classifyFlyersAhead(sim, facing);
            const wallDestr = destructibleWallAhead(sim, facing);
            if (flyers.beamTargetable || (wallDestr && onGround)) return pickFireFlat(sim);
        }

        // Movement target obstacles in the chosen direction.
        const stomper = nearestStomperAhead(sim, dir);
        const stomperClose = stomper && Math.abs(stomper.dx) < 90;
        const flyersDir = classifyFlyersAhead(sim, dir);

        const wAhead = wallAhead(sim, dir);
        const hBlock = headBlock(sim, dir);
        const pAhead = pitAhead(sim, dir);

        // Body-level flyer: must jump or die.
        if (flyersDir.bodyLevel && (onGround || coyoteHot)) {
            return moveFlat(dir > 0 ? 5 : 4);
        }

        const triggerJump = wAhead || hBlock || pAhead || stomperClose;

        if (flyersDir.apexLethal && triggerJump && onGround) {
            if (!pAhead) return moveFlat(dir > 0 ? 2 : 1);
        }
        if (flyersDir.apexLethal && !triggerJump) return moveFlat(dir > 0 ? 2 : 1);

        if (triggerJump && (onGround || coyoteHot)) return moveFlat(dir > 0 ? 5 : 4);

        // Hold the jump while rising (avoid jumpCutMul).
        if (!onGround && p.vy < -50) return moveFlat(dir > 0 ? 5 : 4);

        return moveFlat(dir > 0 ? 2 : 1);
    }

    // Run one heuristic episode end-to-end. Returns an array of pending
    // tuples {obs, policyTarget, mask, reward}; caller seals with discounted
    // returns and pushes into the buffer. Caller sets the spawn before
    // calling.
    function rollOne(sim, rng, maxDecisions) {
        sim.reset();
        const out = [];
        const HEAD_SIZES   = SwSim.HEAD_SIZES;
        const HEAD_OFFSETS = SwSim.HEAD_OFFSETS;
        const PER_HEAD_TOTAL = SwSim.PER_HEAD_TOTAL;
        // Per-head one-hot target. The trainer's per-head softmax-xent
        // sees three independent distributions concatenated; an empty
        // mask is treated as "all legal" by the trainer. We emit the
        // 29-dim target directly to avoid the trainer having to
        // marginalize on every push.
        const mask = new Float32Array(0);

        for (let t = 0; t < maxDecisions; t++) {
            const obs = SwAgentObs.build(sim).slice();
            const flat = heuristicAction(sim, rng);
            const dec  = SwSim.decodeFlat(flat);
            const target = new Float32Array(PER_HEAD_TOTAL);
            target[HEAD_OFFSETS[0] + dec[0]] = 1.0;
            target[HEAD_OFFSETS[1] + dec[1]] = 1.0;
            target[HEAD_OFFSETS[2] + dec[2]] = 1.0;
            const r = sim.step(flat);
            out.push({ obs, policyTarget: target, mask, reward: r.reward });
            if (r.done) break;
        }
        return out;
    }

    function populate(agent, sim, opts) {
        opts = opts || {};
        const targetSamples   = opts.targetSamples   || 30;
        const maxAttempts     = opts.maxAttempts     || 200;
        const gamma           = opts.gamma           || 0.99;
        const maxDecisions    = opts.maxDecisions    || 400;
        const minReward       = opts.minReward       != null ? opts.minReward : 0.2;
        let seed = (opts.seed >>> 0) || 0xBC51A57E;
        function rng() {
            seed = (seed + 0x6D2B79F5) >>> 0;
            let t = seed;
            t = Math.imul(t ^ (t >>> 15), t | 1);
            t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
            return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
        }

        if (opts.spawnX != null) sim.setSpawn(opts.spawnX, opts.spawnY);

        const buf = agent.buffer;
        let pushed = 0, flags = 0, deaths = 0, timeouts = 0, kept = 0, attempts = 0;
        let totalReward = 0;

        while (kept < targetSamples && attempts < maxAttempts) {
            attempts++;
            const tuples = rollOne(sim, rng, maxDecisions);
            const won = sim.won;
            if (won) flags++;
            else if (sim.timeLeft <= 0) timeouts++;
            else deaths++;

            let epReward = 0;
            for (const t of tuples) epReward += t.reward;
            totalReward += epReward;

            if (!won && epReward < minReward) continue;
            kept++;

            let g = 0;
            for (let i = tuples.length - 1; i >= 0; i--) {
                g = tuples[i].reward + gamma * g;
                const v = g < -1 ? -1 : (g > 1 ? 1 : g);
                buf.push({
                    obs: tuples[i].obs,
                    policyTarget: tuples[i].policyTarget,
                    actionMask: tuples[i].mask,
                    valueTarget: v,
                });
                pushed++;
            }
        }

        return {
            attempts, kept, flags, deaths, timeouts,
            tuplesPushed: pushed,
            avgEpisodeReward: attempts > 0 ? totalReward / attempts : 0,
        };
    }

    global.SwBcWarmup = { populate, heuristicAction };
})(typeof window !== 'undefined' ? window : globalThis);
