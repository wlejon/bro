/**
 * =============================================================================
 * bro.lm — Language-model text generation & cross-modal machine learning
 * =============================================================================
 *
 * Backed by brolm (tokenizers + transformer text models) on top of brotensor.
 * Defaults to CUDA; pass { device: 'cpu' } to force the CPU backend.
 *
 * Generation model families (plus the NLLB-200 translator and CLIP/T5 encoders):
 *   - Qwen3 (loadQwen): GGUF checkpoint, Qwen BPE tokenizer, ChatML chat
 *     template. The original surface.
 *   - Mistral 3.1 text (loadMistral): quantized GGUF + native "tekken"
 *     tokenizer (tekken.json), [INST] chat template. Returns paired { model, tokenizer }.
 *   - Gemma-2 (loadGemma2): HF checkpoint dir (config.json + tokenizer.json +
 *     *.safetensors shards), google/gemma-2-2b. Same { model, tokenizer } pair.
 *   - Qwen3.5 (loadQwen35): safetensors checkpoint dir driven by brolm's VLM
 *     driver (hybrid full/linear-attention decoder, M-RoPE).
 *   - Qwen3-VL (loadQwen3VL): safetensors checkpoint dir driven by brolm's VLM
 *     driver: dense (plain GQA, full-rotary M-RoPE) decoder plus vision tower.
 *   - NLLB-200 (loadNllb): machine translation between 200+ FLORES-200 languages.
 *   - CLIP (loadClip): ViT-L/14 cross-modal scorer (text <-> image similarity).
 *   - T5 (loadT5): encoder-only text encoder (T5-XXL, Flux text conditioning).
 *   - ModernBERT (loadModernBert): bidirectional text encoder (8192-token
 *     context, alternating global / 128-token sliding attention) for text
 *     embeddings; a Hugging Face ModernBERT directory or a Laya checkpoint's
 *     encoder.
 *   - Laya (loadLaya / loadLayaAsync): a realtime decision model — typed
 *     choice / score / yes-no questions about a state, calibrated
 *     probabilities in one forward pass, served by a request scheduler that
 *     batches concurrent calls across GPUs. Documented in laya-api.js: what
 *     it is good and bad at, escalation, latency under load.
 *
 * @example
 *   // --- Qwen3 Text Generation ----------------------------------------------
 *   const { model, tokenizer } = bro.lm.loadQwen('../brolm/weights/Qwen3-0.6B-GGUF/Qwen3-0.6B-BF16.gguf');
 *   const prompt = tokenizer.applyChatTemplate([
 *     { role: 'system', content: 'You are concise. Reply in one short sentence.' },
 *     { role: 'user', content: 'Say hello to a new friend named Bro.' },
 *   ], true);
 *   const promptIds = tokenizer.encode(prompt);
 *   model.allocateCache(promptIds.length + 64);
 *   const newIds = model.generate(promptIds, {
 *     maxNewTokens: 40,
 *     eosId: tokenizer.imEndId,
 *     sampling: { temperature: 0.7, topK: 40, topP: 0.95, seed: 42 },
 *   });
 *   const reply = tokenizer.decode(newIds);
 *   console.log(reply.trim());
 *
 * @example
 *   // --- Streaming Generation -----------------------------------------------
 *   const { model, tokenizer } = bro.lm.loadQwen('../brolm/weights/Qwen3-0.6B-GGUF/Qwen3-0.6B-BF16.gguf');
 *   const prompt = tokenizer.applyChatTemplate([
 *     { role: 'user', content: 'Count from 1 to 5.' },
 *   ], true);
 *   const promptIds = tokenizer.encode(prompt);
 *   const acc: number[] = [];
 *   let shown = '';
 *   model.generateStream(promptIds, {
 *     maxNewTokens: 40,
 *     eosId: tokenizer.imEndId,
 *     sampling: { temperature: 0.7, topK: 40, topP: 0.95, seed: 42 },
 *   }, (id) => {
 *     acc.push(id);
 *     const text = tokenizer.decode(acc);
 *     const delta = text.slice(shown.length);
 *     shown = text;
 *     if (delta) console.log(delta);
 *     return true;
 *   });
 *
 * @example
 *   // --- Async Generation (All Families) ------------------------------------
 *   const { model, tokenizer } = bro.lm.loadQwen('../brolm/weights/Qwen3-0.6B-GGUF/Qwen3-0.6B-BF16.gguf');
 *   const promptIds = tokenizer.encode('Hello world');
 *   const h = bro.lm.generate(model, promptIds, {
 *     maxNewTokens: 256,
 *     eosId: tokenizer.imEndId,
 *     sampling: { temperature: 0.7, topK: 40, topP: 0.95 },
 *     onToken: (id) => {},
 *     onDone: (ids) => {
 *       console.log(ids);
 *     },
 *   });
 *   h.cancel();
 *
 * @example
 *   // --- Mistral 3.1 --------------------------------------------------------
 *   const mis = bro.lm.loadMistral(
 *     '../brolm/weights/Mistral-Small-3.1-24B-Instruct-2503-GGUF/mistralai_Mistral-Small-3.1-24B-Instruct-2503-Q4_K_M.gguf',
 *     { tokenizerPath: '../brolm/weights/Mistral-Small-3.1-24B-Instruct-2503/tekken.json' });
 *   const mPrompt = mis.tokenizer.applyChatTemplate(
 *     [{ role: 'user', content: 'One-sentence fun fact about owls.' }], true);
 *   const mIds = mis.model.generate(mis.tokenizer.encode(mPrompt, false), {
 *     maxNewTokens: 64,
 *     eosId: mis.tokenizer.eosId,
 *     sampling: { temperature: 0 },
 *   });
 *   console.log(mis.tokenizer.decode(mIds));
 *
 * @example
 *   // --- Gemma-2 ------------------------------------------------------------
 *   const gem = bro.lm.loadGemma2('../brolm/weights/gemma-2-2b');
 *   const gIds = gem.model.generate(
 *     gem.tokenizer.encode('The capital of France is'), {
 *       maxNewTokens: 16,
 *       eosId: gem.tokenizer.eosId,
 *       sampling: { temperature: 0 },
 *     });
 *   console.log(gem.tokenizer.decode(gIds));
 *
 * @example
 *   // --- Qwen3.5 & Vision ---------------------------------------------------
 *   const q35 = bro.lm.loadQwen35('../brolm/weights/Qwen3.5-0.8B');
 *   const ids35 = q35.generate(
 *     '<|im_start|>user\\nOne-word answer: capital of France?<|im_end|>\\n<|im_start|>assistant\\n',
 *     { maxNewTokens: 16, sampling: { temperature: 0 } });
 *   console.log(q35.decode(ids35));
 *
 * @example
 *   // --- NLLB-200 Machine Translation ---------------------------------------
 *   const nllb = bro.lm.loadNllb('../brolm/weights/nllb-200-distilled-600M');
 *   console.log(nllb.translate('Hello, world!', 'eng_Latn', 'fra_Latn'));
 *
 * @example
 *   // --- CLIP Cross-Modal Scoring -------------------------------------------
 *   const clip = bro.lm.loadClip({
 *     vocabPath: '../clip-vit-large-patch14/vocab.json',
 *     mergesPath: '../clip-vit-large-patch14/merges.txt',
 *     weightsPath: '../clip-vit-large-patch14/model.safetensors',
 *   });
 *   const emb = clip.encodeText('a photo of an astronaut riding a horse');
 *   console.log(emb);
 *
 * @example
 *   // --- T5 Encoder ---------------------------------------------------------
 *   const t5 = bro.lm.loadT5({
 *     tokenizerPath: '../FLUX.1-schnell/tokenizer_2/tokenizer.json',
 *     shards: [
 *       '../FLUX.1-schnell/text_encoder_2/model-00001-of-00002.safetensors',
 *       '../FLUX.1-schnell/text_encoder_2/model-00002-of-00002.safetensors',
 *     ],
 *     maxLength: 256,
 *   });
 *   const t5enc = t5.encode('a serene mountain lake at dawn');
 *   console.log(t5enc.length, 'tokens', t5enc.dim, 'dims');
 *
 * @example
 *   // --- ModernBERT Encoder -------------------------------------------------
 *   const bert = bro.lm.loadModernBert('../ModernBERT-large');
 *   const { pooled } = bert.encode('Please refund the duplicate charge.', { pooling: 'mean' });
 *   console.log(pooled.length);  // bert.hiddenSize
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * Chat message turn representation for applyChatTemplate.
 * @typedef {Object} ChatMessage
 * @property {string} role
 * @property {string} content
 */

