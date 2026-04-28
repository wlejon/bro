// play_agent.js — inference-only ExIt-style agent. PolicyValueNet + MCTS +
// per-decision tuple bookkeeping. No replay buffer, no trainer — those
// live in the trainer worker. This module is the substrate used by both
// the live (display) worker and the data-generating MCTS workers.
//
// Lifecycle:
//   const agent = PlayAgent.create({ sim, iterations, ... });
//   agent.setWeights(bytes, version);   // when a publish arrives
//   while (true) {
//       agent.startEpisode();            // captures startSnap, clears buffers
//       while (!done) {
//           const action = agent.decide();
//           const out = agent.applyAction(action);
//           done = out.done;
//       }
//       const result = agent.endEpisode(reason);
//       // result = { tuples, actions, startSnap, totalReturn, decisions, bestX }
//   }
//
// Optional behavior knobs:
//   priorBias(sig, legal) → Float32Array(numActions) of multiplicative
//     factors applied to the network's prior at every expansion. Used by
//     the live worker to penalize actions on the failure tape.
//   sigFn() → string used as the lookup key for priorBias. Worker-supplied.

(function (global) {
    'use strict';

    const NN = bro.ai.game.nn;

    function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }
    function softmax(logits, mask) {
        const n = logits.length;
        const out = new Float32Array(n);
        let m = -Infinity;
        for (let i = 0; i < n; i++) if (!mask || mask[i]) if (logits[i] > m) m = logits[i];
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
        if (!sim) throw new Error('PlayAgent.create requires {sim}');

        const obsDim     = SwAgentObs.OBS_DIM;
        const numActions = sim.numActions;
        const iterations   = opts.iterations   != null ? opts.iterations   : 64;
        const cPuct        = opts.cPuct        != null ? opts.cPuct        : 1.5;
        const gamma        = opts.gamma        != null ? opts.gamma        : 0.99;
        const rolloutDepth = opts.rolloutDepth != null ? opts.rolloutDepth : 12;
        const dirichletAlpha   = opts.dirichletAlpha   != null ? opts.dirichletAlpha   : 0.0;
        const dirichletEpsilon = opts.dirichletEpsilon != null ? opts.dirichletEpsilon : 0.0;

        const priorBias = opts.priorBias || null;
        const sigFn     = opts.sigFn     || null;

        const net = NN.createPolicyValueNet({
            inDim: obsDim,
            hidden: opts.hidden || [128, 128],
            valueHidden: opts.valueHidden || 64,
            numActions,
            seed: opts.seed != null ? BigInt(opts.seed) : 0xA11CE5n,
        });
        const xT  = NN.createTensor(obsDim);
        const lgT = NN.createTensor(numActions);

        let netVersion = 0n;
        let weightsLoaded = false;

        function setWeights(bytes, version) {
            net.load(bytes);
            netVersion = BigInt(version || 0n);
            weightsLoaded = true;
        }

        function netForward(obs) {
            xT.fromArray(obs);
            const v = net.forward(xT, lgT);
            return { value: v, logits: lgT.toArray() };
        }
        function priorFn(obs, legal) {
            const { logits } = netForward(obs);
            const mask = new Float32Array(numActions);
            for (let i = 0; i < legal.length; i++) mask[legal[i]] = 1;
            const probs = softmax(logits, mask);
            // Apply optional bias (failure-tape penalty etc.) and renormalize.
            if (priorBias && sigFn) {
                const sig = sigFn();
                const bias = priorBias(sig, legal);
                if (bias) {
                    let s = 0;
                    for (let i = 0; i < legal.length; i++) {
                        const a = legal[i];
                        const b = bias[a];
                        probs[a] *= (b != null && b > 0) ? b : 1;
                        s += probs[a];
                    }
                    if (s > 0) for (let i = 0; i < legal.length; i++) {
                        probs[legal[i]] /= s;
                    }
                }
            }
            return probs;
        }
        function valueFn(obs) { return netForward(obs).value; }

        const mctsSeed = opts.seed != null
            ? (Number(BigInt(opts.seed) & 0xFFFFFFFFn) ^ 0xC0DE) >>> 0
            : 0xC0DE;
        const mcts = bro.ai.game.createGenericMcts({
            env: {
                numActions,
                snapshot:     () => sim.snapshot(),
                restore:      (s) => sim.restore(s),
                step:         (a) => sim.step(a),
                legalActions: () => sim.legalActions(),
                observe:      () => SwAgentObs.build(sim),
            },
            cPuct, gamma, rolloutDepth,
            iterations,
            dirichletAlpha, dirichletEpsilon,
            seed: mctsSeed,
            priorFn, valueFn,
        });

        // Per-episode buffers.
        const pending   = [];     // [{obs, policyTarget, reward}]
        const actionLog = [];     // sequence of actions taken
        let startSnap = null;
        let bestX = -Infinity;
        let totalReward = 0;

        function startEpisode() {
            startSnap = sim.snapshot();
            pending.length = 0;
            actionLog.length = 0;
            bestX = sim.player.x;
            totalReward = 0;
        }

        function decide() {
            const obs = SwAgentObs.build(sim).slice();
            mcts.reset();
            const action = mcts.search();
            const visits = mcts.rootVisits();
            pending.push({ obs, policyTarget: visits, reward: 0 });
            return action;
        }

        function applyAction(action) {
            const out = sim.step(action);
            if (pending.length) pending[pending.length - 1].reward = out.reward;
            actionLog.push(action);
            totalReward += out.reward;
            if (sim.player.x > bestX) bestX = sim.player.x;
            return out;
        }

        // Step without MCTS (used by live worker to replay a trajectory
        // prefix from the best-crop pool before taking over with searches).
        function applyActionNoSearch(action) {
            const out = sim.step(action);
            actionLog.push(action);
            totalReward += out.reward;
            if (sim.player.x > bestX) bestX = sim.player.x;
            return out;
        }

        function endEpisode(reason) {
            // Seal value targets: discounted return from end → start, clamp ±1.
            let g = 0;
            for (let i = pending.length - 1; i >= 0; i--) {
                g = pending[i].reward + gamma * g;
                pending[i].valueTarget = clamp(g, -1, 1);
            }
            const tuples = pending.slice();
            const result = {
                tuples,
                reason: reason || 'end',
                actions: actionLog.slice(),
                startSnap,
                bestX,
                decisions: tuples.length,
                totalReturn: totalReward,
            };
            pending.length = 0;
            actionLog.length = 0;
            startSnap = null;
            return result;
        }

        // Drop accumulated state without sealing — used when caller wants
        // to abandon an in-progress episode (e.g. reseed mid-flight).
        function abortEpisode() {
            pending.length = 0;
            actionLog.length = 0;
            startSnap = null;
            totalReward = 0;
            bestX = -Infinity;
        }

        return {
            setWeights, startEpisode,
            decide, applyAction, applyActionNoSearch,
            endEpisode, abortEpisode,
            get sim() { return sim; },
            get netVersion() { return netVersion; },
            get weightsLoaded() { return weightsLoaded; },
            get iterations() { return iterations; },
            get net() { return net; },
        };
    }

    global.PlayAgent = { create };
})(typeof window !== 'undefined' ? window : globalThis);
