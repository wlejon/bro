// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * @typedef {Object} WhisperLoadOptions
 * @property {string} [device] `'cuda'` | `'metal'` | `'cpu'`; defaults to CUDA,
 *   then Metal, when available, else CPU. (Shared by loadParakeet / loadQwenAsr /
 *   loadParakeetTokenizer.)
 * @property {Function} [onReady] `onReady(model)`: when given, the load runs on
 *   a background thread and the loader returns an AsyncHandle instead of the model.
 * @property {Function} [onError] `onError(message)`, the async load's failure.
 */

/**
 * @typedef {Object} TokenizerLoadOptions
 * @property {string} vocabPath HF Whisper `vocab.json`.
 * @property {string} mergesPath HF Whisper `merges.txt`.
 * @property {string} [addedTokensPath] Upstream `added_tokens.json` carrying the
 *   `<|...|>` specials; unmodified openai/whisper-* checkouts need it, the converted
 *   (merged) layout does not.
 * @property {Function} [onReady] `onReady(tokenizer)`: async load, see WhisperLoadOptions.
 * @property {Function} [onError]
 */

/**
 * Whisper decode options. The decoder prefix is NOT an option: it is the
 * required positional `promptIds` argument of every Whisper transcribe, built
 * by `WhisperTokenizer.buildPrompt(language, task, withTimestamps)`.
 * @typedef {Object} WhisperTranscribeOptions
 * @property {number} [maxNewTokens] Cap on the autoregressive loop (0 = the
 *   model's maxTargetPositions); in long-form mode it caps EACH 30 s window.
 * @property {number} [timestampBeginId] `tokenizer.firstTimestampId`. When set
 *   (>= 0) and the audio is longer than 30 s, the input is windowed into 30 s
 *   segments (Whisper's sequential long-form decode) instead of truncated to the
 *   first window. Needs a timestamps prompt (`buildPrompt(lang, task, true)`).
 * @property {number} [noTimestampsId] `tokenizer.noTimestampsId`. When set
 *   (>= 0) the decoder can never pick `<|notimestamps|>`; pass it whenever you
 *   want timings, a timestamps prompt only omits the token from the prefix.
 * @property {Function} [onToken] `onToken(id)` once per decoded token, in order.
 * @property {Function} [onWindow] `onWindow(startSeconds)` as each long-form
 *   window opens (every window restarts at `<|0.00|>`).
 * @property {Function} [onDone] `onDone(ids, info)` with `info = { cancelled,
 *   error? }` — the async forms only (`bro.stt.transcribe`, `session.transcribe`).
 */

/**
 * @typedef {Object} ParakeetTranscribeOptions
 * @property {Function} [onToken]
 * @property {Function} [onDone]
 */

/**
 * @typedef {Object} ParakeetResult
 * @property {Int32Array} [tokenIds]
 * @property {Int32Array} [frameOffsets]
 */

/**
 * @typedef {Object} QwenAsrTranscribeOptions
 * @property {number} [maxNewTokens]
 * @property {(Int32Array|Array<number>)} [contextIds]
 * @property {Function} [onToken]
 * @property {Function} [onDone]
 */

/**
 * @typedef {Object} QwenAsrEncodeResult
 * @property {Int32Array} [tokenIds]
 */

/**
 * @typedef {Object} QwenAsrStreamOptions
 * @property {string} [device]
 */

/**
 * @typedef {Object} SttAudioBuffer
 * @property {Float32Array} [samples]
 * @property {number} [sampleRate]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * The HF Whisper byte-level BPE tokenizer plus the `<|...|>` specials. There is
 * no `loaded` flag: `loadTokenizer` either returns a working tokenizer or throws.
 *
 * Well-known special-token ids, each -1 when the vocab lacks the token:
 * `eosId`, `sotId` (`<|startoftranscript|>`), `noSpeechId`, `noTimestampsId`,
 * `transcribeId`, `translateId`, `firstTimestampId` (`<|0.00|>`),
 * `lastTimestampId` (`<|30.00|>`); plus `vocabCount` and `mergeCount`.
 *
 * Language detection: there is NO auto-detect surface — no `detectLanguage()`,
 * no `'auto'` language — the caller names the language up front. Whisper's own
 * detection trick (decode one token after a bare `<|startoftranscript|>` and
 * read the `<|xx|>` it picks) is not exposed by these bindings.
 */
class WhisperTokenizer {

  /** @readonly @type {number} */ eosId;
  /** @readonly @type {number} */ sotId;
  /** @readonly @type {number} */ noSpeechId;
  /** @readonly @type {number} */ noTimestampsId;
  /** @readonly @type {number} */ transcribeId;
  /** @readonly @type {number} */ translateId;
  /** @readonly @type {number} */ firstTimestampId;
  /** @readonly @type {number} */ lastTimestampId;
  /** @readonly @type {number} */ vocabCount;
  /** @readonly @type {number} */ mergeCount;