/**
 * Autoregressive sampling configuration options. The penalty knobs may also
 * sit directly on the GenerateOptions object (camelCase or snake_case).
 * Penalties read the whole context (prompt + generated ids).
 * @typedef {Object} SamplingOptions
 * @property {number} [temperature=1] - <= 0 is greedy (argmax)
 * @property {number} [topK=0] - 0 disables
 * @property {number} [topP=1] - >= 1 disables
 * @property {number} [minP=0] - keep tokens with p >= minP * max p; 0 disables
 * @property {number} [repetitionPenalty=1] - 1 disables
 * @property {number} [frequencyPenalty=0]
 * @property {number} [presencePenalty=0]
 * @property {number} [penaltyLastN=64] - context window the penalties see; 0 = all (LMModel only)
 * @property {number} [dryMultiplier=0] - DRY n-gram repetition penalty; 0 disables (LMModel only)
 * @property {number} [dryBase=1.75] - (LMModel only)
 * @property {number} [dryAllowedLength=2] - (LMModel only)
 * @property {(number|bigint)} [seed=0]
 */

/**
 * Text and multimodal generation options.
 * @typedef {Object} GenerateOptions
 * @property {number} [maxNewTokens=64]
 * @property {number} [eosId] - Stop token. LMModel defaults to its paired
 *   tokenizer's end-of-turn id (`model.eosId`); pass -1 to never stop on one.
 * @property {boolean} [stopOnEos=true]
 * @property {SamplingOptions} [sampling]
 * @property {Grammar} [grammar] - Constrain the output (LMModel, Qwen35Model,
 *   Qwen3VLModel): every step is masked so the text conforms. Control tokens
 *   (a special with no text) are never allowed, and the stop token only once
 *   the grammar accepts; with stopOnEos false the decode ends when the
 *   grammar is complete. Generation works on a copy of the grammar's state,
 *   so one Grammar serves any number of calls.
 * @property {(Object|Array<Object>)} [images] - Qwen35Model / Qwen3VLModel:
 *   `{ data, width, height }` RGBA images
 * @property {Function} [onToken] - `(id) => boolean|void`; returning false stops
 * @property {Function} [onDone] - bro.lm.generate: `(ids, { cancelled, error? })`,
 *   also after cancel() with the ids produced so far
 * @property {Function} [onError] - bro.lm.generate: `(message)` on a failure
 *   (without it the failure rides onDone's `info.error`)
 */

