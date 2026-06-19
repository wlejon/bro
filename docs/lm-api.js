/**
 * bro.lm — Language-model text generation (Qwen3, Mistral 3.1, Gemma-2,
 *          Qwen3.5) + NLLB-200 machine translation
 *
 * Backed by brolm (tokenizers + transformer text models) on top of brotensor.
 * Defaults to CUDA; pass { device: 'cpu' } to force the CPU backend.
 *
 * Generation model families (plus the NLLB-200 translator — see its section):
 *   - Qwen3 (loadQwen): GGUF checkpoint, Qwen BPE tokenizer, ChatML chat
 *     template. The original surface — most of this file documents it.
 *   - Mistral 3.1 text (loadMistral): quantized GGUF + the native "tekken"
 *     tokenizer (tekken.json), [INST] chat template. Returns the same
 *     { model, tokenizer } pair, and the model speaks the same LMModel API
 *     (generate / generateStream / async bro.lm.generate / cache control);
 *     model.family distinguishes 'qwen3' from 'mistral3'. See the Mistral
 *     section at the bottom.
 *   - Gemma-2 (loadGemma2): HF checkpoint dir (config.json + tokenizer.json +
 *     *.safetensors shards) — google/gemma-2-2b, added to brolm as Sana's text
 *     encoder. Same { model, tokenizer } pair and LMModel API (family
 *     'gemma2'); the base PT tokenizer has no chat template. See the Gemma-2
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


// ── Gemma-2 ───────────────────────────────────────────────────────────────────

/**
 * Load Gemma-2 (google/gemma-2-2b family) from a HuggingFace checkpoint
 * directory: config.json + tokenizer.json + one or more *.safetensors shards.
 * Gemma-2 was added to brolm as Sana's text encoder (its last_hidden_state
 * conditions the Sana DiT — see bro.diffusion.loadModel for Sana txt2img), but
 * it is a full causal LM and is bound here as one.
 *
 * Unlike loadMistral, the tokenizer travels with the checkpoint (tokenizer.json
 * in the same dir), so there is no separate tokenizerPath. Like loadQwen35 this
 * is a directory load, but it returns the shared LMModel + a GemmaTokenizer
 * handle (the model does NOT own tokenization).
 *
 * @param {string} modelDir            - HF gemma-2-2b dir (config.json,
 *                                       tokenizer.json, *.safetensors shards).
 * @param {Object} [opts]
 * @param {string} [opts.device='cuda']
 * @param {function} [opts.onReady]    - async load: onReady({model, tokenizer}).
 * @param {function} [opts.onError]
 * @returns {{ model: LMModel, tokenizer: GemmaTokenizer }}
 *
 * GemmaTokenizer (SentencePiece-BPE, byte-fallback):
 * @property {number} eosId, bosId, padId, unkId, vocabCount
 * @method encode(text, addBos=true) → Int32Array
 *         Prepends <bos> by default (no <eos>), matching GemmaTokenizer. Pass
 *         false to suppress the leading <bos>.
 * @method decode(ids) → string
 *         There is NO applyChatTemplate — gemma-2-2b ships as a base PT model.
 *         For an -it checkpoint build the turn framing in JS (the added tokens
 *         are matched verbatim):
 *           '<start_of_turn>user\n' + text +
 *           '<end_of_turn>\n<start_of_turn>model\n'
 *
 * The returned model is the same LMModel class as loadQwen's (family 'gemma2')
 * — generate, generateStream, and async bro.lm.generate all work.
 */
const gem = bro.lm.loadGemma2('../brolm/weights/gemma-2-2b');
const gIds = gem.model.generate(
    gem.tokenizer.encode('The capital of France is'), {
        maxNewTokens: 16,
        eosId: gem.tokenizer.eosId,
        sampling: { temperature: 0 },
    });
console.log(gem.tokenizer.decode(gIds));


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
 *                 images, onToken(id) → return false to stop early }
 *
 *         VISION INPUT — opts.images is an ImageBitmap / ImageData
 *         ({data,width,height}, RGBA) or an array of them. The prompt must
 *         already contain one placeholder triple per image, in the position the
 *         image should appear:
 *           <|vision_start|><|image_pad|><|vision_end|>
 *         Images are consumed in order. Supported on the async
 *         bro.lm.generate(q35, prompt, { images, onToken, onDone }) too.
 *
 * Async: bro.lm.generate(q35, promptString, opts) — same streaming/cancel
 * contract as the LMModel form (opts.images supported there too).
 */
