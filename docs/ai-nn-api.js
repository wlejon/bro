// =============================================================================
// bro.ai.game.nn, Neural-network primitives for game AI
// =============================================================================
//
// The training half of the brogameagent stack: dense circuits, the bundled
// policy/value nets, the free forward/backward ops, the factored-action
// helpers, and the lock-free weights handle that hands new parameters to a
// running searcher.
//
// Companion files (one namespace each, same library):
//   docs/ai-game-api.js       NavGrid / HexNav / NavMesh / routing
//   docs/ai-game-planning.js  Agent, World, Unit, steering, perception, bindings
//   docs/ai-game-learning.js  MCTS family, planners, belief, simulation, replay
//   docs/ai-learn-api.js      bro.ai.game.learn: buffers, trainers, inference
//   docs/ai-game-tools.js     bro.ai.game.grid: obs windows, tapes, GridTrainer
//
// Available in all modes, windowed, headless and bro-server, and inside a
// Worker (each realm installs its own copy of the classes).
//
// Feature gate: the whole namespace is compiled out of a build without the
// tensor tower. Test it before using anything here:
//
//   if (bro.ai.game.nn.available === false) return;   // compiled out
//
// A compiled-in build does NOT define `available`; only the stub does. That
// mirrors how the other optional towers report themselves.
//
// -----------------------------------------------------------------------------
// Tensors: there is no AI tensor type any more
// -----------------------------------------------------------------------------
//
// The old surface had its own `AITensor` class with rows/cols/get/set/
// toArray/fromArray/copyFrom. It is GONE. Every argument that used to take
// one now takes either:
//
//   - a bro.tensor GpuTensor handle (see docs/tensor-api.js), or
//   - a plain Float32Array, which the binding views IN PLACE as an
//     `n x 1` CPU tensor for the duration of the call (no copy, no
//     allocation, and writes land back in your array).
//
// `nn.createTensor(rows, cols=1)` is kept as a convenience factory, but what
// it returns is an ordinary GpuTensor, so its shape/element API is
// bro.tensor's, not this namespace's. One tensor type in the whole stack.
//
// The mask arguments (softmaxForward, factoredSoftmax, factoredXent, ...) are
// the exception: they are read as a raw float buffer and must be
// Float32Arrays, never tensors.
//
// Quick start, a hand-wired 2-layer net trained by SGD:
//
//   const nn = bro.ai.game.nn;
//   const x  = nn.createTensor(8, 1),  h = nn.createTensor(16, 1);
//   const y  = nn.createTensor(4, 1);
//   const l1 = nn.createLinear(8, 16, 0xC0DEn);
//   const r1 = nn.createRelu();
//   const l2 = nn.createLinear(16, 4, 0xC0DFn);
//
//   l1.forward(x, h); r1.forward(h, h); l2.forward(h, y);
//   // ... fill dY from a loss ...
//   l2.backward(dY, dH); r1.backward(dH, dH); l1.backward(dH, dX);
//   l1.sgdStep(0.01, 0.9); l2.sgdStep(0.01, 0.9);
//   l1.zeroGrad();        l2.zeroGrad();
//
// =============================================================================


// -----------------------------------------------------------------------------
// Tensor factory
// -----------------------------------------------------------------------------

/**
 * Allocate an `rows x cols` tensor. Returns a bro.tensor GpuTensor (see
 * docs/tensor-api.js for its own methods); this namespace only allocates it.
 *
 * @param {number} rows
 * @param {number} [cols=1]
 * @returns {GpuTensor}
 * @throws {Error} on a negative dim, or when bro.tensor's natives are not
 *     installed in this realm
 */
const W = bro.ai.game.nn.createTensor(4, 3);
const v = bro.ai.game.nn.createTensor(8);          // 8 x 1 column

// Anywhere a tensor is expected, a Float32Array works too, viewed in place:
const xs = new Float32Array(8);
bro.ai.game.nn.reluForward(xs, xs);                // writes back into xs


// -----------------------------------------------------------------------------
// Circuits: Linear, Relu, Tanh
// -----------------------------------------------------------------------------
//
// Every circuit shares one shape: forward(x, y) writes into `y`,
// backward(dY, dX) consumes the cache the matching forward left behind and
// writes the input gradient into `dX`, accumulating parameter gradients.
// zeroGrad() clears the accumulators, sgdStep(lr, momentum) applies them.
// Each circuit owns its own cache, so a circuit can only be backwarded
// against its most recent forward.
//
// save() returns a Uint8Array blob; load(blob) restores it. Blobs are
// per-class: a Linear blob is not a ValueHead blob, and a malformed one
// throws rather than corrupting the circuit.

