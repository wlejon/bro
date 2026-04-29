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
//   priorAdjust(sig, prior) → Float32Array(numActions) — receives the
//     softmax-and-mask-applied prior and returns an adjusted prior (already
//     multiplied + renormalized). Used by the live worker to penalize
//     actions tagged in the failure tape; matches bro.ai.game.grid.
//     FailureTape.applyPriors's contract directly.
//   sigFn() → string used as the lookup key for priorAdjust. Worker-supplied.

(function (global) {
    'use strict';

    const NN = bro.ai.game.nn;

    function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }

    function create(opts) {
        opts = opts || {};
        const sim = opts.sim;
        if (!sim) throw new Error('PlayAgent.create requires {sim}');

        const obsDim     = SwAgentObs.OBS_DIM;
        const numActions = sim.numActions;       // 6 (movement-only)
        const headSizes  = SwSim.HEAD_SIZES;
        const perHeadTotal = SwSim.PER_HEAD_TOTAL;
        const iterations   = opts.iterations   != null ? opts.iterations   : 64;
        const cPuct        = opts.cPuct        != null ? opts.cPuct        : 1.5;
        const gamma        = opts.gamma        != null ? opts.gamma        : 0.99;
        const rolloutDepth = opts.rolloutDepth != null ? opts.rolloutDepth : 12;
        const dirichletAlpha   = opts.dirichletAlpha   != null ? opts.dirichletAlpha   : 0.0;
        const dirichletEpsilon = opts.dirichletEpsilon != null ? opts.dirichletEpsilon : 0.0;

        const priorAdjust = opts.priorAdjust || null;
        const sigFn       = opts.sigFn       || null;

        const net = NN.createPolicyValueNet({
            inDim: obsDim,
            hidden: opts.hidden || [128, 128],
            valueHidden: opts.valueHidden || 64,
            headSizes,
            seed: opts.seed != null ? BigInt(opts.seed) : 0xA11CE5n,
        });
        const xT  = NN.createTensor(obsDim);
        // Single-head movement-only policy: net outputs `numActions` logits
        // directly (perHeadTotal === numActions === 6).
        const lgT = NN.createTensor(perHeadTotal);

        let netVersion = 0n;
        let weightsLoaded = false;

        function setWeights(bytes, version) {
            net.load(bytes);
            netVersion = BigInt(version || 0n);
            weightsLoaded = true;
        }

        function netForwardValue(obs) {
            xT.fromArray(obs);
            return net.forward(xT, lgT);
        }
        function priorFn(obs, legal) {
            xT.fromArray(obs);
            net.forward(xT, lgT);
            // Single-head softmax over the 6 movement actions, masked to
            // the legal set (which is currently always all 6).
            const logits = lgT.toArray();
            let m = -Infinity;
            for (let i = 0; i < legal.length; i++) {
                const v = logits[legal[i]];
                if (v > m) m = v;
            }
            const probs = new Float32Array(numActions);
            let s = 0;
            for (let i = 0; i < legal.length; i++) {
                const a = legal[i];
                const e = Math.exp(logits[a] - m);
                probs[a] = e; s += e;
            }
            if (s > 0) for (let i = 0; i < legal.length; i++) probs[legal[i]] /= s;
            if (priorAdjust && sigFn) {
                const adjusted = priorAdjust(sigFn(), probs);
                if (adjusted) {
                    let s2 = 0;
                    for (let i = 0; i < adjusted.length; i++) s2 += adjusted[i];
                    if (s2 > 0) for (let i = 0; i < adjusted.length; i++) adjusted[i] /= s2;
                    return adjusted;
                }
            }
            return probs;
        }
        function valueFn(obs) { return netForwardValue(obs); }

        const mctsSeed = opts.seed != null
            ? (Number(BigInt(opts.seed) & 0xFFFFFFFFn) ^ 0xC0DE) >>> 0
            : 0xC0DE;
        // env.snapshot/restore stash the tilemap's damage state in a single
        // tilemap-side slot rather than ferrying it through the JS snapshot
        // object (and the FFI) as an Int32Array each iteration. The damage
        // state stays in the tilemap library on this worker thread; the
        // snapshot we hand to C++ MCTS is small. One search at a time per
        // worker, so a single saved slot is enough.
        const mcts = bro.ai.game.createGenericMcts({
            env: {
                numActions,
                snapshot:     () => {
                    sim.tilemap.saveDamageSnapshot();
                    return sim.snapshot();
                },
                restore:      (s) => {
                    sim.restore(s);
                    sim.tilemap.restoreDamageSnapshot();
                },
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
            // Drop any overlays MCTS may have pushed during search — its
            // env.restore at the end of each iteration already truncated to
            // the saved count, but be explicit so the next applyAction
            // starts from a clean overlay slate before pushing real shapes.
            if (sim.tilemap.clearOverlays) sim.tilemap.clearOverlays();
            const visits = mcts.rootVisits();
            pending.push({ obs, policyTarget: visits, reward: 0 });
            return action;
        }

        function applyAction(action) {
            const out = sim.step(action);
            // Bake the real action's overlay shapes (if any) into the
            // bitmask. This is the one place per decision that pixel
            // iteration runs for fires; MCTS rollouts pay zero pixel cost.
            if (sim.tilemap.commitOverlays) sim.tilemap.commitOverlays();
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
            if (sim.tilemap.commitOverlays) sim.tilemap.commitOverlays();
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