const q35 = bro.lm.loadQwen35('../brolm/weights/Qwen3.5-0.8B');
const ids35 = q35.generate(
    '<|im_start|>user\nOne-word answer: capital of France?<|im_end|>\n<|im_start|>assistant\n',
    { maxNewTokens: 16, sampling: { temperature: 0 } });
console.log(q35.decode(ids35));

// Vision: caption an image (the prompt carries the image placeholder triple).
const visBmp = await createImageBitmap(someImageData);
const visIds = q35.generate(
    '<|im_start|>user\n<|vision_start|><|image_pad|><|vision_end|>' +
    'Describe this image.<|im_end|>\n<|im_start|>assistant\n',
    { maxNewTokens: 64, images: visBmp, sampling: { temperature: 0 } });
console.log(q35.decode(visIds));


// ── NLLB-200 — encoder-decoder machine translation ──────────────────────────────

/**
 * Load an NLLB-200 checkpoint directory (the converted HF layout: config.json,
 * tokenizer.json, model.safetensors — e.g. nllb-200-distilled-600M). brolm's
 * Translator owns the SentencePiece-metaspace-BPE tokenizer, the M2M-100
 * encoder-decoder transformer, and beam search; the model translates between any
 * pair of the 200+ FLORES-200 languages. CUDA by default; CPU FP32 otherwise.
 *
 * @param {string} checkpointDir
 * @param {Object} [opts]
 * @param {string} [opts.device='cuda']
 * @param {function} [opts.onReady]  - async load: onReady(model). The weights
 *        are ≈2.4 GB, so prefer the async form to keep the UI responsive.
 * @param {function} [opts.onError]
 * @returns {NllbModel}
 *
 * NllbModel:
 * @property {string} family         - 'nllb'.
 * @property {number} vocabSize, dModel, encoderLayers, decoderLayers
 * @property {number} languageCount  - number of FLORES-200 codes the tokenizer
 *           knows (200+).
 * @method hasLanguage(code) → boolean
 *         True if `code` (a FLORES-200 code like 'eng_Latn') is supported.
 * @method translate(text, srcLang, tgtLang, opts?) → string | AsyncHandle
 *         srcLang / tgtLang are FLORES-200 codes ('eng_Latn', 'fra_Latn',
 *         'spa_Latn', 'zho_Hans', 'arb_Arab', ...). Throws on an unknown code.
 *         opts: { numBeams=5, maxNewTokens=200, lengthPenalty=1.0 }
 *         SYNC by default — returns the translated string (blocks on the beam
 *         search). If opts.onDone is a function the search runs on a background
 *         thread and the call returns an AsyncHandle:
 *           opts.onDone(text)      - the translation, on the JS thread.
 *           opts.onError(message)  - on failure.
 *         handle.cancel() drops the pending result (the in-flight search is
 *         monolithic — it runs to completion but onDone is not fired).
 *         One translation per model at a time: a second concurrent call throws.
 */
const nllb = bro.lm.loadNllb('../brolm/weights/nllb-200-distilled-600M');
// Sync — short text, blocks until done.
console.log(nllb.translate('Hello, world!', 'eng_Latn', 'fra_Latn'));
// Async — keep the UI responsive on longer text.
const tr = nllb.translate('The quick brown fox jumps over the lazy dog.',
    'eng_Latn', 'spa_Latn', {
        numBeams: 5,
        onDone: (text) => console.log('translation:', text),
        onError: (e) => console.error('translate failed:', e),
    });
// tr.cancel();  // abandon the pending result if needed


// ── CLIP — ViT-L/14 cross-modal scorer (text ↔ image) ───────────────────────────

