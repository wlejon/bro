// =============================================================================
// bro.ai.game.learn, Training infrastructure for game AI
// =============================================================================
//
// The glue between a search (docs/ai-game-learning.js) and a network
// (docs/ai-nn-api.js): replay buffers, the evaluator/prior adapters that let
// an MCTS consult a net, the expert-iteration trainers, and the batched
// inference server with its two backend wrappers.
//
// Companion files (one namespace each, same library):
//   docs/ai-game-api.js       NavGrid / HexNav / NavMesh / routing
//   docs/ai-game-planning.js  Agent, World, Unit, steering, perception, bindings
//   docs/ai-game-learning.js  MCTS family, planners, belief, simulation, replay
//   docs/ai-nn-api.js         bro.ai.game.nn: circuits, nets, ops, WeightsHandle
//   docs/ai-game-tools.js     bro.ai.game.grid: obs windows, tapes, GridTrainer
//
// Available in all modes, windowed, headless and bro-server. Training loops
// are normally driven from bro-headless or bro-server.
//
// Feature gate: this namespace shares the tensor tower with bro.ai.game.nn,
// so it is compiled out together with it:
//
//   if (bro.ai.game.learn.available === false) return;   // compiled out
//
// A compiled-in build does not define `available`; only the stub does.
//
// Two parallel stacks live here, and they do NOT mix:
//
//   combat-shaped                      observation-agnostic
//   -------------                      --------------------
//   Situation                          GenericSituation
//   createReplayBuffer                 createGenericReplayBuffer
//   createExItTrainer                  createGenericExItTrainer
//   SingleHeroNet                      PolicyValueNet / SingleHeroNetTX
//   createNeuralEvaluator/Prior        createDirectBackend/createServerBackend
//   drives createMcts & friends        drives createGenericMcts
//
// Pick the left column when you are planning over the bundled combat sim
// (hero, enemies, abilities). Pick the right column for anything else: your
// own JS env, a flat discrete action space, a hand-crafted observation.
//
// =============================================================================


// -----------------------------------------------------------------------------
// Situation, the combat-shaped training example
// -----------------------------------------------------------------------------
//
// A plain JS object. Every field is optional on the way IN (missing fields
// stay zero) and always present on the way OUT of sample()/all(), where each
// array is a freshly allocated Float32Array copy:
//
//   {
//     obs:           Float32Array(bro.ai.game.OBS_TOTAL),
//     atkMask:       Float32Array(nn.N_ATTACK),    // 1 legal, 0 illegal
//     abilMask:      Float32Array(nn.N_ABILITY),
//     targetMove:    Float32Array(nn.N_MOVE),      // soft visit distribution
//     targetAttack:  Float32Array(nn.N_ATTACK),
//     targetAbility: Float32Array(nn.N_ABILITY),
//     valueTarget:   number in [-1, 1],
//   }
//
// Arrays shorter than the head width are copied as far as they go; longer
// ones are truncated. Nothing throws on a length mismatch.

/**
 * Fixed-capacity FIFO of Situations. Pushing past `capacity` drops the
 * oldest.
 * @param {number} [capacity=4096] - non-positive falls back to 4096
 * @returns {AIReplayBuffer}
 */
const buf = bro.ai.game.learn.createReplayBuffer(4096);

buf.push(situation);          // throws TypeError on a non-object
buf.size; buf.capacity;       // read-only
const batch = buf.sample(32); // uniform WITH replacement, [Situation, ...]
const all = buf.all();        // every live entry, oldest first
buf.clear();


// -----------------------------------------------------------------------------
// Neural adapters, let an MCTS consult a net
// -----------------------------------------------------------------------------
//
// Pass these objects as the `evaluator` / `prior` option of createMcts,
// createDecoupledMcts, createTeamMcts, createInfoSetMcts, ... in place of the
// string presets ("hpDelta", "attackBias", ...). They only accept a
// SingleHeroNet: the combat heads are what the search's action space is
// shaped like.
//
// The optional WeightsHandle is the hot-swap channel: each evaluation checks
// the handle's version and reloads the net when the trainer has published
// something newer, so a long-running searcher picks up training progress
// without being restarted.

