/**
 * bro.lm — Language-model text generation (Qwen3, Mistral 3.1, Qwen3.5)
 *
 * Backed by brolm (tokenizers + transformer text models) on top of brotensor.
 * Defaults to CUDA; pass { device: 'cpu' } to force the CPU backend.
 *
 * Three model families:
 *   - Qwen3 (loadQwen): GGUF checkpoint, Qwen BPE tokenizer, ChatML chat
 *     template. The original surface — most of this file documents it.
 *   - Mistral 3.1 text (loadMistral): quantized GGUF + the native "tekken"
 *     tokenizer (tekken.json), [INST] chat template. Returns the same
 *     { model, tokenizer } pair, and the model speaks the same LMModel API
 *     (generate / generateStream / async bro.lm.generate / cache control);
 *     model.family distinguishes 'qwen3' from 'mistral3'. See the Mistral
 *     section at the bottom.
 *   - Qwen3.5 (loadQwen35): safetensors checkpoint dir driven by brolm's VLM
 *     driver (hybrid full/linear-attention decoder, M-RoPE). The driver owns
 *     tokenization, so generate() takes a STRING prompt and the model exposes
 *     encode()/decode() itself — no separate tokenizer handle. See the
 *     Qwen3.5 section at the bottom.
 *
 * A loadQwen/loadMistral load returns two objects: a `model` (the transformer
 * + KV cache) and a `tokenizer` (BPE encode/decode + chat templating). They
 * are paired — encode a prompt with the tokenizer, generate with the model,
 * decode the result.
 *
 * Sampling defaults to temperature 1.0 (full sampling) for every family —
 * pass sampling: { temperature: 0 } for greedy decoding.
 *
 * Generation is synchronous and blocks the JS thread for its duration; for a
 * 0.6B model on CUDA expect a few ms per token. Allocate the KV cache once
 * (promptLen + maxNewTokens) before the first generate(). For non-blocking,
 * cancellable generation use bro.lm.generate(model, promptIds, opts) — the
 * async form every family shares (see the voice-pipeline app).
 */


// ── Load ──────────────────────────────────────────────────────────────────

/**
 * Load a Qwen3 model + tokenizer from a GGUF file.
 *
 * @param {string} ggufPath       - Path to a Qwen3 *.gguf checkpoint.
 *                                   Resolved against the app's base path.
 * @param {Object} [opts]
 * @param {string} [opts.device='cuda'] - 'cuda' or 'cpu'.
 * @returns {{ model: QwenModel, tokenizer: QwenTokenizer }}
 */
const { model, tokenizer } =
    bro.lm.loadQwen('../brolm/weights/Qwen3-0.6B-GGUF/Qwen3-0.6B-BF16.gguf');
// model.vocabSize === 151936, model.hiddenSize === 1024, model.numLayers === 28

/**
 * Load only a tokenizer (no model) — e.g. to count tokens or build prompts
 * ahead of loading the weights. (loadQwen already returns a tokenizer paired
 * with the model; use this when you want the tokenizer on its own.)
 *
 * @param {Object} opts
 * @param {string} opts.vocabPath  - path to vocab.json
 * @param {string} opts.mergesPath - path to merges.txt
 * @returns {QwenTokenizer}
 */
const tok = bro.lm.loadTokenizer({
    vocabPath:  '../brolm/weights/Qwen3-0.6B-GGUF/vocab.json',
    mergesPath: '../brolm/weights/Qwen3-0.6B-GGUF/merges.txt',
});


// ── Tokenizer ───────────────────────────────────────────────────────────────

/**
 * QwenTokenizer
 *
 * @property {number} imEndId   - id of the <|im_end|> turn terminator (use as eosId).
 * @property {number} imStartId - id of the <|im_start|> role marker.
 *
 * @method encode(text)  → number[]   - BPE-encode a string to token ids.
 * @method decode(ids)   → string     - Decode token ids back to text.
 * @method applyChatTemplate(messages, addGenerationPrompt) → string
 *         Render an array of { role, content } messages into the Qwen chat
 *         format. Pass addGenerationPrompt=true to append the assistant
 *         turn marker so the model continues as the assistant.
 */
