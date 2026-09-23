// =============================================================================
// bro.ai.game.grid, 2D grid-world / platformer training kit
// Observation windows, frame stacks, tapes, BC generation, replay, GridTrainer
// =============================================================================
//
// Built on the env-agnostic primitives (createGenericMcts,
// nn.createPolicyValueNet, learn.createGenericReplayBuffer,
// learn.createGenericExItTrainer, nn.createWeightsHandle). Use this when your
// env is a tilemap plus dynamic entities rather than the bundled combat sim.
// Each primitive is independently useful; GridTrainer composes them into a
// complete loop so a new project does not re-author the boilerplate.
//
// Companion files (one area each, same library):
//   docs/ai-game-api.js       NavGrid / HexNav / NavMesh / routing
//   docs/ai-game-planning.js  Agent, World, Unit, steering, perception, bindings
//   docs/ai-game-learning.js  MCTS family, planners, belief, simulation, replay
//   docs/ai-nn-api.js         bro.ai.game.nn: circuits, nets, ops, WeightsHandle
//   docs/ai-learn-api.js      bro.ai.game.learn: buffers, trainers, inference
//
// Available in all modes, windowed, headless and bro-server. The namespace
// shares the tensor tower's feature gate with nn and learn:
//
//   if (bro.ai.game.grid.available === false) return;   // compiled out
//
// A compiled-in build does not define `available`; only the stub does.

// -----------------------------------------------------------------------------
// ObsWindow, egocentric multi-channel rasterizer
// -----------------------------------------------------------------------------
//
// Caller-driven rasterization of an egocentric (cols × rows) window around
// the agent into a flat Float32Array. Tile sampler returns per-cell channel
// values; entity layers rasterize as additional channel planes by walking
// caller-supplied entity lists. No top-K cap, entities collide naturally
// into channels via accumulation (or "last write wins" in overwrite mode).
// A tail "self block" of caller-filled scalars is appended (velocities,
// flags, signed distances, whatever doesn't fit the grid).

/**
 * @param {Object} opts
 * @param {Object} opts.spec
 *   .colsBehind / .colsAhead / .rowsUp / .rowsDown, window extents
 *   .tileChannels, channels per tile cell (default 1)
 *   .selfBlockSize, tail floats appended for ego state (default 0)
 * @param {Object} [opts.tile]
 *   .channels, overrides spec.tileChannels
 *   .normalize, Float32Array per-tile-channel multiplier
 *   .oob, Float32Array filled into out-of-world cells
 *   .sample(col, row) → bool | number | Array | Float32Array
 *     Return false for OOB cells; otherwise the rasterizer writes either a
 *     bool/number broadcast across all channels, or an array of length
 *     tileChannels.
 * @param {Array<Object>} [opts.layers]: entity layers in z-order
 *   .channels, per-cell channels for this layer
 *   .overwrite, true => last write wins; false (default) => additive
 *   .normalize, per-channel multiplier applied AFTER accumulation
 *   .enumerate() → number, count of entities to walk
 *   .sample(i)   → { col, row, values }, values is per-channel array; or
 *                                          { col, row, value } for a
 *                                          single-channel layer shorthand
 * @returns {ObsWindow}
 */
const obsWin = bro.ai.game.grid.createObsWindow({
    spec: { colsBehind: 4, colsAhead: 11, rowsUp: 6, rowsDown: 6,
            tileChannels: 2, selfBlockSize: 8 },
    tile: {
        normalize: new Float32Array([1, 1]),
        sample(c, r) {
            const t = world.getTile(c, r);
            if (!t) return false;
            return [t.solid ? 1 : 0, t.hazard ? 1 : 0];
        },
    },
    layers: [{
        channels: 3,
        normalize: new Float32Array([1, 0.01, 0.5]),
        enumerate() { return mobs.length; },
        sample(i) {
            const m = mobs[i];
            return { col: m.col, row: m.row, values: [1, m.hp, m.kind] };
        },
    }],
});

obsWin.outDim;          // total Float32 length
obsWin.layout();        // {cols, rows, tileOffset, tileSize, layers:[{offset,channels,size}], selfOffset, selfSize, total}
const buf = obsWin.build(egoCol, egoRow,
    new Float32Array([vx, vy, jumping ? 1 : 0, /* ... */]));   // returns Float32Array of length outDim


// -----------------------------------------------------------------------------
// FrameStack, last-k frames concatenated for temporal context
// -----------------------------------------------------------------------------
//
// Stateful ring of k inner observations. read() returns the chronological
// concatenation [oldest, ..., newest]. reset() at episode boundaries,
// without it, the first decision sees the previous episode's tail.

/** @param {{innerDim, k}} opts @returns {FrameStack} */
const stack = bro.ai.game.grid.createFrameStack({ innerDim: obsWin.outDim, k: 4 });
stack.outDim;       // = innerDim * k
stack.filled;       // count of pushed frames since last reset (clamped at k)
stack.push(buf);
const stacked = stack.read();   // Float32Array length k * innerDim
stack.reset();      // call at episode start