/**
 * Model loading options for causal LM backends. Relative paths resolve
 * against the app directory.
 * @typedef {Object} LoadModelOptions
 * @property {string} [device="cuda"]
 * @property {string} [tokenizerPath]
 * @property {number} [maxSeqLen=4096] - Qwen3.5 / Qwen3-VL KV/state capacity
 * @property {Function} [onReady] - When given, the load runs on a worker
 *   thread and the loader returns an AsyncHandle at once; `onReady(result)`
 *   fires on a later tick with what the loader would have returned (the
 *   `{ model, tokenizer }` pair, or the model handle). Not fired after cancel().
 *   Supported by loadQwen, loadMistral, loadGemma2, loadQwen35, loadQwen3VL,
 *   loadNllb.
 * @property {Function} [onError] - `(message)` when an async load fails
 */

/**
 * BPE Tokenizer loading options.
 * @typedef {Object} LoadTokenizerOptions
 * @property {string} vocabPath
 * @property {string} mergesPath
 */

/**
 * CLIP cross-modal scorer loading options.
 * @typedef {Object} LoadClipOptions
 * @property {string} vocabPath
 * @property {string} mergesPath
 * @property {string} [weightsPath]
 * @property {string} [textPath]
 * @property {string} [imagePath]
 * @property {string} [projectionPath]
 * @property {string} [textPrefix="text_model."]
 * @property {string} [visionPrefix="vision_model."]
 * @property {string} [projectionPrefix=""]
 * @property {string} [device="cuda"]
 */

/**
 * T5 architectural dimension configuration overrides.
 * @typedef {Object} T5Config
 * @property {number} [vocabSize]
 * @property {number} [dModel]
 * @property {number} [dFf]
 * @property {number} [dKv]
 * @property {number} [numHeads]
 * @property {number} [numLayers]
 */

/**
 * T5 text encoder loading options.
 * @typedef {Object} LoadT5Options
 * @property {string} tokenizerPath
 * @property {string} [ggufPath]
 * @property {string} [weightsPath]
 * @property {Array<string>} [shards]
 * @property {string} [prefix=""]
 * @property {number} [maxLength=512]
 * @property {boolean} [quantizeWeights=false]
 * @property {T5Config} [config]
 * @property {string} [device="cuda"]
 */

/**
 * Per-call encoding options for T5.
 * @typedef {Object} T5EncodeOptions
 * @property {number} [maxLength]
 */

/**
 * Flattened hidden state tensor result from T5 text encoder.
 * @typedef {Object} T5EncodeResult
 * @property {Float32Array} data
 * @property {number} length
 * @property {number} dim
 * @property {Int32Array} ids
 */

/**
 * ModernBERT loading options. Relative paths resolve against the app
 * directory; each path defaults to the Hugging Face layout, then the Laya one.
 * @typedef {Object} LoadModernBertOptions
 * @property {string} [device="cuda"]
 * @property {string} [configPath] - Default `<dir>/config.json`, else `<dir>/encoder/config.json`
 * @property {string} [tokenizerPath] - Default `<dir>/tokenizer.json`, else `<dir>/tokenizer/tokenizer.json`
 *   (a tokenizer_config.json beside it names the [CLS]/[SEP]/[PAD]/[MASK] roles)
 * @property {string} [weightsPath] - Default `<dir>/model.safetensors`
 * @property {string} [prefix] - Tensor-name prefix; found by probing `model.`, `encoder.`, then none
 * @property {number} [maxLength] - Longest sequence encode() accepts; at most (and by
 *   default) the config's max_position_embeddings
 * @property {Function} [onReady] - When given, the load runs on a worker and
 *   `onReady(model)` fires on a later bro.lm.tick; the call returns an AsyncHandle
 * @property {Function} [onError] - `(message)` on an async load failure
 */

/**
 * Per-call ModernBERT options (encode and tokenize).
 * @typedef {Object} ModernBertEncodeOptions
 * @property {boolean} [addSpecialTokens=true] - Wrap text as `[CLS] text [SEP]`, as
 *   the Hugging Face tokenizer does by default
 * @property {number} [maxLength] - Cut the ids to this many (both specials kept);
 *   1..model.maxLength
 * @property {string} [pooling="none"] - encode(): also return `pooled`, the
 *   `'cls'` row or the `'mean'` over every row (specials included)
 */

/**
 * ModernBERT last hidden state.
 * @typedef {Object} ModernBertEncodeResult
 * @property {Float32Array} data - `length * dim`, row-major, one row per id
 * @property {number} length
 * @property {number} dim
 * @property {Int32Array} ids - The ids encoded
 * @property {Float32Array} [pooled] - `dim` values, when opts.pooling is 'cls' or 'mean'
 */

/**
 * Options for NLLB-200 beam-search translation.
 * @typedef {Object} NllbTranslateOptions
 * @property {number} [numBeams=5]
 * @property {number} [maxNewTokens=200]
 * @property {number} [lengthPenalty=1]
 * @property {Function} [onDone]
 * @property {Function} [onError]
 */

