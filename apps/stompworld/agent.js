// agent.js — ExIt-style learning agent for stompworld.
//
// Wires together:
//   - bro.ai.game.nn.createPolicyValueNet           (the apprentice)
//   - bro.ai.game.learn.createGenericReplayBuffer   (training tuple FIFO)
//   - bro.ai.game.learn.createGenericExItTrainer    (SGD+momentum)
//   - MctsJs                                         (the expert)
//   - SwSim + SwAgentObs                             (env + observation)
//   - Ghosts                                         (trajectory replay)
//
// One agent owns one live sim. Per decision it MCTS-searches the sim
// (the search snapshots/restores transparently), pushes a (obs, visit_dist)
// tuple into a pending list, applies the chosen action to the live sim, and
// records a ghost frame. On episode end the pending tuples are sealed with
// the discounted episode return (clamped to [-1, 1]) before being pushed
// into the replay buffer; then the trainer runs a few SGD steps. The
// trainer publishes via WeightsHandle so the next decision automatically
// snapshots the latest weights into the prior+value functions.

(function (global) {
    'use strict';

    const NN    = bro.ai.game.nn;
    const LEARN = bro.ai.game.learn;

    function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }
    function softmax(logits, mask) {
        // Numerically stable masked softmax. Returns Float32Array.
        const n = logits.length;
        const out = new Float32Array(n);
        let m = -Infinity;
        for (let i = 0; i < n; i++) {
            if (!mask || mask[i]) { if (logits[i] > m) m = logits[i]; }
        }
        let s = 0;
        for (let i = 0; i < n; i++) {
            if (!mask || mask[i]) { const e = Math.exp(logits[i] - m); out[i] = e; s += e; }
        }
        if (s > 0) for (let i = 0; i < n; i++) out[i] /= s;
        return out;
    }

    function create(opts) {
        opts = opts || {};
        const sim = opts.sim;
        if (!sim) throw new Error('SwAgent.create requires {sim}');

        const obsDim = SwAgentObs.OBS_DIM;
        const numActions = sim.numActions;
        const gamma   = opts.gamma   != null ? opts.gamma   : 0.99;
        // Buffer is large enough to retain many recent episodes; with the
        // shorter (~20s) training horizon used in trainer_worker.js this
        // amounts to dozens of trajectories instead of just one.
        const bufCap  = opts.bufferCapacity != null ? opts.bufferCapacity : 50000;
        const startupIterations = opts.startupIterations != null ? opts.startupIterations : 80;
        const maxIterations     = opts.maxIterations     != null ? opts.maxIterations     : 400;
        const trainStepsPerEpisode = opts.trainStepsPerEpisode != null ? opts.trainStepsPerEpisode : 60;
        // Dirichlet exploration noise on the MCTS root prior. AlphaZero
        // settled on (ε=0.25, α=0.3) for chess; we use (0.25, 0.5) — same
        // ε so MCTS visits aren't dominated by a confident-but-wrong prior,
        // and slightly higher α so the noise spreads across our smaller
        // (6) action space rather than concentrating on one action.
        const dirichletAlpha   = opts.dirichletAlpha   != null ? opts.dirichletAlpha   : 0.5;
        const dirichletEpsilon = opts.dirichletEpsilon != null ? opts.dirichletEpsilon : 0.25;
        // Successful-episode bias: push flag-reaching tuples this many
        // times into the FIFO buffer. Cheap weighted-sampling proxy: makes
        // those critical "JR-at-the-gap-edge" decisions show up more often
        // when the trainer pulls a batch.
        const flagPushMult = opts.flagPushMult != null ? opts.flagPushMult : 3;

        // ── Net + handle + buffer + trainer ─────────────────────────────────
        // Bigger trunk (128×128 vs the old 64×64 — ~3× params) and a wider
        // value head. The user wants the net to drive most of the win, so
        // raw policy capacity is the lever — MCTS will then layer polish
        // on top instead of doing the heavy lifting.
        const net = NN.createPolicyValueNet({
            inDim: obsDim,
            hidden: opts.hidden || [128, 128],
            valueHidden: opts.valueHidden || 64,
            numActions,
            seed: opts.seed != null ? opts.seed : 0xA11CE5n,
        });
        const handle = NN.createWeightsHandle();
        const buf    = LEARN.createGenericReplayBuffer(bufCap);
        const trainer = LEARN.createGenericExItTrainer();
        trainer.setNet(net);
        trainer.setBuffer(buf);
        trainer.setWeightsHandle(handle);
        trainer.setConfig({
            // Wider net + bigger batch ⇒ smaller LR for stable gradients.
            lr:        opts.lr       != null ? opts.lr       : 0.005,
            momentum:  opts.momentum != null ? opts.momentum : 0.9,
            batch:     opts.batch    != null ? opts.batch    : 64,
            policyWeight: 1.0, valueWeight: 1.0,
            publishEvery: 25,
            rngSeed: 0x1234n,
        });

        // Reusable Tensors for forward calls (avoid per-decision allocation).
        const xT = NN.createTensor(obsDim);
        const lgT = NN.createTensor(numActions);

        function netForward(obs) {
            xT.fromArray(obs);
            const v = net.forward(xT, lgT);
            return { value: v, logits: lgT.toArray() };
        }
        function priorFn(obs, legal) {
            const { logits } = netForward(obs);
            const mask = new Float32Array(numActions);
            for (let i = 0; i < legal.length; i++) mask[legal[i]] = 1;
            return softmax(logits, mask);
        }
        function valueFn(obs) {
            const { value } = netForward(obs);
            return value;
        }

        // ── MCTS on top of the sim ──────────────────────────────────────────
        // The sim is a snapshot/restore/step/legalActions provider; the
        // observation function is supplied by the agent (sim-agnostic obs
        // helper, called fresh each time MCTS needs the leaf state vector).
        const mctsEnv = {
            snapshot:     () => sim.snapshot(),
            restore:      (s) => sim.restore(s),
            step:         (a) => sim.step(a),
            legalActions: () => sim.legalActions(),
            observe:      () => SwAgentObs.build(sim),
            numActions,
        };
        const mcts = MctsJs.create({
            env: mctsEnv,
            numActions,
            cPuct: opts.cPuct != null ? opts.cPuct : 1.5,
            seed: 0xC0DE,
        });

        // ── Episode bookkeeping ─────────────────────────────────────────────
        const pending = []; // {obs:Float32Array, policyTarget:Float32Array, reward:number}
        const ghostRec = { frames: [] }; // populated by recordFrameTick

        let episodeNum = 0;
        let bestX = 0;
        let stats = {
            episode: 0,
            iters: 0,
            steps: 0,
            bestX: 0,
            lastReward: 0,
            lastReason: 'fresh',
            lossValue: 0,
            lossPolicy: 0,
            trainSteps: 0,
            netVersion: 0n,
        };

        // ── Decide one action and apply it to the live sim ──────────────────
        function decide() {
            // Build obs from current live state for the training tuple.
            const obs = SwAgentObs.build(sim).slice(); // copy: shared buffer in builder

            // Run MCTS. priorFn/valueFn use current net snapshot.
            const iters = Math.min(maxIterations,
                                   startupIterations + (episodeNum * 5));
            mcts.reset(); // fresh root each decision (tree reuse + drift in
                          // dynamic envs is brittle; cheap to rebuild).
            const action = mcts.search({
                iterations: iters,
                priorFn, valueFn,
                gamma,
                rolloutDepth: 6,
                dirichletAlpha, dirichletEpsilon,
            });
            const visits = mcts.rootVisits();

            pending.push({ obs, policyTarget: visits, reward: 0 });
            stats.iters++;
            return action;
        }

        function applyAction(action) {
            const out = sim.step(action);
            if (pending.length) pending[pending.length - 1].reward = out.reward;
            if (sim.player.x > bestX) bestX = sim.player.x;
            stats.steps++;
            return out;
        }

        // Per-decision frame for ghost replay (just AFTER step).
        function recordFrameTick() {
            const p = sim.player;
            // Frame index mirrors app.js drawHero: 0 idle, 1/2 run, 3 jumping.
            let frame = 0;
            if (!p.onGround) frame = 3;
            else if (Math.abs(p.vx) > 8) frame = 1 + (((sim.tick / 8) | 0) % 2);
            ghostRec.frames.push({
                tick: sim.tick,
                x: p.x, y: p.y,
                frame, facing: p.facing,
            });
        }

        // ── Episode end: seal value targets, train, reset stats ─────────────
        function endEpisode(reason, ghosts) {
            // Compute discounted return from end → start, clamp to [-1, 1].
            let g = 0;
            for (let i = pending.length - 1; i >= 0; i--) {
                g = pending[i].reward + gamma * g;
                pending[i].valueTarget = clamp(g, -1, 1);
            }
            const action_mask = new Float32Array(numActions);
            for (let i = 0; i < numActions; i++) action_mask[i] = 1;
            // Push flag-reaching trajectories N× to bias the buffer. The
            // FIFO is 50k tuples; even 3× across a successful 300-decision
            // run only consumes ~1.8% of the buffer.
            const repeats = (reason === 'flag') ? flagPushMult : 1;
            for (let k = 0; k < repeats; k++) {
                for (const tup of pending) {
                    buf.push({
                        obs: tup.obs,
                        policyTarget: tup.policyTarget,
                        actionMask: action_mask,
                        valueTarget: tup.valueTarget,
                    });
                }
            }
            const tupCount = pending.length * repeats;
            pending.length = 0;

            // Hand the trajectory off to the ghost recorder.
            if (ghosts && ghostRec.frames.length > 0) {
                ghosts.commit({ frames: ghostRec.frames.slice() });
            }
            ghostRec.frames.length = 0;

            // Train.
            let last = { lossValue: 0, lossPolicy: 0, samples: 0 };
            if (buf.size >= 32) {
                last = trainer.stepN(trainStepsPerEpisode);
            }

            episodeNum++;
            stats.episode = episodeNum;
            stats.bestX = bestX;
            stats.lastReason = reason;
            stats.lossValue = last.lossValue;
            stats.lossPolicy = last.lossPolicy;
            stats.trainSteps = trainer.totalSteps;
            const snap = handle.snapshot();
            stats.netVersion = snap ? snap.version : 0n;
            stats.tuplesPushed = tupCount;
            stats.bufSize = buf.size;
            return last;
        }

        function resetEpisode() {
            sim.reset();
            mcts.reset();
            bestX = sim.player.x;
            pending.length = 0;
            ghostRec.frames.length = 0;
        }

        return {
            decide, applyAction, recordFrameTick,
            endEpisode, resetEpisode,
            get sim() { return sim; },
            get stats() { return stats; },
            get net() { return net; },
            get buffer() { return buf; },
            get trainer() { return trainer; },
            get handle() { return handle; },
        };
    }

    global.SwAgent = { create };
})(typeof window !== 'undefined' ? window : globalThis);
