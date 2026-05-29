/**
 * bro.lm — Language-model text generation (Qwen3)
 *
 * Loads a Qwen3 GGUF checkpoint and runs autoregressive text generation with
 * a KV cache. Backed by brolm (tokenizers + transformer text models) on top
 * of brotensor. Defaults to CUDA; pass { device: 'cpu' } to force the CPU
 * backend.
 *
 * A load returns two objects: a `model` (the transformer + KV cache) and a
 * `tokenizer` (BPE encode/decode + chat templating). They are paired — encode
 * a prompt with the tokenizer, generate with the model, decode the result.
 *
 * Generation is synchronous and blocks the JS thread for its duration; for a
 * 0.6B model on CUDA expect a few ms per token. Allocate the KV cache once
 * (promptLen + maxNewTokens) before the first generate().
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
