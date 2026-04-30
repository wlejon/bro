// agent.js — trainer-side wiring: PolicyValueNet + replay buffer + ExIt
// trainer + WeightsHandle, all stitched together. The trainer worker is
// the only consumer; it pulls .net / .buffer / .trainer / .handle off the
// returned object. Self-play and MCTS live in play_agent.js (used by
// mcts_worker); they don't go through here.

(function (global) {
    'use strict';

    const NN    = bro.ai.game.nn;
    const LEARN = bro.ai.game.learn;

    function create(opts) {
        opts = opts || {};
        const sim = opts.sim;
        if (!sim) throw new Error('SwAgent.create requires {sim}');

        const obsDim = SwAgentObs.OBS_DIM;
        const headSizes = SwSim.HEAD_SIZES;
        const bufCap = opts.bufferCapacity != null ? opts.bufferCapacity : 50000;

        const net = NN.createPolicyValueNet({
            inDim: obsDim,
            hidden: opts.hidden || [128, 128],
            valueHidden: opts.valueHidden || 64,
            headSizes,
            seed: opts.seed != null ? opts.seed : 0xA11CE5n,
        });

        // Trainer device: prefer GPU when the runtime has CUDA compiled in.
        // Inference workers build their own nets and stay on CPU — the trainer
        // publishes downloaded weights through the WeightsHandle, so consumers
        // are unaffected by where training runs.
        const wantGpu = opts.device !== 'cpu'
                     && bro.ai.game.nn.gpu && bro.ai.game.nn.gpu.available;
        if (wantGpu) net.to('gpu');

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
            device: wantGpu ? 'gpu' : 'cpu',
        });

        return { net, buffer: buf, trainer, handle };
    }

    global.SwAgent = { create };
})(typeof window !== 'undefined' ? window : globalThis);