/**
 * Dense layer. `W` is (outDim, inDim), `b` is (outDim, 1).
 * Passing dims constructs it initialized; passing none leaves it unsized
 * for a later init()/load().
 *
 * @param {number} [inDim]
 * @param {number} [outDim]
 * @param {bigint|number} [seed=0xC0DE1234n]
 * @returns {AILinear}
 */
const lin = bro.ai.game.nn.createLinear(8, 16, 0xC0DEn);

/**
 * (Re)initialize in place with fresh Xavier weights.
 * @param {number} inDim
 * @param {number} outDim
 * @param {bigint|number} [seed=0xC0DE1234n]
 * @returns {bigint} the seed actually used
 */
lin.init(8, 16, 0xC0DEn);

lin.forward(x, y);            // y = W·x + b
lin.backward(dY, dX);         // accumulates dW/dB, writes dX
lin.zeroGrad();
lin.sgdStep(/*lr*/ 0.01, /*momentum*/ 0.9);

lin.name;                     // "Linear"
lin.inDim; lin.outDim;        // shape
lin.numParams;                // inDim*outDim + outDim

// Parameter and gradient tensors. Each read returns a fresh GpuTensor COPY
// (a snapshot), not an alias — writing into it does not touch the circuit.
lin.W; lin.b; lin.dW; lin.dB;

const blob = lin.save();      // Uint8Array
lin.load(blob);

/**
 * Elementwise activations. Stateless apart from the backward cache, so they
 * take no dims and no seed.
 * @returns {AIRelu|AITanh}
 */
const relu = bro.ai.game.nn.createRelu();
const tanh = bro.ai.game.nn.createTanh();
relu.forward(h, h);           // in-place is fine
relu.backward(dH, dH);
relu.name;                    // "Relu" / "Tanh"
relu.numParams;               // 0
// zeroGrad()/sgdStep() exist and are no-ops, so an activation drops into the
// same loop as a parameterised circuit without a special case.


// -----------------------------------------------------------------------------
// DeepSetsEncoder, permutation-invariant entity encoder
// -----------------------------------------------------------------------------
//
// Encodes self + a variable-length set of enemies + a variable-length set of
// allies into one fixed embedding, so the network is invariant to the order
// the entities happen to be listed in.

/**
 * @param {Object} [cfg]
 * @param {number} [cfg.hidden]    - per-entity hidden width
 * @param {number} [cfg.embedDim]  - per-pool embedding width
 * @param {bigint|number} [seed=0xC0DE1234n]
 * @returns {AIDeepSetsEncoder}
 */
const enc = bro.ai.game.nn.createDeepSetsEncoder({ hidden: 32, embedDim: 32 }, 0xC0DEn);

/** Re-init in place. @returns {bigint} the seed used */
enc.init({ hidden: 32, embedDim: 32 }, 0xC0DEn);

enc.name;                      // "DeepSetsEncoder"
enc.outDim;                    // 3 * embedDim (self | enemies | allies)
enc.numParams;

enc.forward(obsVec, embed);    // obsVec is the OBS_TOTAL observation
enc.backward(dEmbed, dObs);
enc.zeroGrad(); enc.sgdStep(lr, momentum);
enc.load(enc.save());


// -----------------------------------------------------------------------------
// Heads: ValueHead, FactoredPolicyHead
// -----------------------------------------------------------------------------

/**
 * Scalar value head, tanh-squashed into [-1, 1].
 * @param {number} [embedDim]
 * @param {number} [hidden]
 * @param {bigint|number} [seed=0xC0DE1234n]
 * @returns {AIValueHead}
 */
const vHead = bro.ai.game.nn.createValueHead(96, 32, 0xC0DEn);
vHead.init(96, 32, 0xC0DEn);          // @returns {bigint}

/** forward takes ONE argument and RETURNS the scalar (it writes no tensor).
 *  @param {GpuTensor|Float32Array} embed
 *  @returns {number} value in [-1, 1] */
const val = vHead.forward(embed);

/** backward takes the scalar value gradient plus the embedding-gradient sink.
 *  @param {number} dValue
 *  @param {GpuTensor|Float32Array} dEmbed */
vHead.backward(dValue, dEmbed);
vHead.name; vHead.numParams;
vHead.zeroGrad(); vHead.sgdStep(lr, momentum);
vHead.load(vHead.save());

/**
 * Factored policy head: one logit block per action factor (move direction,
 * attack target slot, ability slot) rather than one flat softmax over their
 * product. Cheap for combinatorial action spaces.
 *
 * @param {number} [embedDim]
 * @param {bigint|number} [seed=0xC0DE1234n]
 * @returns {AIFactoredPolicyHead}
 */
