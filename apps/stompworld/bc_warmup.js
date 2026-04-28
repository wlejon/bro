// bc_warmup.js — behavioral-cloning warm start for the stompworld agent.
//
// Generates demonstration episodes from a hand-coded heuristic ("walk right;
// jump over walls, pits, and stompers") and pushes (obs, one-hot policy,
// discounted return) tuples directly into the agent's replay buffer. This
// breaks the cold-start trap where a randomly initialized policy never
// stumbles into the +5 flag reward and so learns "stand still" as the
// optimum. The net pre-trains on these tuples on the trainer's first
// stepN() call after warmup completes.
//
// The heuristic uses only the obs vector (no privileged sim state), so it
// stays a fair upper bound on what the policy could learn from observation
// alone. We do *not* clone the heuristic exactly — we just give the buffer
// enough good trajectories that "move right" stops being numerically
// indistinguishable from "stand still."

(function (global) {
    'use strict';

    const TILE_COLS = 8;     // matches agent_obs forward window
    const TILE_BLOCK_OFF = 8;
    const STOMPER_OFF = TILE_BLOCK_OFF + 40;          // obs[48..62]
    const FLYER_OFF   = STOMPER_OFF + 15;             // obs[63..77]

    // Read a forward-window cell from the obs vector.
    //   dr ∈ [-2, +2], dc ∈ [-1, +6]
    function tileAt(obs, dr, dc) {
        const idx = TILE_BLOCK_OFF + (dr + 2) * TILE_COLS + (dc + 1);
        return obs[idx] > 0.5;
    }

    // Pick a discrete action in [0..5] from an obs vector. Always biases
    // rightward; jumps proactively when an obstacle, pit, or live stomper
    // is within reach in the next few tiles. Lookahead extends to dc=3 —
    // at runSpeed=240 that's ~400 ms of warning, enough margin for the
    // jump arc (~570 ms aloft).
    function heuristicAction(obs, rng) {
        const onGround = obs[2] > 0.5;
        const coyoteHot = obs[4] > 0.1;

        // Trigger jumps only when the obstacle is at the very next tile.
        // Looking ≥2 tiles ahead causes the player to take off too early
        // and land short — at jumpVel=-850 the arc is 170 px (~5.3 tiles)
        // wide, so jumping from 3 tiles before a gap lands you in the
        // middle of it.
        const wallAhead = tileAt(obs, 0, 1);
        const headBlock = tileAt(obs, -1, 1);
        const pitAhead  = !tileAt(obs, 1, 1);

        // Stomper in stomp-trigger window. With jumpVel=-850, runSpeed=240,
        // the player descends back to stomper-top height at ~0.68 s after
        // takeoff. Approach rate (player + stomper) ≈ 290 px/s, so the
        // pre-jump center-to-center dx that lines the descent up with the
        // stomper is ≈ 195 px (dxN ≈ 0.65). AABB overlap allows ±26 px,
        // i.e. dxN ∈ [0.57, 0.74]. We widen the lower bound to ~0 so the
        // heuristic also fires "evade by jumping" when too close to stomp
        // cleanly — being airborne over a passing stomper is always safer
        // than walking into it.
        let stomperClose = false;
        for (let i = 0; i < 3; i++) {
            const off = STOMPER_OFF + i * 5;
            const valid = obs[off] > 0.5;
            const alive = obs[off + 4] > 0.5;
            if (!valid || !alive) continue;
            const dxN = obs[off + 1];   // dx / 300
            const dyN = obs[off + 2];   // dy / 96
            if (dxN > 0 && dxN < 0.75 && Math.abs(dyN) < 0.6) {
                stomperClose = true;
                break;
            }
        }
        // If a pit is between us and the stomper, defer the stomper-jump:
        // jumping from too far back lands us inside the gap. Let pitAhead
        // (dc=1) trigger the jump at the gap edge instead — the same arc
        // clears the gap and lands on/past the stomper.
        const gapBetween = !tileAt(obs, 1, 1) || !tileAt(obs, 1, 2) ||
                           !tileAt(obs, 1, 3) || !tileAt(obs, 1, 4);
        if (stomperClose && gapBetween) stomperClose = false;

        // Tiny exploration noise so demo trajectories don't collapse to one
        // path — but capped low to keep most episodes successful.
        const r = rng();
        if (r < 0.02) return 2 + ((rng() * 4) | 0);   // 2..5

        // Flyer awareness. dy in obs is normalized over 192 px:
        //   dyN ≈   0    → row 15 (body level)         — MUST jump over
        //   dyN ≈ -0.5   → row 12 (apex transit)       — jumping is risky
        //   dyN ≈ -0.65  → row 11 (apex collision)     — jumping is fatal
        // The categories are deliberately wide to also catch bobbing
        // flyers ('X') as they swing through their range.
        let bodyLevelFlyer = false;   // forces a jump-over
        let apexLethalFlyer = false;  // suppresses jumping
        for (let i = 0; i < 3; i++) {
            const off = FLYER_OFF + i * 5;
            if (obs[off] < 0.5) continue;
            const dxN = obs[off + 1];
            const dyN = obs[off + 2];
            // Only consider flyers ahead in close range.
            if (dxN <= 0 || dxN > 0.55) continue;
            if (dyN > -0.18 && dyN < 0.18)             bodyLevelFlyer = true;
            else if (dyN > -0.85 && dyN < -0.30)       apexLethalFlyer = true;
        }
        // Body-level flyer overrides everything: must jump or die. Lead
        // distance is short here (we want the jump fired close to contact
        // so apex aligns over the flyer rather than after it).
        if (bodyLevelFlyer && (onGround || coyoteHot)) return 5;

        const triggerJump = wallAhead || pitAhead || stomperClose || headBlock;

        // Apex-lethal flyer in front: never jump. Walk under it. This is
        // the "anti-jump-spam" lesson the prior must learn — without it,
        // the policy generalizes "JR is always best" and dies at the very
        // first row-11 flyer (col 6).
        if (apexLethalFlyer && triggerJump && onGround) {
            // We'd otherwise jump, but a lethal flyer is in the apex zone.
            // Suppress the jump unless we have no choice (pit ahead = will
            // fall). For pits we accept the risk and jump anyway; flyer
            // patrols are stochastic enough that some attempts succeed.
            if (!pitAhead) return 2;
        }
        if (apexLethalFlyer && !triggerJump) {
            // Belt-and-suspenders: even without an existing trigger, force
            // walk in the apex-lethal zone in case some later check fires.
            return 2;
        }

        if (triggerJump && (onGround || coyoteHot)) return 5; // jump-right

        // Hold the jump while rising. Switching to R while airborne lets
        // jumpHeld go false, which triggers Platformer's jumpCutMul=0.45
        // every tick and slams the apex — fatal for the wider arcs at
        // jumpVel=-850. obs[1] = vy / MAX_FALL; negative = rising.
        const rising = obs[1] < -0.05;
        if (!onGround && rising) return 5;

        return 2;
    }

    // Run one heuristic episode end-to-end. Returns an array of pending
    // tuples {obs, policyTarget, reward}; caller seals with discounted
    // returns and pushes into the buffer. Caller sets the spawn before
    // calling.
    function rollOne(sim, rng, maxDecisions) {
        sim.reset();
        const out = [];
        const numActions = sim.numActions;
        const mask = new Float32Array(numActions);
        for (let i = 0; i < numActions; i++) mask[i] = 1;

        for (let t = 0; t < maxDecisions; t++) {
            const obs = SwAgentObs.build(sim).slice();
            const a = heuristicAction(obs, rng);
            // Hard one-hot: a strong, decisive teacher. The MCTS-derived
            // visit distributions added later will smooth this out
            // naturally; we want the gradient to actually push the prior
            // off uniform during pretraining.
            const target = new Float32Array(numActions);
            target[a] = 1.0;
            const r = sim.step(a);
            out.push({ obs, policyTarget: target, mask, reward: r.reward });
            if (r.done) break;
        }
        return out;
    }

    // Drain heuristic rollouts into `agent.buffer`. We keep "good" episodes
    // — either flag-reaching or with a positive-enough total return —
    // because hard one-hot demos from a partial-success run still teach
    // the apprentice the right local response (jump pits, jump walls,
    // jump stompers). Strict flag-only filtering won't work from spawn 2:
    // the heuristic can't clear the unjumpable 5-tile gap at cols 45–49.
    //
    // The caller picks `spawnX` to point the warmup at a region the
    // heuristic can actually solve (e.g. col 78 — long flat run to flag).
    function populate(agent, sim, opts) {
        opts = opts || {};
        const targetSamples   = opts.targetSamples   || 30;   // good episodes
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

            // Keep flag-reachers and partial-progress runs above the floor.
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