/**
 * IEvaluator adapter: leaf value from the net's value head.
 * @param {AISingleHeroNet} net
 * @param {AIWeightsHandle} [handle]
 * @returns {AINeuralEvaluator}
 * @throws {TypeError} when `net` is not a SingleHeroNet
 */
const neuralEval = bro.ai.game.learn.createNeuralEvaluator(net, handle);

/** @param {AIWorld} world @param {number} heroId @returns {number} [-1, 1] */
neuralEval.evaluate(world, heroId);

/**
 * IPrior adapter: action priors from the net's factored policy head.
 * @param {AISingleHeroNet} net
 * @param {AIWeightsHandle} [handle]
 * @returns {AINeuralPrior}
 */
const neuralPrior = bro.ai.game.learn.createNeuralPrior(net, handle);

/** Softmax temperature on the emitted priors. > 1 flattens, < 1 sharpens. */
neuralPrior.setTemperature(1.0);

/** Blend this much uniform mass into the prior, an exploration floor that
 *  keeps a confidently wrong net from starving every other action. */
neuralPrior.setUniformMix(0.05);

/**
 * Wrap any bound prior and add IID Gumbel noise at the root — the cheap
 * exploration trick for small iteration budgets (Danihelka 2022). The inner
 * prior may be a NeuralPrior, another GumbelNoisePrior, or one of the
 * built-in prior objects (createUniformPrior / createAttackBiasPrior /
 * createTacticPrior).
 * @param {Object} innerPrior
 * @param {number} [scale=1.0]
 * @returns {AIGumbelNoisePrior}
 * @throws {TypeError} when innerPrior is not a bound prior object
 */
const gumbel = bro.ai.game.learn.createGumbelNoisePrior(neuralPrior, 1.0);
gumbel.reseed(0xA11CEn);       // bigint or number
gumbel.setScale(1.0);

const mctsWithNN = bro.ai.game.createMcts({
    iterations: 800, priorC: 2.0,
    evaluator: neuralEval,
    prior: gumbel,
});


// -----------------------------------------------------------------------------
// ExItTrainer, expert iteration against a SingleHeroNet
// -----------------------------------------------------------------------------
//
// Mini-batch SGD with momentum against (value, policy) targets drawn from a
// ReplayBuffer, publishing to a WeightsHandle every `publishEvery` steps so
// searchers pick the new weights up mid-run. Set the net, the buffer and the
// handle once; then step().

const trainer = bro.ai.game.learn.createExItTrainer();

trainer.setNet(net);                  // SingleHeroNet; else TypeError
trainer.setBuffer(buf);               // ReplayBuffer;  else TypeError
trainer.setWeightsHandle(handle);     // WeightsHandle; else TypeError

/**
 * Merge into the current config; omitted keys keep their value.
 * @param {Object} cfg
 * @param {number} [cfg.lr]
 * @param {number} [cfg.momentum]
 * @param {number} [cfg.batch]         - situations per step
 * @param {number} [cfg.policyWeight]  - loss mix
 * @param {number} [cfg.valueWeight]
 * @param {number} [cfg.publishEvery]  - steps between WeightsHandle publishes
 * @param {bigint|number} [cfg.rngSeed] - batch-sampling seed
 */
trainer.setConfig({
    lr: 0.01, momentum: 0.9, batch: 32,
    policyWeight: 1.0, valueWeight: 1.0,
    publishEvery: 100,
    rngSeed: 0x1234n,
});

/** One SGD step. @returns {{lossValue, lossPolicy, lossTotal, samples}}
 *  `samples` is 0 when the buffer had nothing to draw. */
const step = trainer.step();

/** n steps; the result is the LAST step's losses, not an average. */
const stepN = trainer.stepN(100);

trainer.totalSteps; trainer.totalPublishes;   // read-only counters


// -----------------------------------------------------------------------------
// Turning a finished search into training targets
// -----------------------------------------------------------------------------
//
// All three read the root of the MOST RECENT search on a classic Mcts
// (createMcts / createDecoupledMcts / createTeamMcts / createInfoSetMcts),
// and return null when that search never ran or the tree is empty.

