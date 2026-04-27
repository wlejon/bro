// live_agent.js — inference-only agent that drives the displayed sim while
// the trainer worker churns in the background.
//
// Owns its own PolicyValueNet of identical shape to the worker's. On every
// `setWeights(bytes)` it `load()`s a fresh snapshot. Each decision runs a
// low-iteration MCTS using the live net as prior + value. No buffer, no
// trainer, no episode bookkeeping — failure on a death/flag/timeout is the
// caller's problem (they reset the displayed sim).

(function (global) {
    'use strict';

    const NN = bro.ai.game.nn;

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
        if (!sim) throw new Error('LiveAgent.create requires {sim}');

        const obsDim     = SwAgentObs.OBS_DIM;
        const numActions = sim.numActions;
        const iterations = opts.iterations != null ? opts.iterations : 24;
        const cPuct      = opts.cPuct      != null ? opts.cPuct      : 1.5;
        const gamma      = opts.gamma      != null ? opts.gamma      : 0.99;
        const rolloutDepth = opts.rolloutDepth != null ? opts.rolloutDepth : 4;

        // Match the worker's net config exactly. If you change the worker's
        // hidden/valueHidden, change them here too — load() will fault on a
        // shape mismatch otherwise.
        const net = NN.createPolicyValueNet({
            inDim: obsDim,
            hidden: opts.hidden || [64, 64],
            valueHidden: opts.valueHidden || 32,
            numActions,
            seed: 0xA11CE5n,
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
            return softmax(logits, mask);
        }
        function valueFn(obs) { return netForward(obs).value; }

        const mcts = MctsJs.create({
            env: {
                snapshot:     () => sim.snapshot(),
                restore:      (s) => sim.restore(s),
                step:         (a) => sim.step(a),
                legalActions: () => sim.legalActions(),
                observe:      () => SwAgentObs.build(sim),
                numActions,
            },
            numActions, cPuct, seed: 0xC0DE,
        });

        function decide() {
            mcts.reset();
            return mcts.search({
                iterations, priorFn, valueFn, gamma, rolloutDepth,
            });
        }

        return {
            decide,
            setWeights,
            get sim() { return sim; },
            get netVersion() { return netVersion; },
            get weightsLoaded() { return weightsLoaded; },
        };
    }

    global.LiveAgent = { create };
})(typeof window !== 'undefined' ? window : globalThis);