/**
 * Result pair returned by loadQwen containing model and tokenizer.
 * @typedef {Object} LMModelPair
 * @property {LMModel} model
 * @property {QwenTokenizer} tokenizer
 */

/**
 * Result pair returned by loadMistral containing model and tokenizer.
 * @typedef {Object} MistralModelPair
 * @property {LMModel} model
 * @property {MistralTokenizer} tokenizer
 */

/**
 * Result pair returned by loadGemma2 containing model and tokenizer.
 * @typedef {Object} GemmaModelPair
 * @property {LMModel} model
 * @property {GemmaTokenizer} tokenizer
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * Asynchronous job handle with cancellation support. Callbacks of the job
 * fire on the thread that started it, when bro.lm.tick() runs (bro's frame
 * pump does this) or inside wait().
 */
class AsyncHandle {

  /**
   * True once the job finished and its callbacks ran.
   * @readonly
   * @type {boolean}
   */
  done;

  /**
   * True once cancel() was called.
   * @readonly
   * @type {boolean}
   */
  cancelled;

  /**
   * Cancel the in-flight background job (a generation stops within one token).
   */
  cancel() {}

  /**
   * Block until the job finished, delivering its callbacks meanwhile.
   */
  wait() {}

}

/**
 * A decoding constraint for GenerateOptions.grammar (LMModel, Qwen35Model,
 * Qwen3VLModel). Built by the
 * static factories only (`new Grammar()` throws). The instance methods drive
 * the object's own state, for validating text by hand; generation never
 * advances the object it is given.
 *
 * @example
 *   const g = bro.lm.Grammar.jsonSchema({ type: 'object',
 *     properties: { mood: { enum: ['happy', 'sad'] } }, required: ['mood'] });
 *   const ids = model.generate(promptIds, { maxNewTokens: 32, grammar: g });
 */
class Grammar {

  /**
   * Regular expression: literals, classes [a-z] [^0-9], \d \w \s, `.`, `|`,
   * groups, and the quantifiers * + ? {n} {m,n} {m,}.
   * @param {string} pattern
   * @returns {Grammar}
   */
  static regex(pattern) {}

  /**
   * BNF / EBNF: `rule ::= expression` lines; the start rule is `root` (or the first).
   * @param {string} text
   * @returns {Grammar}
   */
  static bnf(text) {}

  /**
   * A JSON Schema (text, or an object that is JSON.stringify'd): object
   * properties, required, types, enums, string patterns.
   * @param {(string|Object)} schema
   * @returns {Grammar}
   */
  static jsonSchema(schema) {}

  /**
   * Exactly one of the given strings.
   * @param {Array<string>} options
   * @returns {Grammar}
   */
  static choice(options) {}

  /**
   * Exactly this string.
   * @param {string} text
   * @returns {Grammar}
   */
  static exact(text) {}

  /** @returns {Grammar} any JSON object */
  static jsonObject() {}
  /** @returns {Grammar} any JSON array */
  static jsonArray() {}
  /** @returns {Grammar} any JSON value */
  static jsonValue() {}
  /** @returns {Grammar} a JSON string */
  static jsonString() {}
  /** @returns {Grammar} a JSON number */
  static jsonNumber() {}
  /** @returns {Grammar} a JSON integer */
  static jsonInteger() {}
  /** @returns {Grammar} true | false */
  static jsonBoolean() {}
  /** @returns {Grammar} null */
  static jsonNull() {}

  /**
   * Would appending `text` keep the state valid? Does not change the state.
   * @param {string} text
   * @returns {boolean}
   */
  canAccept(text) {}

  /**
   * Advance the state by `text`.
   * @param {string} text
   * @returns {boolean} false when `text` is not a valid continuation
   */
  accept(text) {}

  /**
   * Is the consumed text a complete match?
   * @returns {boolean}
   */
  isAccepted() {}

  /** Back to the initial state. */
  reset() {}

  /**
   * A copy in the current state.
   * @returns {Grammar}
   */
  clone() {}

}

/**
 * Qwen3 BPE Tokenizer handle.
 */
class QwenTokenizer {

  /**
   * Id of the <|im_end|> turn terminator (use as eosId).
   * @readonly
   * @type {number}
   */
  imEndId;

  /**
   * Id of the <|im_start|> role marker.
   * @readonly
   * @type {number}
   */
  imStartId;

  /**
   * End-of-sequence id (the <|im_end|> turn terminator).
   * @readonly
   * @type {number}
   */
  eosId;

  /**
   * Id of <|endoftext|>.
   * @readonly
   * @type {number}
   */
  endoftextId;

  /**
   * Vocabulary entries.
   * @readonly
   * @type {number}
   */
  vocabCount;

  /**
   * BPE merge rules.
   * @readonly
   * @type {number}
   */
  mergeCount;

  /**
   * BPE-encode a string to token ids.
   * Special-token literals in `text` (e.g. `<|im_start|>`) always encode
   * as their single ids.
   *
   * @param {string} text - Text string to encode
   * @param {boolean} [addSpecial=false] - Append <|endoftext|> (Qwen3 has no BOS)
   * @returns {Int32Array} Token ids
   */
  encode(text, addSpecial) {}

