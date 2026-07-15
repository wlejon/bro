// BatchedInferenceServer / DirectBackend / ServerBackend — device-neutral
// (works on CPU, no GPU required), so this runs unconditionally.

const G = bro.ai.game;
const nn = G.nn;
if (!nn || nn.available === false) {
    console.log('test_inference_server: gameai-nn not built (app profile), skipping');
} else {

const inDim = 4, numActions = 2;
const net = nn.createPolicyValueNet({ inDim, hidden: [8], valueHidden: 8, numActions, seed: 0xC0DEn });

function isEvalResult(r) {
    return r && typeof r === 'object' && r.logits instanceof Float32Array &&
           r.logits.length === numActions && typeof r.value === 'number' && Number.isFinite(r.value);
}

// createInferenceServer rejects a non-BatchedNet (SingleHeroNet).
{
    const shn = nn.createSingleHeroNet({ enc: { hidden: 8, embedDim: 8 }, trunkHidden: 16, valueHidden: 8, seed: 1n });
    let threw = false;
    try { G.learn.createInferenceServer(shn); } catch (e) { threw = true; }
    assert(threw, 'BUG: createInferenceServer(SingleHeroNet) should throw TypeError');
}

// InferenceServer.evaluate / evaluateBatch.
{
    const server = G.learn.createInferenceServer(net, { maxBatchSize: 8, maxWaitMicros: 200 });
    const obs = new Float32Array(inDim).fill(0.3);

    const r = server.evaluate(obs);
    assert(isEvalResult(r), 'server.evaluate returns {logits, value}: ' + JSON.stringify(r));

    const batch = server.evaluateBatch([obs, obs, obs]);
    assert(Array.isArray(batch) && batch.length === 3, 'evaluateBatch returns 3 results, got ' + batch.length);
    for (const row of batch) assert(isEvalResult(row), 'evaluateBatch row shape');
    assert(Math.abs(batch[0].value - r.value) < 1e-4, 'evaluateBatch value matches evaluate() for identical obs');

    assert(typeof server.batchesRun === 'number' && server.batchesRun > 0,
        'batchesRun counted, got ' + server.batchesRun);

    server.shutdown();
}

// DirectBackend / ServerBackend expose numActions/inDim.
{
    const direct = G.learn.createDirectBackend(net);
    assert(direct.numActions === numActions, 'DirectBackend.numActions');
    assert(direct.inDim === inDim, 'DirectBackend.inDim');

    const server = G.learn.createInferenceServer(net);
    const viaServer = G.learn.createServerBackend(server, net);
    assert(viaServer.numActions === numActions, 'ServerBackend.numActions');
    assert(viaServer.inDim === inDim, 'ServerBackend.inDim');
    server.shutdown();
}

// GenericMcts `backend` option: native prior/value fast path, no JS priorFn/
// valueFn callback. Deterministic 2-state env (same shape as
// test_generic_mcts.js) — action 0 always wins on reward, backend only
// supplies leaf guidance so real rollouts must still find it.
{
    const mctsNet = nn.createPolicyValueNet({ inDim: 1, hidden: [8], valueHidden: 8, numActions: 2, seed: 2n });
    const backend = G.learn.createDirectBackend(mctsNet);

    let state = 0;
    const env = {
        numActions: 2,
        snapshot() { return state; },
        restore(s) { state = s; },
        step(a) {
            if (state === 0 && a === 0) { state = 1; return { reward: 1, done: true }; }
            if (state === 0 && a === 1) { state = 0; return { reward: 0, done: false }; }
            return { reward: 0, done: true };
        },
        legalActions() { return state === 0 ? [0, 1] : []; },
        observe() { return new Float32Array([state]); },
    };

    const m = G.createGenericMcts({ env, backend, iterations: 300, cPuct: 1.5, gamma: 0.99, rolloutDepth: 4, seed: 42n });
    state = 0;
    const action = m.search();
    assert(typeof action === 'number', 'GenericMcts+backend search returns number, got ' + typeof action);
    assert(action === 0 || action === 1, 'action in legal range, got ' + action);
    assert(action === 0, 'GenericMcts+backend still finds the rewarding action, got ' + action);
}

console.log('test_inference_server: OK');
}