// -----------------------------------------------------------------------------
// FailureTape, penalize repeated bad-state actions
// -----------------------------------------------------------------------------
//
// Records (signature, action) pairs from the tail of failed episodes.
// Emits floor-clamped penalty^count multipliers for use as a prior post-
// multiplier in GenericMcts.priorFn, discourages repeating mistakes at
// known-bad state signatures.

/** @param {{tapeDepth, ringCapacity, penalty, floor}} opts */
const tape = bro.ai.game.grid.createFailureTape({
    tapeDepth: 16,
    ringCapacity: 4096,
    penalty: 0.5,
    floor: 0.05,
});
tape.recordFailure([
    { sig: "5|hazard", action: 1 },
    { sig: "6|hazard", action: 1 },
    // ... last tapeDepth steps of the failed episode
]);
const m1 = tape.multipliers("5|hazard", /*numActions*/ 6);   // Float32Array
const adjusted = tape.applyPriors("5|hazard", netPrior);     // returns adjusted array
tape.size; tape.capacity; tape.clear();


// -----------------------------------------------------------------------------
// BestCrop, ranked snapshot pool for "search from historical bests"
// -----------------------------------------------------------------------------
//
// Bounded pool of (snapshot, action prefix, score, depth). Effective rank =
// score + depthBonus*depth - ageDecay*age. seed() picks uniformly from the
// top-K, feed its result to GenericMcts to start search at a historical
// good state instead of always replaying from spawn.

/** @param {{capacity, depthBonus, ageDecay, seedTopK, seed}} opts */
const crop = bro.ai.game.grid.createBestCrop({
    capacity: 64, depthBonus: 0.01, ageDecay: 0.001, seedTopK: 4,
    seed: 0xC0DE1234n,
});
crop.push({ snapshot: env.snapshot(), prefix: actionsTaken,
            score: episodeReturn, depth: ep_depth });
const pickedSeed = crop.seed();         // { snapshot, prefix }
if (pickedSeed.snapshot) {
    env.restore(pickedSeed.snapshot);
    for (const a of pickedSeed.prefix) env.step(a);
    const action = mcts.search();        // search from a deep good state
}
crop.size; crop.capacity; crop.clear();


// -----------------------------------------------------------------------------
// PotentialShaper / StallDetector, reward shaping helpers
// -----------------------------------------------------------------------------

/** Ng/Harada/Russell potential-based shaping: emit γΦ(s')-Φ(s) per step.
 *  Preserves the optimal policy of the underlying MDP. */
const shaper = bro.ai.game.grid.createPotentialShaper({ gamma: 0.99 });
shaper.reset(/*phi0*/ -distToGoal(state) * 0.01);
const bonus = shaper.step(/*phi'*/ -distToGoal(nextState) * 0.01);
// r_shaped = r + bonus

/** Stall detector, fires once a sliding window of progress samples has
 *  spread < epsilon. Use for early termination of stuck episodes. */
const stallDet = bro.ai.game.grid.createStallDetector({ epsilon: 0.5, patience: 60 });
stallDet.reset();
if (stallDet.tick(progress(state))) endEpisode("stalled");


// -----------------------------------------------------------------------------
// generateBC, heuristic policy → GenericSituation tuples
// -----------------------------------------------------------------------------
//
// Run a hand-coded policy from each starting snapshot, filter by minReturn,
// emit GenericSituations with one-hot policy targets and per-step return-to-go
// as the value target. Use to escape cold-start traps where a freshly-
// initialized net never stumbles into reward.

/** @param {Object} opts
 *  @param {Object} opts.env: same shape as createGenericMcts env (numActions, snapshot, restore, step, legalActions, observe)
 *  @param {function(obs, legal): number} opts.heuristic
 *  @param {Array<any>} opts.starts: env-snapshot objects
 *  @param {number} [opts.minReturn=0]
 *  @param {number} [opts.rolloutHorizon=256]
 *  @param {number} [opts.gamma=0.99]
 *  @param {boolean} [opts.clipValue=true]
 *  @returns {Array<GenericSituation>}
 */
const bcSits = bro.ai.game.grid.generateBC({
    env: { numActions, snapshot, restore, step, legalActions, observe },
    heuristic: (obs, legal) => pickGreedyAction(obs, legal),
    starts: [snap1, snap2, snap3],
    minReturn: 0.5, rolloutHorizon: 200, gamma: 0.99,
});
gtrainer.warmupWith(bcSits);