/**
 * AlphaZero-style visit-count targets, one normalized distribution per head.
 * @param {AIMcts} mcts
 * @returns {{move: Float32Array, attack: Float32Array, ability: Float32Array}|null}
 */
const targets = bro.ai.game.learn.targetsFromMcts(mcts);

/**
 * Build a complete Situation from a finished search: observation, masks and
 * policy targets are filled from the world/hero/root. `valueTarget` is left
 * at 0 — the caller writes the eventual episode return before pushing, since
 * only the caller knows how the episode ended.
 * @param {AIMcts} mcts @param {AIAgent} hero @param {AIWorld} world
 * @returns {Object|null} a Situation
 */
const sit = bro.ai.game.learn.makeSituation(mcts, hero, world);
sit.valueTarget = finalReturn;
buf.push(sit);

/**
 * Gumbel-improved policy target (Danihelka 2022, simplified): a completed-Q
 * reweighting that gives a usable target from far fewer iterations than raw
 * visit counts need.
 * @returns {{move, attack, ability}|null}
 */
const tgt2 = bro.ai.game.learn.gumbelImprovedPolicy(mcts);


// =============================================================================
// Generic ExIt: arbitrary observation, flat discrete action space
// =============================================================================
//
// Pair a PolicyValueNet (or SingleHeroNetTX) with the generic buffer and
// trainer when the problem does not fit the combat-shaped Situation: no
// enemy slots, no ability cooldowns, no factored heads. Same SGD+momentum
// loop, same WeightsHandle hot-swap; it differs only in the example shape and
// which net it drives.

/**
 * GenericSituation, a plain JS object the buffer accepts:
 *   {
 *     obs:          Float32Array(net.inDim),        // required
 *     policyTarget: Float32Array(net.numActions),   // required, sums to ~1
 *     actionMask?:  Float32Array(net.numActions),   // 1 legal / 0 illegal;
 *                                                   // omit ⇒ all legal
 *     valueTarget:  number in [-1, 1],              // usually the clipped
 *                                                   // discounted return
 *   }
 * A missing `obs` or `policyTarget` makes push() throw a TypeError.
 */

/** @param {number} [capacity=4096] @returns {AIGenericReplayBuffer} */
const gbuf = bro.ai.game.learn.createGenericReplayBuffer(4096);
gbuf.push({ obs: o, policyTarget: pi, actionMask: mask, valueTarget: 0.7 });
gbuf.size; gbuf.capacity;
const gbatch = gbuf.sample(32);   // [GenericSituation, ...], with replacement
const gall = gbuf.all();
gbuf.clear();

const gtrainer = bro.ai.game.learn.createGenericExItTrainer();

/** PolicyValueNet or SingleHeroNetTX. A SingleHeroNet is rejected. */
gtrainer.setNet(pvnet);
gtrainer.setBuffer(gbuf);
gtrainer.setWeightsHandle(handle);

/**
 * Same keys as ExItTrainer.setConfig, plus one:
 * @param {"cpu"|"gpu"} [cfg.device] - where the SGD runs. Requires
 *     `net.to("gpu")` first. Unlike the net's own to(), an unavailable GPU
 *     here does NOT throw: the trainer silently falls back to the CPU, so
 *     check `pvnet.device` if you need to be sure.
 */
gtrainer.setConfig({
    lr: 0.01, momentum: 0.9, batch: 32,
    policyWeight: 1.0, valueWeight: 1.0,
    publishEvery: 100,
    rngSeed: 0x1234n,
    device: "gpu",
});

const gstep = gtrainer.step();      // {lossValue, lossPolicy, lossTotal, samples}
const gstepN = gtrainer.stepN(100);
gtrainer.totalSteps; gtrainer.totalPublishes;