/**
 * Load the CLIP ViT-L/14 cross-modal scorer (openai/clip-vit-large-patch14):
 * the CLIP BPE tokenizer + text tower + image tower + the two cross-modal
 * projections, wrapped as brolm's CLIPScorer. Blocking (file IO + GPU upload),
 * matching the bro.diffusion loaders. CUDA by default.
 *
 * @param {Object} opts
 * @param {string} opts.vocabPath        - CLIP tokenizer vocab.json (required).
 * @param {string} opts.mergesPath       - CLIP tokenizer merges.txt (required).
 * @param {string} [opts.weightsPath]    - one safetensors holding the text tower
 *        (text_model.*), the vision tower (vision_model.*), and the projections
 *        (text_projection.weight / visual_projection.weight). The single-file
 *        CLIP export; used as the default for the three component paths below.
 * @param {string} [opts.textPath]       - load the text tower from its own file.
 * @param {string} [opts.imagePath]      - load the vision tower from its own file.
 * @param {string} [opts.projectionPath] - load the projections from their own file.
 * @param {string} [opts.textPrefix='text_model.']
 * @param {string} [opts.visionPrefix='vision_model.']
 * @param {string} [opts.projectionPrefix='']
 * @param {string} [opts.device='cuda']
 * @returns {ClipModel}
 *
 * ClipModel:
 * @property {number} projectionDim - shared cross-modal embedding dim (768).
 * @method encodeText(text|text[]) → Float32Array | Float32Array[]
 *         Projected, L2-normalised text feature(s) in the shared space.
 * @method score(text|text[], image) → number | number[]
 *         Cosine similarity in [-1, 1] between `image` (ImageBitmap / ImageData)
 *         and each prompt. Passing a text[] scores the one image against every
 *         candidate — the zero-shot-classification call (take the argmax).
 *
 * NOTE: the scorer exposes the projected TEXT feature and a fused
 * score(image)-vs-cached-prompt, but not a standalone projected IMAGE feature,
 * so there is no encodeImage()/score(textEmb, imageEmb) — the brolm CLIPScorer
 * surface has no public accessor for the projected image embedding.
 */
const clip = bro.lm.loadClip({
    vocabPath:   '../clip-vit-large-patch14/vocab.json',
    mergesPath:  '../clip-vit-large-patch14/merges.txt',
    weightsPath: '../clip-vit-large-patch14/model.safetensors',
});
// Zero-shot image classification: score one image against candidate labels.
const photo  = await createImageBitmap(someImageData);
const labels = ['a photo of a cat', 'a photo of a dog', 'a photo of a car'];
const scores = clip.score(labels, photo);
const best   = labels[scores.indexOf(Math.max(...scores))];
console.log('best match:', best, scores);
// Text embeddings (e.g. to cache / cluster prompts).
const emb = clip.encodeText('a photo of an astronaut riding a horse');


// ── T5 — encoder-only text encoder (T5-XXL, Flux's second text encoder) ──────────

/**
 * Load the T5 encoder (brolm's t5::TextEncoder). Defaults to the T5-XXL config —
 * Flux's second text encoder — but the architectural dims can be overridden for
 * smaller T5 variants. Blocking, matching the bro.diffusion loaders. CUDA by
 * default.
 *
 * @param {Object} opts
 * @param {string} opts.tokenizerPath      - SentencePiece-Unigram tokenizer.json
 *        (required).
 * @param {string} [opts.ggufPath]         - a T5 .gguf (config + weights read
 *        from the file; takes precedence and ignores the config overrides).
 * @param {string} [opts.weightsPath]      - a single safetensors (T5EncoderModel).
 * @param {string[]} [opts.shards]         - safetensors paths (the diffusers
 *        sharded T5-XXL). One of ggufPath / weightsPath / shards is required.
 * @param {string} [opts.prefix='']        - tensor-name prefix.
 * @param {number} [opts.maxLength=512]    - fixed encode() sequence length.
 * @param {boolean} [opts.quantizeWeights=false] - INT8 (W8A16) attn/FFN weights
 *        (GPU-only; ignored on CPU).
 * @param {Object} [opts.config]           - override the T5-XXL defaults for a
 *        safetensors/shard load: { vocabSize, dModel, dFf, dKv, numHeads,
 *        numLayers }.
 * @param {string} [opts.device='cuda']
 * @returns {T5Model}
 *
 * T5Model:
 * @property {number} dModel, maxLength, padId, eosId, vocabCount
 * @method encode(text, opts?) → { data: Float32Array, length, dim, ids: Int32Array }
 *         Encodes to a fixed-length sequence (eos + pad to maxLength), runs the
 *         encoder, and returns the (length × dim) hidden states flattened
 *         row-major. Padded positions are masked from attention; their rows are
 *         still returned so the buffer matches Flux's full-length conditioning.
 *         opts.maxLength overrides the handle default for this call.
 */
const t5 = bro.lm.loadT5({
    tokenizerPath: '../FLUX.1-schnell/tokenizer_2/tokenizer.json',
    shards: ['../FLUX.1-schnell/text_encoder_2/model-00001-of-00002.safetensors',
             '../FLUX.1-schnell/text_encoder_2/model-00002-of-00002.safetensors'],
    maxLength: 256,
});
const t5enc = t5.encode('a serene mountain lake at dawn');
console.log(t5enc.length, 'tokens ×', t5enc.dim, 'dims =', t5enc.data.length, 'floats');
