// agent.js — trainer-side wiring: PolicyValueNet + replay buffer + ExIt
// trainer + WeightsHandle, all stitched together. The trainer worker is
// the only consumer; it pulls .net / .buffer / .trainer / .handle off the
// returned object. Self-play and MCTS live in play_agent.js (used by
// live_worker and mcts_worker); they don't go through here.

(function (global) {
    'use strict';

    const NN    = bro.ai.game.nn;
    const LEARN = bro.ai.game.learn;

    function create(opts) {
        opts = opts || {};
        const sim = opts.sim;
        if (!sim) throw new Error('SwAgent.create requires {sim}');

        const obsDim = SwAgentObs.OBS_DIM;
        const numActions = sim.numActions;
        // Buffer is large enough to retain many recent episodes; with the
        // shorter (~20s) training horizon used in trainer_worker.js this
        // amounts to dozens of trajectories instead of just one.
        const bufCap = opts.bufferCapacity != null ? opts.bufferCapacity : 50000;

        // Bigger trunk (128×128 vs 64×64) and a wider value head — raw
        // policy capacity is the lever, MCTS just polishes on top.
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

        return { net, buffer: buf, trainer, handle };
    }

    global.SwAgent = { create };
})(typeof window !== 'undefined' ? window : globalThis);