  /**
   * BPE-encode `text`; a substring that exactly matches a special token becomes
   * that token's single id. `addSpecial` appends a trailing `<|endoftext|>`
   * (independent of the decoder prompt — use buildPrompt for that).
   * @param {string} text
   * @param {boolean} [addSpecial=false]
   * @returns {Int32Array}
   */
  encode(text, addSpecial) {}

  /**
   * Inverse of encode. Special ids decode to their literal `<|...|>` form unless
   * `skipSpecial` drops them; unknown ids are skipped.
   * @param {(Int32Array|Array<number>)} tokenIds
   * @param {boolean} [skipSpecial=false]
   * @returns {string}
   */
  decode(tokenIds, skipSpecial) {}

  /**
   * The decoder prefix every Whisper transcribe takes as its `promptIds`:
   * `<|startoftranscript|> <|language|> <|task|> [<|notimestamps|>]`.
   * Positional, not an options object.
   * @param {string} [language='en'] ISO-639-1 code (`'en'`, `'zh'`, …); the
   *   matching `<|xx|>` token must exist in the vocab or the call throws.
   * @param {string} [task='transcribe'] `'transcribe'` or `'translate'` (to English).
   * @param {boolean} [withTimestamps=true] `true` omits `<|notimestamps|>` so the
   *   decoder may emit timestamp tokens (needed for long-form windowing);
   *   `false` appends it for plain text.
   * @returns {Int32Array}
   * @example
   *   const plain = tok.buildPrompt('en', 'transcribe', false);   // text only
   *   const timed = tok.buildPrompt('de');                         // timestamps on
   */
  buildPrompt(language, task, withTimestamps) {}

  /**
   * @param {number} id
   * @returns {boolean} true iff `id` is in [firstTimestampId, lastTimestampId].
   */
  isTimestamp(id) {}

  /**
   * @param {number} id A timestamp token id.
   * @returns {number} Seconds, `0.02 * (id - firstTimestampId)`.
   */
  timestampSeconds(id) {}

}

/**
 * Config getters: `loaded`, `sampleRate`, `numMelBins`, `dModel`,
 * `maxSourcePositions`, `maxTargetPositions`, `vocabSize`, `eosTokenId`,
 * `decoderStartTokenId`. (No `device` getter; pick the device at load time.)
 */
class WhisperModel {

  /** @readonly @type {boolean} */ loaded;
  /** @readonly @type {number} */ sampleRate;
  /** @readonly @type {number} */ numMelBins;
  /** @readonly @type {number} */ dModel;
  /** @readonly @type {number} */ maxSourcePositions;
  /** @readonly @type {number} */ maxTargetPositions;
  /** @readonly @type {number} */ vocabSize;
  /** @readonly @type {number} */ eosTokenId;
  /** @readonly @type {number} */ decoderStartTokenId;

  /**
   * Synchronous (blocking) decode: 16 kHz mono PCM -> token ids, the prompt
   * included. `promptIds` is required and must be non-empty. For a non-blocking
   * decode use `bro.stt.transcribe(model, …)` or a session.
   * @param {(Float32Array|SttAudioBuffer)} audio 16 kHz Float32Array, or
   *   `{ samples, sampleRate }` (resampled for you).
   * @param {(Int32Array|Array<number>)} promptIds From `tokenizer.buildPrompt(...)`.
   * @param {WhisperTranscribeOptions} [opts] `onDone` is ignored here (sync).
   * @returns {Int32Array}
   */
  transcribe(audio, promptIds, opts) {}

  /**
   * A decode session over the shared weights with its own KV cache. Sessions of
   * one model still serialize on the model's single in-flight gate.
   * @returns {WhisperSession}
   */
  createSession() {}

}

class WhisperSession {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * Asynchronous decode on a background thread; `opts.onDone(ids, info)` gets
   * the ids (prompt included) and `info = { cancelled, error? }`. `.cancel()`
   * on the handle stops within about one token.
   * @param {(Float32Array|SttAudioBuffer)} audio
   * @param {(Int32Array|Array<number>)} promptIds Required, non-empty.
   * @param {WhisperTranscribeOptions} [opts]
   * @returns {AsyncHandle}
   */
  transcribe(audio, promptIds, opts) {}

  reset() {}

}

class ParakeetTokenizer {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @param {string} text
   * @returns {(Int32Array|Array<number>)}
   */
  encode(text) {}

  /**
   * @param {(Int32Array|Array<number>)} tokenIds
   * @returns {string}
   */
  decode(tokenIds) {}

}