const pHead = bro.ai.game.nn.createFactoredPolicyHead(96, 0xC0DEn);
pHead.init(96, 0xC0DEn);              // @returns {bigint}
pHead.totalLogits;                    // N_MOVE + N_ATTACK + N_ABILITY
pHead.name; pHead.numParams;

pHead.forward(embed, logits);         // writes totalLogits values
pHead.backward(dLogits, dEmbed);
pHead.zeroGrad(); pHead.sgdStep(lr, momentum);
pHead.load(pHead.save());

// Head widths, mirrored as namespace constants so a caller can size buffers
// without instantiating a head.
bro.ai.game.nn.N_MOVE;      // 9  (hold + 8 compass directions)
bro.ai.game.nn.N_ATTACK;    // N_ENEMY_SLOTS + 1 (no-attack)
bro.ai.game.nn.N_ABILITY;   // ability slots + 1 (no-cast)


// -----------------------------------------------------------------------------
// SingleHeroNet, the bundled combat net
// -----------------------------------------------------------------------------
//
// DeepSetsEncoder → trunk → { ValueHead, FactoredPolicyHead }, shaped for the
// bundled observation (bro.ai.game.buildObservation) and factored combat
// action space. Plug it into learn.createNeuralEvaluator / createNeuralPrior
// (docs/ai-learn-api.js) to drive an MCTS.
//
// SingleHeroNet does NOT implement forwardBatched — it does not satisfy the
// batched-net interface. PolicyValueNet and SingleHeroNetTX do.

/**
 * @param {Object} [opts]
 * @param {Object} [opts.enc]             - {hidden, embedDim} for the encoder
 * @param {number} [opts.trunkHidden]
 * @param {number} [opts.valueHidden]
 * @param {bigint|number} [opts.seed]
 * @returns {AISingleHeroNet}
 */
const net = bro.ai.game.nn.createSingleHeroNet({
    enc: { hidden: 32, embedDim: 32 },
    trunkHidden: 64,
    valueHidden: 32,
    seed: 0xC0DEn,
});

/** forward(x, logits) → the scalar value; policy logits land in `logits`. */
const value = net.forward(x, logits);
/** backward(dValue, dLogits): dValue is a NUMBER, dLogits a tensor. */
net.backward(dValue, dLogits);
net.zeroGrad(); net.sgdStep(lr, momentum);
net.embedDim; net.trunkDim; net.policyLogits; net.numParams;
net.load(net.save());         // Uint8Array; a bad blob throws a TypeError


// -----------------------------------------------------------------------------
// PolicyValueNet, generic MLP for a flat discrete action space
// -----------------------------------------------------------------------------
//
// Decoupled from the combat-shaped observation/action layout: use it when
// your observation is hand-crafted and your actions are a small flat set
// (platformer buttons, grid moves, puzzle pieces). Architecture:
//
//   inDim → hidden[0] → ReLU → ... → hidden[n-1] → { value (tanh), logits }
//
// Wire-format magic differs from SingleHeroNet, so blobs are NOT
// interchangeable between the two.
//
// Multi-head mode: pass `headSizes` instead of (or as well as) `numActions`
// and the single logit vector is partitioned into per-factor blocks; the
// factored helpers at the bottom of this file convert between per-head
// choices and a flat action index.

/**
 * @param {Object} opts
 * @param {number}   opts.inDim               - required
 * @param {number[]} opts.hidden              - required, non-empty
 * @param {number}   opts.valueHidden         - required
 * @param {number}   [opts.numActions]        - one of numActions / headSizes
 * @param {number[]} [opts.headSizes]           is required
 * @param {bigint|number} [opts.seed]
 * @returns {AIPolicyValueNet}
 * @throws {TypeError} when inDim / hidden / valueHidden are missing, or
 *     neither numActions nor headSizes is given
 * @throws {RangeError} when headSizes breaks the head limits (at most 64
 *     heads, each 1..2^24, product within int32; see the multi-head helpers
 *     below)
 */
const pvnet = bro.ai.game.nn.createPolicyValueNet({
    inDim: 60,
    hidden: [64, 64],
    valueHidden: 32,
    numActions: 6,
    seed: 0xC0DE1234n,
});

pvnet.inDim; pvnet.numActions; pvnet.trunkDim; pvnet.numParams;
pvnet.numHeads;               // 1 for a flat head, else headSizes.length
pvnet.device;                 // "cpu" | "gpu"
pvnet.headSizes();            // Int32Array-shaped array of per-head widths
pvnet.headOffsets();          // n+1 prefix offsets into the logit vector

