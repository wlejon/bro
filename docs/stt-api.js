// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * @typedef {Object} WhisperLoadOptions
 * @property {string} [device]
 * @property {boolean} [quantize]
 */

/**
 * @typedef {Object} TokenizerLoadOptions
 * @property {string} path
 * @property {string} [specialTokens]
 */

/**
 * @typedef {Object} WhisperTranscribeOptions
 * @property {number} [maxNewTokens]
 * @property {(Int32Array|Array<number>)} [prompt]
 * @property {number} [temperature]
 * @property {Function} [onToken]
 * @property {Function} [onDone]
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

class WhisperTokenizer {

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

  /**
   * @param {Object} [opts]
   * @returns {Int32Array}
   */
  buildPrompt(opts) {}

}

class WhisperModel {

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
   * @param {(Int32Array|Array<number>|WhisperTranscribeOptions)} [promptOrOpts]
   * @param {WhisperTranscribeOptions} [opts]
   * @returns {AsyncHandle}
   */
  transcribe(audio, promptOrOpts, opts) {}

  /**
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
   * @param {(Float32Array|SttAudioBuffer)} audio
   * @param {(Int32Array|Array<number>|WhisperTranscribeOptions)} [promptOrOpts]
   * @param {WhisperTranscribeOptions} [opts]
   * @returns {AsyncHandle}
   */
  transcribe(audio, promptOrOpts, opts) {}

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
 * @param {string} dir
 * @param {WhisperLoadOptions} [opts]
 * @returns {(WhisperModel|AsyncHandle)}
 */
bro.stt.loadWhisper = function(dir, opts) {};

/**
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
 * @param {Object} model
 * @param {(Float32Array|SttAudioBuffer)} audio
 * @param {*} [promptOrOpts]
 * @param {Object} [opts]
 * @returns {AsyncHandle}
 */
bro.stt.transcribe = function(model, audio, promptOrOpts, opts) {};

