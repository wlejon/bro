// Smoke test for the new bro.lm model families — Qwen3.5 (loadQwen35, string
// prompts through brolm's VLM driver) and Mistral 3.1 text (loadMistral, GGUF
// + tekken tokenizer through the shared LMModel surface). The existing Qwen3
// path is covered by tests/_async_lm_smoke.js.
// Run against the minimal smoke app:
//   bro-headless tests/_smoke_app tests/_lm_models_smoke.js
// Needs ../brolm/weights/Qwen3.5-0.8B; the Mistral leg self-skips when the
// ~14 GB GGUF is absent.

function pumpUntil(pred, budgetMs) {
    const start = Date.now();
    while (!pred() && (Date.now() - start) < budgetMs) { sleep(20); }
    return pred();
}

const fs = require('fs');

const QWEN35_DIR   = '../brolm/weights/Qwen3.5-0.8B';
const MISTRAL_GGUF = '../brolm/weights/Mistral-Small-3.1-24B-Instruct-2503-GGUF/' +
    'mistralai_Mistral-Small-3.1-24B-Instruct-2503-Q4_K_M.gguf';
const MISTRAL_TOK  = '../brolm/weights/Mistral-Small-3.1-24B-Instruct-2503/tekken.json';

// ── 1. Qwen3.5: async load ───────────────────────────────────────────────────
let q35 = null, q35Err = null;
const qh = bro.lm.loadQwen35(QWEN35_DIR, {
    onReady: (m) => { q35 = m; },
    onError: (e) => { q35Err = e; },
});
assert(qh && typeof qh.cancel === 'function', 'loadQwen35 async returns a handle');
assert(pumpUntil(() => q35 || q35Err, 600000), 'qwen35 load finished');
assert(!q35Err, 'qwen35 load did not error: ' + q35Err);
assert(q35.family === 'qwen35', 'family qwen35');
assert(q35.vocabSize > 150000, 'vocabSize plausible (' + q35.vocabSize + ')');
assert(q35.imEndId > 0, 'tokenizer ids exposed');
console.log('[smoke] qwen35 loaded: ' + q35.numLayers + ' layers, hidden ' +
            q35.hiddenSize);

// encode/decode round-trip through the driver-owned tokenizer.
const rt = q35.decode(q35.encode('hello world'));
assert(rt === 'hello world', 'tokenizer round-trip (got "' + rt + '")');

// ── 2. Qwen3.5: sync generate (greedy completion) ────────────────────────────
const sync35 = [];
const ids35 = q35.generate('The capital of France is', {
    maxNewTokens: 8,
    sampling: { temperature: 0 },        // greedy
    onToken: (id) => sync35.push(id),
});
assert(ids35 instanceof Int32Array && ids35.length > 0, 'qwen35 generated tokens');
assert(sync35.length === ids35.length, 'sync onToken saw every token');
const text35 = q35.decode(ids35);
console.log('[smoke] qwen35 sync: "' + text35 + '"');
assert(/paris/i.test(text35), 'qwen35 knows Paris (got "' + text35 + '")');

// onToken early-stop: returning false halts after the first token.
const oneTok = q35.generate('The capital of France is', {
    maxNewTokens: 32,
    sampling: { temperature: 0 },
    onToken: () => false,
});
assert(oneTok.length === 1, 'onToken=false stops after one token (' + oneTok.length + ')');

// ── 3. Qwen3.5: async generate with streaming + cancel + busy guard ──────────
const streamed = [];
let asyncDone = null;
bro.lm.generate(q35, 'The capital of France is', {
    maxNewTokens: 8,
    sampling: { temperature: 0 },
    onToken: (id) => streamed.push(id),
    onDone:  (ids, info) => { asyncDone = { ids, info }; },
});
let threw = false;
try { bro.lm.generate(q35, 'x', { maxNewTokens: 4, onDone: () => {} }); }
catch (e) { threw = true; }
assert(threw, 'concurrent generate on one model throws');
assert(pumpUntil(() => asyncDone !== null, 300000), 'async generate completed');
assert(!asyncDone.info.error, 'async no error: ' + asyncDone.info.error);
assert(asyncDone.ids.length > 0 && streamed.length === asyncDone.ids.length,
       'async streamed every token');
assert(/paris/i.test(q35.decode(asyncDone.ids)), 'async transcript has Paris');
console.log('[smoke] qwen35 async: "' + q35.decode(asyncDone.ids) + '"');

let cancelDone = null;
const ch = bro.lm.generate(q35, 'Write a very long story about a dragon.', {
    maxNewTokens: 512,
    sampling: { temperature: 0.7 },
    onToken: () => { if (cancelDone === null) ch.cancel(); },
    onDone:  (ids, info) => { cancelDone = { ids, info }; },
});
assert(pumpUntil(() => cancelDone !== null, 300000), 'cancelled generate completed');
assert(cancelDone.info.cancelled, 'cancellation reported');
assert(cancelDone.ids.length < 64, 'cancel stopped early (' + cancelDone.ids.length + ' tokens)');
console.log('[smoke] qwen35 cancel after ' + cancelDone.ids.length + ' tokens');

// ── 4. Mistral 3.1 (self-skips without the 14 GB checkpoint) ─────────────────
if (!fs.existsSync(MISTRAL_GGUF) || !fs.existsSync(MISTRAL_TOK)) {
    console.log('[smoke] mistral leg SKIPPED (checkpoint not present)');
} else {
    let mis = null, misErr = null;
    bro.lm.loadMistral(MISTRAL_GGUF, {
        tokenizerPath: MISTRAL_TOK,
        onReady: (r) => { mis = r; },
        onError: (e) => { misErr = e; },
    });
    assert(pumpUntil(() => mis || misErr, 900000), 'mistral load finished');
    assert(!misErr, 'mistral load did not error: ' + misErr);
    assert(mis.model.family === 'mistral3', 'family mistral3');
    assert(mis.tokenizer.vocabCount > 100000, 'tekken vocab loaded');
    console.log('[smoke] mistral loaded: ' + mis.model.numLayers + ' layers');

    // Raw completion (BOS via addSpecial=true), greedy.
    const mIds = mis.model.generate(
        mis.tokenizer.encode('The capital of France is', true),
        { maxNewTokens: 8, eosId: mis.tokenizer.eosId,
          sampling: { temperature: 0 } });
    const mText = mis.tokenizer.decode(mIds);
    console.log('[smoke] mistral sync: "' + mText + '"');
    assert(/paris/i.test(mText), 'mistral knows Paris (got "' + mText + '")');

    // Chat template + the shared async path (LMModel dispatch).
    const chatText = mis.tokenizer.applyChatTemplate(
        [{ role: 'user', content: 'Name one primary color. One word only.' }], true);
    const chatIds = mis.tokenizer.encode(chatText, false);
    let mDone = null;
    const mStreamed = [];
    bro.lm.generate(mis.model, chatIds, {
        maxNewTokens: 16,
        eosId: mis.tokenizer.eosId,
        sampling: { temperature: 0 },
        onToken: (id) => mStreamed.push(id),
        onDone:  (ids, info) => { mDone = { ids, info }; },
    });
    assert(pumpUntil(() => mDone !== null, 300000), 'mistral async completed');
    assert(!mDone.info.error, 'mistral async no error: ' + mDone.info.error);
    assert(mDone.ids.length > 0 && mStreamed.length === mDone.ids.length,
           'mistral async streamed every token');
    console.log('[smoke] mistral chat: "' + mis.tokenizer.decode(mDone.ids) + '"');
}

console.log('[smoke] PASS');
