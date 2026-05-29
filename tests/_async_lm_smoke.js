// Smoke test for async, cancellable bro.lm.generate + async loadQwen.
// Run on the main (GPU) context:
//   bro-headless ../broworkshop/ai/voice-pipeline tests/_async_lm_smoke.js
// (path to the gguf mirrors voice-worker.js)

const GGUF = '../brolm/weights/Qwen3-8B-GGUF/Qwen3-8B-Q8_0.gguf';

// Bound by REAL wall-clock (Date.now() is real in headless), since the async
// jobs run on real background threads. sleep() advances virtual time and drives
// the engine tick (which calls tickAsync to drain results) but returns instantly,
// so the loop spins for real seconds, pumping delivery while the bg thread works.
function pumpUntil(pred, budgetMs) {
    const start = Date.now();
    while (!pred() && (Date.now() - start) < budgetMs) { sleep(20); }
    return pred();
}

// ── 1. async load ────────────────────────────────────────────────────────────
let loaded = null, loadErr = null;
const lh = bro.lm.loadQwen(GGUF, {
    onReady: (r) => { loaded = r; },
    onError: (m) => { loadErr = m; },
});
assert(lh && typeof lh.cancel === 'function', 'loadQwen async returns a handle');
assert(pumpUntil(() => loaded || loadErr, 120000), 'load finished within budget');
assert(!loadErr, 'load did not error: ' + loadErr);
assert(loaded.model && loaded.tokenizer, 'load yielded model + tokenizer');
const { model, tokenizer } = loaded;
console.log('[smoke] model loaded async');

const prompt = tokenizer.encode('Hello, how are you?', false);

// ── 2. streamed generation to completion ─────────────────────────────────────
let toksA = [], doneA = null;
bro.lm.generate(model, prompt, {
    maxNewTokens: 24,
    eosId: tokenizer.imEndId,
    sampling: { temperature: 0.7, topK: 40, topP: 0.95, seed: 1234 },
    onToken: (id) => { toksA.push(id); },
    onDone:  (ids, info) => { doneA = { ids, info }; },
});
assert(pumpUntil(() => doneA !== null, 60000), 'generation A completed');
assert(!doneA.info.cancelled, 'A not cancelled');
assert(toksA.length > 0, 'A streamed tokens via onToken');
assert(doneA.ids.length === toksA.length, 'A onDone ids match streamed count');
console.log('[smoke] gen A streamed ' + toksA.length + ' tokens: "' +
            tokenizer.decode(doneA.ids).slice(0, 60).replace(/\n/g, ' ') + '"');

// ── 3. cancel mid-generation ─────────────────────────────────────────────────
let toksB = [], doneB = null;
const hb = bro.lm.generate(model, prompt, {
    maxNewTokens: 200,
    eosId: tokenizer.imEndId,
    sampling: { temperature: 0.7, topK: 40, topP: 0.95, seed: 99 },
    onToken: (id) => {
        toksB.push(id);
        if (toksB.length === 5) hb.cancel();   // barge-in after 5 tokens
    },
    onDone: (ids, info) => { doneB = { ids, info }; },
});
assert(pumpUntil(() => doneB !== null, 60000), 'generation B completed after cancel');
assert(doneB.info.cancelled, 'B reports cancelled:true');
assert(doneB.ids.length < 30, 'B stopped early (got ' + doneB.ids.length + ' << 200)');
console.log('[smoke] gen B cancelled at ' + doneB.ids.length + ' tokens');

console.log('[smoke] PASS');