const valuePV = pvnet.forward(obsTensor, logitsTensor);   // returns the scalar
pvnet.backward(dValuePV, dLogitsTensor);                  // (number, tensor)
pvnet.zeroGrad(); pvnet.sgdStep(lr, momentum);
pvnet.load(pvnet.save());

/**
 * Move the net between devices. GPU-first, like the rest of the stack:
 * to("gpu") on a build with no GPU backend THROWS rather than quietly
 * staying on the CPU, so a training loop cannot silently run 100x slow.
 * @param {"cpu"|"gpu"} device
 */
pvnet.to("gpu");

/**
 * Evaluate B rows in one dispatch instead of B. `x` is (B, inDim); the
 * caller pre-sizes `logits` as (B, numActions) and `values` as (B, 1).
 * This is the win for a single-threaded JS caller that gathers several
 * leaves within one tick — the inference SERVER (docs/ai-learn-api.js) only
 * pays off across concurrent callers.
 */
const xB = bro.ai.game.nn.createTensor(8, pvnet.inDim);
const logitsB = bro.ai.game.nn.createTensor(8, pvnet.numActions);
const valuesB = bro.ai.game.nn.createTensor(8, 1);
pvnet.forwardBatched(xB, logitsB, valuesB);


// -----------------------------------------------------------------------------
// SingleHeroNetTX, transformer trunk over entity slots
// -----------------------------------------------------------------------------
//
// Self-attention over the entity slots instead of the deep-sets pooling:
// a slot projection into `dModel`, `numBlocks` transformer blocks with
// `numHeads` attention heads and a `dFf` feed-forward, then the same
// value/policy pair. Heavier than SingleHeroNet, and the only net here with
// a built-in Adam optimizer.

/**
 * @param {Object} [opts]
 * @param {number} [opts.selfHidden]   - width of the self-feature MLP
 * @param {number} [opts.slotProj]     - per-entity slot projection width
 * @param {number} [opts.dModel]       - transformer model width
 * @param {number} [opts.dFf]          - feed-forward width
 * @param {number} [opts.numHeads]     - attention heads per block
 * @param {number} [opts.numBlocks]    - transformer blocks
 * @param {number} [opts.trunkHidden]
 * @param {number} [opts.valueHidden]
 * @param {bigint|number} [opts.seed]
 * @returns {AISingleHeroNetTX}
 */
const tx = bro.ai.game.nn.createSingleHeroNetTX({
    slotProj: 32, dModel: 64, dFf: 128,
    numHeads: 4, numBlocks: 2,
    trunkHidden: 64, valueHidden: 32,
    seed: 0xC0DEn,
});

tx.inDim; tx.numActions; tx.numHeads; tx.numParams; tx.device;
tx.headSizes(); tx.headOffsets();
const valueTx = tx.forward(x, logits);
tx.backward(dValue, dLogits);
tx.forwardBatched(xB, logitsB, valuesB);
tx.zeroGrad(); tx.sgdStep(lr, momentum);
tx.to("gpu");
tx.load(tx.save());

/**
 * Adam. Unlike sgdStep this one is stateful across calls, so `step` must be
 * the running 1-based update count (it drives the bias correction).
 * @param {number} lr
 * @param {number} beta1
 * @param {number} beta2
 * @param {number} eps
 * @param {number} step - 1-based update index
 */
tx.adamStep(3e-4, 0.9, 0.999, 1e-8, ++stepCount);


// -----------------------------------------------------------------------------
// WeightsHandle, hand new parameters to a running searcher
// -----------------------------------------------------------------------------
//
// A trainer publishes a blob + a version; readers snapshot it whenever they
// like. The handoff is atomic and lock-free, so a search thread never blocks
// on the trainer and never reads a half-written blob. Versions are BigInts.

const handle = bro.ai.game.nn.createWeightsHandle();

/** @param {Uint8Array|TypedArray} blob  @param {bigint|number} version */
handle.publish(net.save(), 1n);

/** @returns {{blob: Uint8Array, version: bigint}|null} null before the
 *  first publish */
const snap = handle.snapshot();
if (snap) net.load(snap.blob);

/** @returns {bigint} the currently published version (0n before any) */
handle.version();


// -----------------------------------------------------------------------------
// Free ops, the brotensor primitives one-for-one
// -----------------------------------------------------------------------------
//
// Every tensor argument may be a GpuTensor or a Float32Array. Every mask
// argument must be a Float32Array (or omitted / null / undefined for "no
// mask"). All of these write in place and throw on a shape mismatch.