  /**
   * Decode token ids back to text string.
   *
   * @param {(Array<number>|Int32Array)} ids - Array of token ids or Int32Array
   * @returns {string} Decoded text
   */
  decode(ids) {}

  /**
   * Render an array of { role, content } messages into the Qwen chat format.
   *
   * @param {Array<ChatMessage>} messages - Array of chat turn messages
   * @param {boolean} [addGenerationPrompt] - Append assistant turn marker if true
   * @returns {string} Formatted prompt string
   */
  applyChatTemplate(messages, addGenerationPrompt) {}

}

/**
 * Mistral 3.1 native tekken BPE Tokenizer handle.
 */
class MistralTokenizer {

  /**
   * End of sequence token id.
   * @readonly
   * @type {number}
   */
  eosId;

  /**
   * Beginning of sequence token id.
   * @readonly
   * @type {number}
   */
  bosId;

  /**
   * Total vocabulary token count.
   * @readonly
   * @type {number}
   */
  vocabCount;

  /**
   * BPE-encode text to Int32Array token ids.
   *
   * @param {string} text - Text to encode
   * @param {boolean} [addSpecial] - Prepend BOS (<s>) token if true
   * @returns {Int32Array} Int32Array of token ids
   */
  encode(text, addSpecial) {}

  /**
   * Decode token ids back to text string.
   *
   * @param {(Array<number>|Int32Array)} ids - Token ids
   * @returns {string} Decoded text
   */
  decode(ids) {}

  /**
   * Render messages into Mistral's [INST] chat format.
   *
   * @param {Array<ChatMessage>} messages - Chat messages
   * @param {boolean} [addGenerationPrompt] - Append generation prompt if true
   * @returns {string} Formatted prompt string
   */
  applyChatTemplate(messages, addGenerationPrompt) {}

}

/**
 * Gemma-2 SentencePiece-BPE Tokenizer handle.
 */
class GemmaTokenizer {

  /**
   * End of sequence token id.
   * @readonly
   * @type {number}
   */
  eosId;

  /**
   * Beginning of sequence token id.
   * @readonly
   * @type {number}
   */
  bosId;

  /**
   * Padding token id.
   * @readonly
   * @type {number}
   */
  padId;

  /**
   * Unknown token id.
   * @readonly
   * @type {number}
   */
  unkId;

  /**
   * Total vocabulary token count.
   * @readonly
   * @type {number}
   */
  vocabCount;

  /**
   * BPE-encode text to Int32Array token ids.
   *
   * @param {string} text - Text to encode
   * @param {boolean} [addBos] - Prepend <bos> token if true (default true)
   * @returns {Int32Array} Int32Array of token ids
   */
  encode(text, addBos) {}

  /**
   * Decode token ids back to text string.
   *
   * @param {(Array<number>|Int32Array)} ids - Token ids
   * @returns {string} Decoded text
   */
  decode(ids) {}

}

/**
 * Causal Transformer Language Model (Qwen3, Mistral 3.1, Gemma-2).
 */
class LMModel {

  /**
   * Model family name ('qwen3', 'mistral3', 'gemma2').
   * @readonly
   * @type {string}
   */
  family;

  /**
   * Vocabulary dimension size.
   * @readonly
   * @type {number}
   */
  vocabSize;

  /**
   * Hidden embedding dimension size.
   * @readonly
   * @type {number}
   */
  hiddenSize;

  /**
   * Number of hidden transformer layers.
   * @readonly
   * @type {number}
   */
  numLayers;

  /**
   * Maximum position sequence length.
   * @readonly
   * @type {number}
   */
  maxSeqLen;

  /**
   * Current allocated KV cache token capacity.
   * @readonly
   * @type {number}
   */
  cacheLen;

  /**
   * The paired tokenizer's end-of-turn id: the stop token generate() uses
   * when opts.eosId is absent.
   * @readonly
   * @type {number}
   */
  eosId;

  /**
   * True while a generation (sync or a bro.lm.generate job) owns the model.
   * A model runs one decode at a time: a call made meanwhile throws
   * "a generation is already in flight on this model".
   * @readonly
   * @type {boolean}
   */
  busy;

  /**
   * Size and allocate the KV cache. Reused across generate calls.
   *
   * @param {number} maxTokens - Maximum capacity (prompt + generated tokens)
   */
  allocateCache(maxTokens) {}

  /**
   * Reset the KV cache state.
   */
  resetCache() {}

  /**
   * Raw logits of one forward over the ids (all positions, row-major L x vocab).
   *
   * @param {(Array<number>|Int32Array)} ids
   * @returns {Float32Array}
   */
  forward(ids) {}

  /**
   * Run synchronous autoregressive generation.
   *
   * @param {(Array<number>|Int32Array)} promptIds - Input prompt token ids
   * @param {GenerateOptions} [opts] - Sampling, stopping and grammar options
   * @returns {Int32Array} Generated token ids (excluding prompt)
   */
  generate(promptIds, opts) {}

  /**
   * Run synchronous streaming generation with per-token callbacks.
   * Also `generateStream(promptIds, onToken)`.
   *
   * @param {(Array<number>|Int32Array)} promptIds - Input prompt token ids
   * @param {GenerateOptions} opts - Sampling, stopping and grammar options
   * @param {Function} onToken - `(id) => boolean|void`; returning false stops
   * @returns {Int32Array} All newly generated token ids
   */
  generateStream(promptIds, opts, onToken) {}

}

