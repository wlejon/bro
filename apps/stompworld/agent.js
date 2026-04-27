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
        const bufCap  = opts.bufferCapacity != null ? opts.bufferCapacity : 4096;
        const startupIterations = opts.startupIterations != null ? opts.startupIterations : 50;
        const maxIterations     = opts.maxIterations     != null ? opts.maxIterations     : 200;
        const trainStepsPerEpisode = opts.trainStepsPerEpisode != null ? opts.trainStepsPerEpisode : 30;

        // ── Net + handle + buffer + trainer ─────────────────────────────────
        const net = NN.createPolicyValueNet({
            inDim: obsDim,
            hidden: opts.hidden || [64, 64],
            valueHidden: opts.valueHidden || 32,
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
            lr:        opts.lr       != null ? opts.lr       : 0.01,
            momentum:  opts.momentum != null ? opts.momentum : 0.9,
            batch:     opts.batch    != null ? opts.batch    : 32,
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
            for (const tup of pending) {
                buf.push({
                    obs: tup.obs,
                    policyTarget: tup.policyTarget,
                    actionMask: action_mask,
                    valueTarget: tup.valueTarget,
                });
            }
            const tupCount = pending.length;
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
        };
    }

    global.SwAgent = { create };
})(typeof window !== 'undefined' ? window : globalThis);