// -----------------------------------------------------------------------------
// Batched inference: server and backends
// -----------------------------------------------------------------------------
//
// Batching only pays off under CONCURRENT callers. A single-threaded JS
// caller always submits one observation at a time and gets a batch of one;
// to batch several observations gathered within a single JS tick, use
// net.forwardBatched(x, logits, values) (docs/ai-nn-api.js) instead — that
// is the real single-threaded win. The server exists for concurrent callers
// (a root-parallel search sharing one net) and to back `createServerBackend`.
//
// The net must implement the batched interface: PolicyValueNet or
// SingleHeroNetTX. SingleHeroNet is rejected with a TypeError.

/**
 * @param {AIPolicyValueNet|AISingleHeroNetTX} net
 * @param {Object} [cfg]
 * @param {number} [cfg.maxBatchSize=64]
 * @param {number} [cfg.maxWaitMicros=500] - how long the worker waits for a
 *     batch to fill before dispatching what it has
 * @returns {AIInferenceServer}
 */
const server = bro.ai.game.learn.createInferenceServer(pvnet, {
    maxBatchSize: 64,
    maxWaitMicros: 500,
});

/** Blocking single evaluation.
 *  @param {Float32Array} obs
 *  @returns {{logits: Float32Array, value: number}} */
const r1 = server.evaluate(obsF32);

/** Fan several rows out through the async path so they coalesce into one
 *  batch on the worker, then wait on all of them.
 *  @param {Float32Array[]} rows
 *  @returns {Array<{logits: Float32Array, value: number}>} */
const rows = server.evaluateBatch([obsF32, obsF32Other]);

server.batchesRun;   // read-only count of dispatched batches
server.shutdown();   // stop and join the worker thread; the server is dead
                     // afterwards, evaluate() on it throws

/**
 * IInferenceBackend wrappers. These are what GenericMcts's `backend` option
 * takes: a masked-softmax prior plus a value, evaluated entirely in C++ with
 * no per-node JS round trip.
 *
 * Direct evaluates the net inline on the calling thread (no server, no
 * threading). Server routes through a BatchedInferenceServer.
 */
const directBackend = bro.ai.game.learn.createDirectBackend(pvnet);
const serverBackend = bro.ai.game.learn.createServerBackend(server, pvnet);
directBackend.numActions; directBackend.inDim;   // read-only

// IMPORTANT: a backend is NOT a drop-in for the `evaluator` / `prior` slot on
// createMcts / createDecoupledMcts / createTeamMcts. Those take the
// combat-shaped IEvaluator / IPrior above. A backend plugs in one layer
// down, at GenericMcts's own prior_fn / value_fn, over a flat action space
// matching net.numActions. An explicit priorFn/valueFn always wins over
// `backend` when both are supplied.
const gm = bro.ai.game.createGenericMcts({
    env: myGenericEnv,
    backend: directBackend,   // or serverBackend
    iterations: 400,
});


// -----------------------------------------------------------------------------
// A complete self-play loop, end to end
// -----------------------------------------------------------------------------

const nn = bro.ai.game.nn;
const learn = bro.ai.game.learn;

const heroNet = nn.createSingleHeroNet({ trunkHidden: 64, valueHidden: 32, seed: 1n });
const weights = nn.createWeightsHandle();
weights.publish(heroNet.save(), 1n);

const replay = learn.createReplayBuffer(65536);
const tr = learn.createExItTrainer();
tr.setNet(heroNet); tr.setBuffer(replay); tr.setWeightsHandle(weights);
tr.setConfig({ lr: 1e-3, momentum: 0.9, batch: 64, publishEvery: 200 });

const search = bro.ai.game.createMcts({
    iterations: 400,
    evaluator: learn.createNeuralEvaluator(heroNet, weights),
    prior: learn.createNeuralPrior(heroNet, weights),
});

for (let episode = 0; episode < 1000; episode++) {
    const pending = [];
    resetEpisode(world);
    while (!episodeOver(world)) {
        const action = search.search(world, hero);
        pending.push(learn.makeSituation(search, hero, world));
        bro.ai.game.applyCombatAction(hero, world, action);
        world.tick(1 / 60);
    }
    const outcome = hero.unit.alive ? 1 : -1;      // the episode return
    for (const s of pending) { s.valueTarget = outcome; replay.push(s); }
    tr.stepN(16);
}