/**
 * Qwen3.5 hybrid full/linear-attention vision-language model handle.
 */
class Qwen35Model {

  /**
   * Model family identifier ('qwen35').
   * @readonly
   * @type {string}
   */
  family;

  /**
   * Vocabulary dimension size.
   * @readonly
   * @type {number}
   */
  vocabSize;

  /**
   * Hidden embedding dimension size.
   * @readonly
   * @type {number}
   */
  hiddenSize;

  /**
   * Number of hidden transformer layers.
   * @readonly
   * @type {number}
   */
  numLayers;

  /**
   * Maximum sequence context length.
   * @readonly
   * @type {number}
   */
  maxSeqLen;

  /**
   * End-of-sequence token id.
   * @readonly
   * @type {number}
   */
  eosId;

  /**
   * Turn terminator token id (<|im_end|>).
   * @readonly
   * @type {number}
   */
  imEndId;

  /**
   * End-of-text special token id.
   * @readonly
   * @type {number}
   */
  endoftextId;

  /**
   * Tokenize text into Int32Array token ids.
   *
   * @param {string} text - String to encode
   * @param {boolean} [addSpecial] - Optional flag to add special tokens
   * @returns {Int32Array} Int32Array of token ids
   */
  encode(text, addSpecial) {}

  /**
   * Decode token ids into text string.
   *
   * @param {(Array<number>|Int32Array)} ids - Token ids
   * @returns {string} Decoded text
   */
  decode(ids) {}

  /**
   * True while a generation owns the model (one decode at a time; a call
   * made meanwhile throws).
   * @readonly
   * @type {boolean}
   */
  busy;

  /**
   * Generate text or multimodal responses from a string prompt (blocking).
   * opts.onToken, when given, streams each id; opts.grammar constrains the
   * reply (see GenerateOptions.grammar).
   *
   * @param {string} prompt - Prompt string formatted with ChatML / vision tokens
   * @param {GenerateOptions} [opts] - Generation options including images
   * @returns {Int32Array} Generated token ids
   */
  generate(prompt, opts) {}

  /**
   * generate() with a required per-token callback. The prompt may also be
   * token ids (decoded back to text); also `generateStream(prompt, onToken)`.
   *
   * @param {(string|Int32Array|Array<number>)} prompt
   * @param {GenerateOptions} opts
   * @param {Function} onToken - `(id) => boolean|void`; returning false stops
   * @returns {Int32Array} Generated token ids
   */
  generateStream(prompt, opts, onToken) {}

}

/**
 * Qwen3-VL dense decoder vision-language model handle with DeepStack feature injection.
 */
class Qwen3VLModel {

  /**
   * Model family identifier ('qwen3vl').
   * @readonly
   * @type {string}
   */
  family;

  /**
   * Vocabulary dimension size.
   * @readonly
   * @type {number}
   */
  vocabSize;

  /**
   * Hidden embedding dimension size.
   * @readonly
   * @type {number}
   */
  hiddenSize;

  /**
   * Number of hidden transformer layers.
   * @readonly
   * @type {number}
   */
  numLayers;

  /**
   * Maximum sequence context length.
   * @readonly
   * @type {number}
   */
  maxSeqLen;

  /**
   * End-of-sequence token id.
   * @readonly
   * @type {number}
   */
  eosId;

  /**
   * Turn terminator token id (<|im_end|>).
   * @readonly
   * @type {number}
   */
  imEndId;

  /**
   * End-of-text special token id.
   * @readonly
   * @type {number}
   */
  endoftextId;

  /**
   * Tokenize text into Int32Array token ids.
   *
   * @param {string} text - String to encode
   * @param {boolean} [addSpecial] - Optional flag to add special tokens
   * @returns {Int32Array} Int32Array of token ids
   */
  encode(text, addSpecial) {}

  /**
   * Decode token ids into text string.
   *
   * @param {(Array<number>|Int32Array)} ids - Token ids
   * @returns {string} Decoded text
   */
  decode(ids) {}

  /**
   * True while a generation owns the model (one decode at a time; a call
   * made meanwhile throws).
   * @readonly
   * @type {boolean}
   */
  busy;

  /**
   * Generate text or multimodal responses from a string prompt (blocking).
   * opts.onToken, when given, streams each id; opts.grammar constrains the
   * reply (see GenerateOptions.grammar).
   *
   * @param {string} prompt - Prompt string formatted with ChatML / vision tokens
   * @param {GenerateOptions} [opts] - Generation options including images
   * @returns {Int32Array} Generated token ids
   */
  generate(prompt, opts) {}

  /**
   * generate() with a required per-token callback. The prompt may also be
   * token ids (decoded back to text); also `generateStream(prompt, onToken)`.
   *
   * @param {(string|Int32Array|Array<number>)} prompt
   * @param {GenerateOptions} opts
   * @param {Function} onToken - `(id) => boolean|void`; returning false stops
   * @returns {Int32Array} Generated token ids
   */
  generateStream(prompt, opts, onToken) {}

}

/**
 * NLLB-200 encoder-decoder machine translation model handle.
 */
class NllbModel {

  /**
   * Model family identifier ('nllb').
   * @readonly
   * @type {string}
   */
  family;