// -----------------------------------------------------------------------------
// GenericRecorder / GenericReplayReader, schema-driven .bgargrid replays
// -----------------------------------------------------------------------------
//
// Caller declares roster / frame / event schemas at open time (i32, i64, f32,
// f64). Recorder streams variable-count rows per frame; reader random-accesses
// any frame via the offset index appended at close. Distinct magic from .bgar
// so the existing combat-shaped reader isn't burdened.
//
// close() returns false when a write failed. The reader validates the file
// as docs/ai-game-learning.js describes for .bgar: counts are checked
// against sane limits and the bytes present before allocating, the index
// and footer must be where the writer put them, frame offsets must lie in
// the frame stream (64-bit, so files past 2 GiB work), an unknown field type
// or a roster row size that disagrees with the schema is rejected, and a
// failed open() (false) leaves the reader empty.

const rec = bro.ai.game.grid.createGenericRecorder();
rec.open("ep_0001.bgargrid", /*episodeId*/ 1n, /*seed*/ 0xC0DEn, /*dt*/ 1/60, {
    roster: [{name: "id", type: "i32"}, {name: "kind", type: "i32"}],
    frame:  [{name: "id", type: "i32"}, {name: "x", type: "f32"}, {name: "y", type: "f32"}],
    events: [{name: "src", type: "i32"}, {name: "amount", type: "f32"}],
});
rec.writeRoster([[0, 1], [1, 2]]);             // rows are arrays in schema order
rec.recordFrame(0n, 0.0,
    [[0, 1.0, 2.0], [1, 3.0, 4.0]],            // entities this frame
    [[1, 12.0]]);                               // events this frame
rec.recordFrame(1n, 1/60, [[0, 1.5, 2.5]], []);
rec.frameCount;
rec.close();

const rr = bro.ai.game.grid.createGenericReplayReader();
rr.open("ep_0001.bgargrid");
rr.frameCount;
const fr = rr.frame(0);                         // { stepIdx, elapsed, rows, events }
const xs = rr.trajectory(/*rowIndex*/ 0, "x"); // values for that field across all frames


// -----------------------------------------------------------------------------
// GridTrainer, owns net + buffer + trainer + best/failure trackers
// -----------------------------------------------------------------------------
//
// Wires the generic primitives into a complete training loop. Producers (self-
// play searches) call ingestSituation / ingestEpisode from any thread; the
// harness drains lock-free MPSC rings, runs SGD on the PolicyValueNet,
// publishes weights via a WeightsHandle, optionally writes a checkpoint ring
// + best.bin to disk, and emits weightsUpdated / bestRotated / episodeIngested
// events you can read with pollEvents() to drive UI / telemetry.
//
// Two modes:
//   - Async: start() spins a trainer thread; producers ingest from anywhere.
//             stop() joins.
//   - Sync:  stepSync(n) drains the rings and runs n SGD steps in line.

/** @param {Object} opts
 *  @param {Object} opts.net: { inDim, hidden:[...], valueHidden, numActions, seed:bigint }
 *  @param {Object} [opts.buffer]: { capacity }
 *  @param {Object} [opts.trainer]: { lr, momentum, batch, policyWeight, valueWeight, publishEvery, rngSeed }
 *  @param {Object} [opts.ckpt]: { dir, ringSize, bestWindow }   omit for no disk writes
 *  @param {number} [opts.ingestBurst=64]
 *  @param {number} [opts.stepsPerTick=1]
 *  @returns {GridTrainer}
 */
const gtrainer = bro.ai.game.grid.createGridTrainer({
    net:     { inDim: stack.outDim, hidden: [128, 128], valueHidden: 64, numActions: 6, seed: 1n },
    buffer:  { capacity: 65536 },
    trainer: { lr: 1e-3, momentum: 0.9, batch: 64,
               policyWeight: 1.0, valueWeight: 1.0, publishEvery: 200 },
    ckpt:    { dir: "ckpts/", ringSize: 8, bestWindow: 50 },
});

gtrainer.warmupWith(bcSits);                    // optional BC warmup

// Producers, safe from any thread.
gtrainer.ingestSituation({
    obs:          stack.read(),
    policyTarget: mcts.rootVisits(),
    actionMask:   maskForLegal,
    valueTarget:  clippedReturn,
});
gtrainer.ingestEpisode({
    totalReturn: r,
    depth:       t,
    failed:      r <= 0,
    snapshot:    episodeStartSnapshot,          // optional → BestCrop on success
    prefix:      actionsTaken,                   // optional replay path
    tail:        [/* {sig, action} ... */],     // optional → FailureTape on failure
});

gtrainer.start();                               // async mode
// ... or
gtrainer.stepSync(/*sgdSteps*/ 16);             // sync mode

for (const ev of gtrainer.pollEvents()) {
    if (ev.kind === "weightsUpdated")  notifySearchers(ev.version);
    if (ev.kind === "bestRotated")     ui.markCheckpoint(ev.path, ev.meanReturn);
    if (ev.kind === "episodeIngested") metrics.push(ev.meanReturn);
}

gtrainer.stats();
//   { totalSteps, totalPublishes, episodesIngested,
//     trailingMeanReturn, bestMeanReturn, bufferSize, running }

gtrainer.stop();