bro.ai.game.nn.linearForward(W, b, x, y);                 // y = W·x + b
bro.ai.game.nn.linearBackward(W, x, dY, dX, dW, dB);
bro.ai.game.nn.reluForward(x, y);
bro.ai.game.nn.reluBackward(x, dY, dX);                   // note: takes x
bro.ai.game.nn.tanhForward(x, y);
bro.ai.game.nn.tanhBackward(y, dY, dX);                   // note: takes y
bro.ai.game.nn.softmaxForward(logits, probs, maskOrNull);
bro.ai.game.nn.softmaxBackward(probs, dProbs, dLogits);

/** Fused masked softmax + cross-entropy. `dLogits` receives (probs - target).
 *  @returns {number} the loss */
const loss = bro.ai.game.nn.softmaxXent(logits, target, probs, dLogits, maskOrNull);

/** Scalar MSE against a target. @returns {{loss: number, dPred: number}} */
const mseR = bro.ai.game.nn.mseScalar(pred, targetValue);

bro.ai.game.nn.addInplace(y, x);            // y += x
bro.ai.game.nn.addScalarInplace(y, 0.5);    // y += 0.5

/** Xavier-initialize a weight tensor.
 *  @param {GpuTensor|Float32Array} W
 *  @param {bigint|number} [seed=0xC0DE1234n]
 *  @returns {bigint} the seed used (feed it forward to chain inits) */
const seed2 = bro.ai.game.nn.xavierInit(W, 0xC0DEn);

/** Factored (per-block) softmax with optional attack/ability masks. */
bro.ai.game.nn.factoredSoftmax(logits, probs, atkMaskOrNull, abilMaskOrNull);

/** Factored cross-entropy over the three target blocks.
 *  @returns {number} the summed loss */
const fLoss = bro.ai.game.nn.factoredXent(
    logits, targetMove, targetAttack, targetAbility,
    probs, dLogits, atkMaskOrNull, abilMaskOrNull);


// -----------------------------------------------------------------------------
// Multi-head action helpers (Float32Array / plain-array in, no tensors)
// -----------------------------------------------------------------------------
//
// A multi-head net emits one logit vector partitioned by `headSizes`. A flat
// search (GenericMcts, a flat replay buffer) wants a single action index over
// the PRODUCT of the head sizes. These four convert between the two, the way
// a trainer worker shuffles data. They take Float32Arrays and plain integer
// arrays, never tensors.
//
// Head sizes are checked wherever a `headSizes` list is taken (these four
// and createPolicyValueNet): at most 64 heads, every head an integer in
// 1..2^24, and their product (the flat action count) within int32
// (2^31 - 1). Anything else is a RangeError naming the argument: a zero head
// used to divide by zero in decodeFlatAction, and an overflowing product
// sized the flat buffers wrong.

/**
 * Turn per-head logits into a flat prior over prod(headSizes): softmax each
 * head, then multiply the per-head probabilities of every combination.
 *
 * @param {Float32Array} logits    - at least sum(headSizes) entries
 * @param {number[]|Int32Array} headSizes - non-empty; head limits above
 * @param {Float32Array} flatPrior - OUT, at least prod(headSizes) entries
 * @param {Float32Array} [headMasks] - sum(headSizes) legality mask; null/
 *     undefined means everything is legal
 * @throws {TypeError} when a buffer is shorter than its head layout requires
 * @throws {RangeError} for a head size outside 1..2^24, more than 64 heads,
 *     or a head-size product past int32
 */
bro.ai.game.nn.factoredToFlat(logits, [9, 6, 3], flatPrior, headMasks);

/** @param {number[]|Int32Array} headSizes @returns {number} prod(headSizes)
 *  @throws {RangeError} under the same head limits (e.g. [65536, 65536]
 *      overflows int32) */
const total = bro.ai.game.nn.flatActionCount([9, 6, 3]);   // 162

/** Flat index → per-head choices.
 *  @returns {number[]} one entry per head
 *  @throws {RangeError} for a flat index outside 0..prod(headSizes) - 1, or
 *      head sizes outside the limits */
const perHead = bro.ai.game.nn.decodeFlatAction(77, [9, 6, 3]);

/** Per-head choices → flat index. Inverse of decodeFlatAction.
 *  @throws {TypeError} when perHead.length !== headSizes.length
 *  @throws {RangeError} when perHead[i] is outside 0..headSizes[i] - 1, or
 *      head sizes outside the limits */
const flat = bro.ai.game.nn.encodeFlatAction(perHead, [9, 6, 3]);