class ParakeetModel {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @param {(Float32Array|SttAudioBuffer)} audio
   * @param {ParakeetTranscribeOptions} [opts]
   * @returns {AsyncHandle}
   */
  transcribe(audio, opts) {}

  /**
   * @returns {ParakeetSession}
   */
  createSession() {}

}

class ParakeetSession {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @param {(Float32Array|SttAudioBuffer)} audio
   * @param {ParakeetTranscribeOptions} [opts]
   * @returns {AsyncHandle}
   */
  transcribe(audio, opts) {}

  reset() {}

}

class QwenAsrModel {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @param {(Float32Array|SttAudioBuffer)} audio
   * @param {QwenAsrTranscribeOptions} [opts]
   * @returns {AsyncHandle}
   */
  transcribe(audio, opts) {}

  /**
   * @returns {QwenAsrSession}
   */
  createSession() {}

}

class QwenAsrSession {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @param {(Float32Array|SttAudioBuffer)} audio
   * @param {QwenAsrTranscribeOptions} [opts]
   * @returns {AsyncHandle}
   */
  transcribe(audio, opts) {}

  reset() {}

}

class QwenAsrStream {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @param {(Float32Array|SttAudioBuffer)} audio
   */
  feed(audio) {}

  /**
   * @returns {QwenAsrEncodeResult}
   */
  finish() {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

bro.stt.init = function() {};

/**
 * Load an HF Whisper checkpoint (`config.json` + `model.safetensors` in `dir`).
 * Returns the model, or an AsyncHandle when `opts.onReady` is given.
 * @param {string} dir
 * @param {WhisperLoadOptions} [opts]
 * @returns {(WhisperModel|AsyncHandle)}
 * @example
 *   const whisper = bro.stt.loadWhisper(dir);
 *   const tok = bro.stt.loadTokenizer({ vocabPath: dir + '/vocab.json', mergesPath: dir + '/merges.txt' });
 *   const prompt = tok.buildPrompt('en', 'transcribe', false);
 *   const ids = whisper.transcribe({ samples, sampleRate: 16000 }, prompt, { maxNewTokens: 224 });
 *   const text = tok.decode(ids, true).trim();
 */
bro.stt.loadWhisper = function(dir, opts) {};

/**
 * Build the Whisper tokenizer from `vocab.json` + `merges.txt` (+ an optional
 * `added_tokens.json`). Returns the tokenizer, or an AsyncHandle with `onReady`.
 * @param {TokenizerLoadOptions} opts
 * @returns {(WhisperTokenizer|AsyncHandle)}
 */
bro.stt.loadTokenizer = function(opts) {};

/**
 * @param {string} dir
 * @param {WhisperLoadOptions} [opts]
 * @returns {(ParakeetModel|AsyncHandle)}
 */
bro.stt.loadParakeet = function(dir, opts) {};

/**
 * @param {string} path
 * @param {WhisperLoadOptions} [opts]
 * @returns {(ParakeetTokenizer|AsyncHandle)}
 */
bro.stt.loadParakeetTokenizer = function(path, opts) {};

/**
 * @param {string} dir
 * @param {WhisperLoadOptions} [opts]
 * @returns {(QwenAsrModel|AsyncHandle)}
 */
bro.stt.loadQwenAsr = function(dir, opts) {};

/**
 * @param {string} dir
 * @param {QwenAsrStreamOptions} [opts]
 * @returns {(QwenAsrStream|AsyncHandle)}
 */
bro.stt.loadQwenAsrStream = function(dir, opts) {};

/**
 * The non-blocking decode for any of the three models, on a background thread
 * with a real `.cancel()`. The argument list depends on the model:
 *   `transcribe(whisper, audio, promptIds, opts?)` — `promptIds` required
 *     (`tokenizer.buildPrompt(...)`); `opts.onDone(ids, info)`.
 *   `transcribe(parakeet, audio, opts?)` — no prompt; `opts.onDone(result, info)`
 *     with `result = { tokenIds, tokenFrames }`.
 *   `transcribe(qwenAsr, audio, opts?)` — no prompt; `opts.onDone(ids, info)`.
 * `info = { cancelled, error? }`. One decode is in flight per model at a time
 * (sessions included); a second call throws.
 * @param {(WhisperModel|ParakeetModel|QwenAsrModel)} model
 * @param {(Float32Array|SttAudioBuffer)} audio
 * @param {(Int32Array|Array<number>|ParakeetTranscribeOptions|QwenAsrTranscribeOptions)} [promptIdsOrOpts]
 * @param {WhisperTranscribeOptions} [opts] Whisper only.
 * @returns {AsyncHandle}
 */
bro.stt.transcribe = function(model, audio, promptIdsOrOpts, opts) {};

