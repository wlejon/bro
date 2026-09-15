// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * @typedef {Object} KokoroLoadOptions
 * @property {string} [device]
 */

/**
 * @typedef {Object} VoiceLoadOptions
 * @property {string} [path]
 */

/**
 * @typedef {Object} KokoroSynthesizeOptions
 * @property {number} [speed=1]
 * @property {Function} [onDone]
 */

/**
 * @typedef {Object} KokoroSynthesizeResult
 * @property {Float32Array} [samples]
 * @property {number} [sampleRate]
 */

/**
 * @typedef {Object} KokoroStreamOptions
 * @property {number} [speed=1]
 * @property {Function} [onChunk]
 * @property {Function} [onDone]
 */

/**
 * @typedef {Object} QwenLoadOptions
 * @property {string} [device]
 */

/**
 * @typedef {Object} QwenSynthesizeOptions
 * @property {string} [speaker]
 * @property {number} [speed=1]
 * @property {Function} [onDone]
 */

/**
 * @typedef {Object} QwenSynthesizeResult
 * @property {Float32Array} [samples]
 * @property {number} [sampleRate]
 */

/**
 * @typedef {Object} QwenStreamOptions
 * @property {string} [speaker]
 * @property {number} [speed=1]
 * @property {Function} [onChunk]
 * @property {Function} [onDone]
 */

/**
 * @typedef {Object} QwenAudioCodes
 * @property {Int32Array} [codes]
 * @property {number} [numFrames]
 */

/**
 * @typedef {Object} SupertonicLoadOptions
 * @property {string} [device]
 */

/**
 * @typedef {Object} SupertonicSynthesizeOptions
 * @property {SupertonicVoice} [voice]
 * @property {number} [speed=1]
 * @property {Function} [onDone]
 */

/**
 * @typedef {Object} SupertonicSynthesizeResult
 * @property {Float32Array} [samples]
 * @property {number} [sampleRate]
 */

/**
 * @typedef {Object} SpeakerEncoderLoadOptions
 * @property {string} [device]
 */

/**
 * @typedef {Object} SpeakerEncoderEmbedOptions
 * @property {(Float32Array|SttAudioBuffer)} [audio]
 * @property {Function} [onDone]
 */

/**
 * @typedef {Object} TtsAssetsOptions
 * @property {string} [root]
 * @property {string} [lexicon]
 * @property {string} [pos]
 * @property {string} [kokoroConfig]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

class KokoroModel {

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
   * @param {string} ipa
   * @returns {Int32Array}
   */
  encodePhonemes(ipa) {}

  /**
   * @param {string} path
   * @returns {Voice}
   */
  loadVoice(path) {}

  /**
   * @returns {KokoroSession}
   */
  createSession() {}

}

class Voice {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @readonly
   * @type {string}
   */
  name;

}

class KokoroSession {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @param {(Int32Array|Array<number>)} phonemes
   * @param {Voice} voice
   * @param {KokoroSynthesizeOptions} [opts]
   * @returns {AsyncHandle}
   */
  synthesize(phonemes, voice, opts) {}

  reset() {}

}

class QwenTtsModel {

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
   * @readonly
   * @type {string}
   */
  variant;

  /**
   * @returns {QwenTtsSession}
   */
  createSession() {}

  /**
   * @param {Float32Array} audio
   * @returns {QwenAudioCodes}
   */
  encodeAudio(audio) {}

  /**
   * @param {Int32Array} codes
   * @returns {Float32Array}
   */
  decodeCodes(codes) {}

}

class QwenTtsSession {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @readonly
   * @type {string}
   */
  variant;

  /**
   * @param {string} text
   * @param {QwenSynthesizeOptions} [opts]
   * @returns {AsyncHandle}
   */
  synthesize(text, opts) {}

  reset() {}

}

class SupertonicModel {

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
   * @param {string} path
   * @returns {SupertonicVoice}
   */
  loadVoiceStyle(path) {}

}

class SupertonicVoice {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @readonly
   * @type {string}
   */
  name;

}

class SpeakerEncoder {

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
   * @param {Float32Array} audio
   * @param {SpeakerEncoderEmbedOptions} [opts]
   * @returns {AsyncHandle}
   */
  embedSpeaker(audio, opts) {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

bro.tts.init = function() {};

/**
 * @param {string} dir
 * @param {KokoroLoadOptions} [opts]
 * @returns {(KokoroModel|AsyncHandle)}
 */
bro.tts.loadKokoro = function(dir, opts) {};

/**
 * @param {string} dir
 * @param {QwenLoadOptions} [opts]
 * @returns {(QwenTtsModel|AsyncHandle)}
 */
bro.tts.loadQwen = function(dir, opts) {};

/**
 * @param {string} dir
 * @param {SupertonicLoadOptions} [opts]
 * @returns {(SupertonicModel|AsyncHandle)}
 */
bro.tts.loadSupertonic = function(dir, opts) {};

/**
 * @param {string} dir
 * @param {SpeakerEncoderLoadOptions} [opts]
 * @returns {(SpeakerEncoder|AsyncHandle)}
 */
bro.tts.loadSpeakerEncoder = function(dir, opts) {};

/**
 * @param {string} text
 * @param {Object} [opts]
 * @returns {Int32Array}
 */
bro.tts.phonemize = function(text, opts) {};

/**
 * @param {string} dir
 */
bro.tts.setAssetRoot = function(dir) {};

/**
 * @param {TtsAssetsOptions} opts
 */
bro.tts.setAssets = function(opts) {};

/**
 * @param {Object} model
 * @param {*} textOrPhonemes
 * @param {*} voiceOrOpts
 * @param {Object} [opts]
 * @returns {AsyncHandle}
 */
bro.tts.synthesize = function(model, textOrPhonemes, voiceOrOpts, opts) {};

/**
 * @param {Object} model
 * @param {*} textOrChunks
 * @param {*} voiceOrOpts
 * @param {Object} [opts]
 * @returns {AsyncHandle}
 */
bro.tts.synthesizeStream = function(model, textOrChunks, voiceOrOpts, opts) {};

/**
 * @param {KokoroModel} kokoro
 * @param {Voice} voice
 * @param {Float32Array} asr
 * @param {Float32Array} F0
 * @param {Float32Array} N
 * @param {number} nPhonemes
 * @param {Object} [opts]
 * @returns {AsyncHandle}
 */
bro.tts.decodeFrom = function(kokoro, voice, asr, F0, N, nPhonemes, opts) {};