const prompt = tokenizer.applyChatTemplate([
    { role: 'system', content: 'You are concise. Reply in one short sentence.' },
    { role: 'user',   content: 'Say hello to a new friend named Bro.' },
], /*addGenerationPrompt=*/true);
const promptIds = tokenizer.encode(prompt);


// ── Generate ──────────────────────────────────────────────────────────────

/**
 * QwenModel
 *
 * @property {number} vocabSize
 * @property {number} hiddenSize
 * @property {number} numLayers
 *
 * @method allocateCache(maxTokens)
 *         Size the KV cache. Call once with at least promptLen + maxNewTokens
 *         before generating; reused across generate() calls.
 *
 * @method generate(promptIds, opts) → number[]
 *         Run autoregressive generation and return ONLY the newly generated
 *         token ids (the prompt is not echoed back).
 *
 *         opts:
 *           maxNewTokens {number}        - hard cap on generated tokens.
 *           eosId        {number}        - stop when this id is produced
 *                                          (typically tokenizer.imEndId).
 *           sampling     {Object}        - { temperature, topK, topP, seed }.
 *                                          Omit for greedy decoding.
 */
model.allocateCache(promptIds.length + 64);
const newIds = model.generate(promptIds, {
    maxNewTokens: 40,
    eosId:        tokenizer.imEndId,
    sampling:     { temperature: 0.7, topK: 40, topP: 0.95, seed: 42 },
});
const reply = tokenizer.decode(newIds);
console.log(reply.trim());


// ── Generate (streaming) ────────────────────────────────────────────────────

/**
 * QwenModel.generateStream(promptIds, opts, onToken) → number[]
 *
 *   Same prefill + decode loop as generate(), but invokes onToken(id) after
 *   each sampled token so callers can stream text as it is produced. The KV
 *   cache persists across the per-token forwards, so this is O(n) — do NOT
 *   emulate streaming by calling generate() in a loop (it resets the cache and
 *   re-prefills the prompt each call).
 *
 *   opts: same shape as generate() (maxNewTokens, eosId, sampling).
 *   onToken(id): called once per generated token. Return false to stop early.
 *                The eos token is neither passed to onToken nor included in the
 *                returned ids.
 *   Returns all newly generated ids (same as generate() would).
 *
 *   Tokens are byte-level BPE pieces that may be partial UTF-8, so decode
 *   incrementally by accumulating ids and emitting the delta of the full decode
 *   (rather than decoding one id at a time).
 */
const acc = [];
let shown = '';
model.generateStream(promptIds, {
    maxNewTokens: 40,
    eosId:        tokenizer.imEndId,
    sampling:     { temperature: 0.7, topK: 40, topP: 0.95, seed: 42 },
}, (id) => {
    acc.push(id);
    const text = tokenizer.decode(acc);
    const delta = text.slice(shown.length);
    shown = text;
    if (delta) process.stdout.write(delta);   // or postMessage(delta) from a worker
    return true;                              // return false to stop early
});


// ── Async generation (all families) ──────────────────────────────────────────

/**
 * bro.lm.generate(model, promptIds, opts) → AsyncHandle
 *
 * Runs the same prefill + decode loop as generateStream(), but on a background
 * thread so the JS thread (and the app) stays responsive, with real
 * cancellation. opts.onToken(id) streams each token to the JS thread as it is
 * produced; opts.onDone(ids, info) fires once with the full Int32Array and
 * info = { cancelled, error? }. handle.cancel() stops the decode within one
 * token (barge-in). Works for Qwen3 and Mistral models (promptIds) and for
 * Qwen3.5 (pass the STRING prompt instead of promptIds).
 *
 * Throws if a generation is already in flight on this model (single-owner).
 */
const h = bro.lm.generate(model, promptIds, {
    maxNewTokens: 256,
    eosId: tokenizer.imEndId,
    sampling: { temperature: 0.7, topK: 40, topP: 0.95 },
    onToken: (id) => { /* live decode */ },
    onDone:  (ids, info) => { if (!info.cancelled) console.log(tokenizer.decode(ids)); },
});
// h.cancel();   // e.g. on barge-in


// ── Mistral 3.1 ───────────────────────────────────────────────────────────────

