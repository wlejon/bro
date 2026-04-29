// bc_warmup.js — behavioral-cloning warm start for the stompworld agent.
//
// Generates demonstration episodes from a hand-coded heuristic and pushes
// (obs, one-hot policy, discounted return) tuples directly into the
// agent's replay buffer. Breaks the cold-start trap where a randomly
// initialized policy never stumbles into reward and so learns "stand
// still" as the optimum.
//
// Movement-only heuristic: walk right toward the flag; jump pits, walls,
// stompers, body-level flyers; avoid jumping into apex-lethal flyers.
// Auto-fire is scripted in sim.js, so the heuristic does not pick fire
// actions — terrain clearing happens automatically once hasWeapon is true.

(function (global) {
    'use strict';

    const TILE = 32;

    function tileSolid(sim, c, r) {
        const tm = sim.tilemap;
        if (c < 0 || c >= tm.cols || r < 0 || r >= tm.rows) return false;
        return !!tm.solidAt(c, r);
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
        // bodyLevel: needs jumping over.
        // apexLethal: lethal at jump apex — avoid jumping near it.
        const p = sim.player;
        const pcx = p.x + p.w / 2;
        const pcy = p.y + p.h / 2;
        let bodyLevel = false, apexLethal = false;
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
        }
        return { bodyLevel, apexLethal };
    }

    function pitAhead(sim, dir) {
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

    function heuristicAction(sim, rng) {
        const p = sim.player;
        const onGround = !!p.onGround;
        const coyoteHot = (p.coyote || 0) > 10;
        const dir = 1;   // always toward the flag

        // Tiny exploration noise.
        const r = rng();
        if (r < 0.02) return (rng() * 6) | 0;

        const stomper = nearestStomperAhead(sim, dir);
        const stomperClose = stomper && Math.abs(stomper.dx) < 90;
        const flyersDir = classifyFlyersAhead(sim, dir);

        const wAhead = wallAhead(sim, dir);
        const hBlock = headBlock(sim, dir);
        const pAhead = pitAhead(sim, dir);

        // Body-level flyer: must jump or die.
        if (flyersDir.bodyLevel && (onGround || coyoteHot)) {
            return dir > 0 ? 5 : 4;
        }

        const triggerJump = wAhead || hBlock || pAhead || stomperClose;

        if (flyersDir.apexLethal && triggerJump && onGround) {
            if (!pAhead) return dir > 0 ? 2 : 1;
        }
        if (flyersDir.apexLethal && !triggerJump) return dir > 0 ? 2 : 1;

        if (triggerJump && (onGround || coyoteHot)) return dir > 0 ? 5 : 4;

        // Hold the jump while rising (avoid jumpCutMul).
        if (!onGround && p.vy < -50) return dir > 0 ? 5 : 4;

        return dir > 0 ? 2 : 1;
    }

    function rollOne(sim, rng, maxDecisions) {
        sim.reset();
        const out = [];
        const NUM_ACTIONS = SwSim.FLAT_NUM_ACTIONS;
        const mask = new Float32Array(0);

        for (let t = 0; t < maxDecisions; t++) {
            const obs = SwAgentObs.build(sim).slice();
            const a = heuristicAction(sim, rng);
            const target = new Float32Array(NUM_ACTIONS);
            target[a] = 1.0;
            const r = sim.step(a);
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