  /**
   * Vocabulary dimension size.
   * @readonly
   * @type {number}
   */
  vocabSize;

  /**
   * Model embedding dimension size.
   * @readonly
   * @type {number}
   */
  dModel;

  /**
   * Number of encoder layers.
   * @readonly
   * @type {number}
   */
  encoderLayers;

  /**
   * Number of decoder layers.
   * @readonly
   * @type {number}
   */
  decoderLayers;

  /**
   * Total number of supported FLORES-200 languages.
   * @readonly
   * @type {number}
   */
  languageCount;

  /**
   * Check whether a FLORES-200 language code is supported.
   *
   * @param {string} code - Language code (e.g. 'eng_Latn', 'fra_Latn')
   * @returns {boolean} True if supported
   */
  hasLanguage(code) {}

  /**
   * Translate text from source language to target language.
   *
   * @param {string} text - Text string to translate
   * @param {string} srcLang - Source language FLORES-200 code
   * @param {string} tgtLang - Target language FLORES-200 code
   * @param {NllbTranslateOptions} [opts] - Translation options (numBeams, onDone for async)
   * @returns {(string|AsyncHandle)} Translated string in sync mode, or AsyncHandle if onDone provided
   */
  translate(text, srcLang, tgtLang, opts) {}

}

/**
 * CLIP ViT-L/14 cross-modal text and image similarity scorer.
 */
class ClipModel {

  /**
   * Dimension size of the shared cross-modal embedding space (768).
   * @readonly
   * @type {number}
   */
  projectionDim;

  /**
   * Compute normalized text embeddings in the shared space.
   *
   * @param {(string|Array<string>)} text - Single prompt string or array of prompt strings
   * @returns {(Float32Array|Array<Float32Array>)} Float32Array or array of Float32Array embeddings
   */
  encodeText(text) {}

  /**
   * Compute normalized image embedding in the shared space.
   *
   * @param {Object} image - ImageBitmap or ImageData object
   * @returns {Float32Array} Projected Float32Array image embedding
   */
  encodeImage(image) {}

  /**
   * Compute cosine similarity in [-1, 1] between text and image.
   *
   * @param {(string|Array<string>)} text - Single prompt or array of candidate prompts
   * @param {Object} image - ImageBitmap or ImageData
   * @returns {(number|Array<number>)} Cosine score or array of scores
   */
  score(text, image) {}

}

/**
 * T5 encoder-only text encoder (T5-XXL / Flux text conditioning).
 */
class T5Model {

  /**
   * Transformer hidden dimension size (4096 for T5-XXL).
   * @readonly
   * @type {number}
   */
  dModel;

  /**
   * Fixed maximum encode sequence length.
   * @readonly
   * @type {number}
   */
  maxLength;

  /**
   * Pad token id.
   * @readonly
   * @type {number}
   */
  padId;

  /**
   * End of sequence token id.
   * @readonly
   * @type {number}
   */
  eosId;

  /**
   * Total vocabulary token count.
   * @readonly
   * @type {number}
   */
  vocabCount;

  /**
   * Encode text to fixed-length row-major hidden state tensor.
   *
   * @param {string} text - Prompt text to encode
   * @param {T5EncodeOptions} [opts] - Encoding options
   * @returns {T5EncodeResult} Encoded data buffer and dimension metadata
   */
  encode(text, opts) {}

}

/**
 * ModernBERT bidirectional text encoder. The output is the model's last
 * hidden state (after the final norm), matching Hugging Face's
 * ModernBertModel.last_hidden_state.
 */
class ModernBertModel {

  /**
   * Always "modernbert".
   * @readonly
   * @type {string}
   */
  family;

  /**
   * Hidden width (1024 for ModernBERT-large).
   * @readonly
   * @type {number}
   */
  hiddenSize;

  /**
   * Encoder layer count.
   * @readonly
   * @type {number}
   */
  numLayers;

  /**
   * Embedding rows; every id passed to encode() must be below it.
   * @readonly
   * @type {number}
   */
  vocabSize;

  /**
   * Longest sequence encode() accepts.
   * @readonly
   * @type {number}
   */
  maxLength;

  /**
   * [CLS], [SEP], [PAD] and [MASK] ids, from the tokenizer's special-token roles.
   * @readonly
   * @type {number}
   */
  clsId;

  /**
   * @readonly
   * @type {number}
   */
  sepId;

  /**
   * @readonly
   * @type {number}
   */
  padId;

  /**
   * @readonly
   * @type {number}
   */
  maskId;

  /**
   * Token ids for `text`, wrapped and cut as encode() would.
   *
   * @param {string} text
   * @param {ModernBertEncodeOptions} [opts]
   * @returns {Int32Array}
   */
  tokenize(text, opts) {}