/**
 * Load the Mistral 3.1 text decoder from a quantized GGUF (Q4_K / Q6_K /
 * Q8_0) + its native tekken tokenizer. The quant matmul path is GPU-only —
 * loading works on CPU but the first forward throws without a GPU backend.
 *
 * @param {string} ggufPath           - the text .gguf (not the mmproj one).
 * @param {Object} opts
 * @param {string} opts.tokenizerPath - tekken.json (REQUIRED — Mistral has no
 *                                      vocab.json + merges.txt).
 * @param {string} [opts.device='cuda']
 * @param {function} [opts.onReady]   - async load: onReady({model, tokenizer}).
 * @param {function} [opts.onError]
 * @returns {{ model: LMModel, tokenizer: MistralTokenizer }}
 *
 * MistralTokenizer:
 * @property {number} eosId, bosId, vocabCount
 * @method encode(text, addSpecial=false) → Int32Array
 *         addSpecial prepends BOS (<s>). A bare prompt wants it; the output
 *         of applyChatTemplate does NOT (the template emits its own <s>).
 * @method decode(ids) → string
 * @method applyChatTemplate(messages, addGenerationPrompt=true) → string
 *         Mistral's [INST] template.
 *
 * The returned model is the same LMModel class as loadQwen's (family
 * 'mistral3') — generate, generateStream, and async bro.lm.generate all work.
 */
const mis = bro.lm.loadMistral(
    '../brolm/weights/Mistral-Small-3.1-24B-Instruct-2503-GGUF/mistralai_Mistral-Small-3.1-24B-Instruct-2503-Q4_K_M.gguf',
    { tokenizerPath: '../brolm/weights/Mistral-Small-3.1-24B-Instruct-2503/tekken.json' });
const mPrompt = mis.tokenizer.applyChatTemplate(
    [{ role: 'user', content: 'One-sentence fun fact about owls.' }], true);
const mIds = mis.model.generate(mis.tokenizer.encode(mPrompt, /*addSpecial=*/false), {
    maxNewTokens: 64,
    eosId: mis.tokenizer.eosId,
    sampling: { temperature: 0 },
});
console.log(mis.tokenizer.decode(mIds));


// ── Qwen3.5 ───────────────────────────────────────────────────────────────────

/**
 * Load a Qwen3.5 checkpoint directory (HF layout: config.json, vocab.json +
 * merges.txt, model.safetensors shard(s) — e.g. Qwen3.5-0.8B). Driven by
 * brolm's qwen35::VLM driver, which owns the tokenizer and the M-RoPE/hybrid
 * cache plumbing.
 *
 * @param {string} checkpointDir
 * @param {Object} [opts]
 * @param {number} [opts.maxSeqLen=4096] - KV/state capacity per generate call.
 * @param {string} [opts.device='cuda']
 * @param {function} [opts.onReady]      - async load: onReady(model).
 * @param {function} [opts.onError]
 * @returns {Qwen35Model}
 *
 * Qwen35Model:
 * @property {string} family       - 'qwen35'.
 * @property {number} vocabSize, hiddenSize, numLayers, maxSeqLen
 * @property {number} eosId, imEndId, endoftextId
 * @method encode(text, addSpecial?) → Int32Array
 * @method decode(ids) → string
 * @method generate(prompt, opts) → Int32Array
 *         prompt is a STRING — the driver tokenizes. Wrap chat turns in
 *         ChatML yourself:
 *           '<|im_start|>user\n' + text + '<|im_end|>\n<|im_start|>assistant\n'
 *         Generation stops on <|im_end|> / <|endoftext|> or maxNewTokens.
 *         opts: { maxNewTokens, sampling: {temperature, topK, topP, seed},
 *                 onToken(id) → return false to stop early }
 *
 * Async: bro.lm.generate(q35, promptString, opts) — same streaming/cancel
 * contract as the LMModel form.
 */
const q35 = bro.lm.loadQwen35('../brolm/weights/Qwen3.5-0.8B');
const ids35 = q35.generate(
    '<|im_start|>user\nOne-word answer: capital of France?<|im_end|>\n<|im_start|>assistant\n',
    { maxNewTokens: 16, sampling: { temperature: 0 } });
console.log(q35.decode(ids35));