  /**
   * Encode text, or ids taken as given (already wrapped; each must be below
   * vocabSize, at most maxLength of them). Blocking; runs on the model's device.
   *
   * @param {(string|Int32Array|Array<number>)} text
   * @param {ModernBertEncodeOptions} [opts]
   * @returns {ModernBertEncodeResult}
   */
  encode(text, opts) {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * Language-model inference and cross-modal embedding namespace.
 */
/**
 * Initialize LM runtime subsystem.
 */
bro.lm.init = function() {};

/**
 * Load a Qwen3 model + tokenizer from a GGUF checkpoint.
 *
 * @param {string} ggufPath - Path to Qwen3 .gguf file
 * @param {LoadModelOptions} [opts] - Optional device configuration
 * @returns {LMModelPair} Paired model and tokenizer
 */
bro.lm.loadQwen = function(ggufPath, opts) {};

/**
 * Load Mistral 3.1 text decoder from GGUF and native tekken tokenizer.
 *
 * @param {string} ggufPath - Path to Mistral .gguf file
 * @param {LoadModelOptions} opts - Options with required tokenizerPath (tekken.json)
 * @returns {MistralModelPair} Paired model and tokenizer
 */
bro.lm.loadMistral = function(ggufPath, opts) {};

/**
 * Load Gemma-2 from a HuggingFace checkpoint directory.
 *
 * @param {string} modelDir - Directory containing config.json and *.safetensors
 * @param {LoadModelOptions} [opts] - Loading options
 * @returns {GemmaModelPair} Paired model and tokenizer
 */
bro.lm.loadGemma2 = function(modelDir, opts) {};

/**
 * Load a Qwen3.5 VLM checkpoint directory.
 *
 * @param {string} checkpointDir - Directory containing config.json and safetensors
 * @param {LoadModelOptions} [opts] - Loading options
 * @returns {Qwen35Model} Qwen35Model handle
 */
bro.lm.loadQwen35 = function(checkpointDir, opts) {};

/**
 * Load a Qwen3-VL checkpoint directory.
 *
 * @param {string} checkpointDir - Directory containing config.json and safetensors
 * @param {LoadModelOptions} [opts] - Loading options
 * @returns {Qwen3VLModel} Qwen3VLModel handle
 */
bro.lm.loadQwen3VL = function(checkpointDir, opts) {};

/**
 * Load an NLLB-200 translation model checkpoint directory.
 *
 * @param {string} checkpointDir - Directory containing config.json and safetensors
 * @param {LoadModelOptions} [opts] - Loading options
 * @returns {NllbModel} NllbModel handle
 */
bro.lm.loadNllb = function(checkpointDir, opts) {};

/**
 * Load standalone Qwen tokenizer without weights: a tokenizer.json path, or
 * { vocabPath, mergesPath }, or { tokenizerPath }.
 *
 * @param {(string|LoadTokenizerOptions)} opts - tokenizer.json path or options
 * @returns {QwenTokenizer} QwenTokenizer handle
 */
bro.lm.loadTokenizer = function(opts) {};

/**
 * Load a Llama-3 tokenizer (tokenizer.json) without weights.
 *
 * @param {(string|{tokenizerPath: string})} pathOrOpts
 * @returns {Llama3Tokenizer} handle with encode(text, addBos=true), decode(ids),
 *   applyChatTemplate(messages, addGenerationPrompt=true) and the ids bosId,
 *   eosId, endOfTextId, eotId, startHeaderId, endHeaderId, vocabCount
 */
bro.lm.loadLlama3Tokenizer = function(pathOrOpts) {};

/** Alias of bro.lm.loadQwen. */
bro.lm.loadModel = function(ggufPath, opts) {};

/** Alias of bro.lm.loadClip. */
bro.lm.loadClipModel = function(opts) {};

/**
 * Load CLIP ViT-L/14 cross-modal scorer.
 *
 * @param {LoadClipOptions} opts - Scorer options specifying weights and vocab
 * @returns {ClipModel} ClipModel handle
 */
bro.lm.loadClip = function(opts) {};

/**
 * Load T5 text encoder.
 *
 * @param {LoadT5Options} opts - Options specifying tokenizer and safetensors/gguf paths
 * @returns {T5Model} T5Model handle
 */
bro.lm.loadT5 = function(opts) {};

/**
 * Load a ModernBERT encoder: a Hugging Face ModernBERT checkpoint directory
 * (config.json, tokenizer.json, model.safetensors) or a Laya checkpoint
 * (encoder/config.json, tokenizer/, the `encoder.` tensors of model.safetensors).
 *
 * @param {string} checkpointDir
 * @param {LoadModernBertOptions} [opts]
 * @returns {(ModernBertModel|AsyncHandle)} The model, or an AsyncHandle with opts.onReady
 */
bro.lm.loadModernBert = function(checkpointDir, opts) {};

/**
 * Asynchronously run generation on a background thread with real-time cancellation.
 * The model is claimed for the job (a sync call on it meanwhile throws) and
 * released before onDone fires, so onDone may start the next generation.
 * opts.onToken(id) fires per token and opts.onDone(ids, { cancelled, error? })
 * once, both on this thread's tick; onDone also fires after cancel() with the
 * ids produced so far. opts.grammar applies to every model kind.
 *
 * @param {Object} model - Model handle (LMModel, Qwen35Model, or Qwen3VLModel)
 * @param {(Array<number>|string)} prompt - Token ids for an LMModel, a prompt string for the VLMs
 * @param {GenerateOptions} [opts] - Generation options including onToken / onDone / onError
 * @returns {AsyncHandle} AsyncHandle for cancel() / wait()
 */
bro.lm.generate = function(model, prompt, opts) {};

/**
 * Deliver finished background work: token / done / error callbacks of
 * bro.lm.generate and async loads, and LayaModel promise settlement. bro's
 * frame pump calls it every frame; a Worker calls it from its own loop.
 */
bro.lm.tick = function() {};

