// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * @typedef {Object} KwsSmoothingOptions
 * @property {number} [hits]
 * @property {number} [window]
 */

/**
 * @typedef {Object} KwsPolicyOptions
 * @property {string} [weights]
 * @property {string} [device]
 * @property {number} [threshold=0.4]
 * @property {number} [refractoryMs=600]
 * @property {KwsSmoothingOptions} [smoothing]
 * @property {number} [minPhonemes=3]
 * @property {number} [entrySilenceFrames=2]
 * @property {number} [emissionFloor=0.15]
 * @property {number} [minCoverage=0]
 * @property {number} [scoreNorm=0]
 * @property {boolean} [enrollGaps=false]
 * @property {number} [gapMinFrames=5]
 * @property {number} [gapTolerance=0.5]
 */

/**
 * @typedef {Object} KwsListenOptions
 * @property {Function} onSpot
 */

/**
 * @typedef {Object} KwsSpan
 * @property {number} [startFrame]
 * @property {number} [endFrame]
 * @property {number} [matchedFrames]
 */

/**
 * @typedef {Object} KwsStateInspection
 * @property {number} [cls]
 * @property {string} [label]
 * @property {boolean} [gap]
 * @property {number} [gapLo]
 * @property {number} [gapHi]
 */

/**
 * @typedef {Object} KwsInspection
 * @property {string} [name]
 * @property {number} [threshold]
 * @property {number} [frameMs]
 * @property {boolean} [hasGaps]
 * @property {Array<KwsStateInspection>} [states]
 */

/**
 * @typedef {Object} KwsTemplateProgress
 * @property {string} [name]
 * @property {number} [matched]
 * @property {number} [length]
 * @property {number} [progress]
 * @property {number} [confidence]
 * @property {number} [completions]
 * @property {number} [lastAdvanceFrame]
 * @property {number} [lastFireFrame]
 */

/**
 * @typedef {Object} KwsProgress
 * @property {number} [frames]
 * @property {number} [generation]
 * @property {Array<KwsTemplateProgress>} [templates]
 */

/**
 * @typedef {Object} KwsPosteriorTopClass
 * @property {number} [cls]
 * @property {string} [label]
 * @property {number} [p]
 */

/**
 * @typedef {Object} KwsPosterior
 * @property {number} [frame]
 * @property {Array<KwsPosteriorTopClass>} [top]
 */

/**
 * @typedef {Object} KwsStats
 * @property {number} [framesDelivered]
 * @property {number} [samplesDelivered]
 * @property {number} [rollingPeak]
 */

/**
 * @typedef {Object} KwsEvent
 * @property {string} [name]
 * @property {number} [confidence]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

class KwsStreamView {

  /**
   * @readonly
   * @type {boolean}
   */
  active;

  /**
   * @param {string} name
   * @param {(Int32Array|Array<number>)} phonemeIds
   * @param {KwsPolicyOptions} [policy]
   * @returns {number}
   */
  enroll(name, phonemeIds, policy) {}

  /**
   * @param {string} name
   * @param {Float32Array} samples
   * @param {KwsPolicyOptions} [policy]
   * @returns {number}
   */
  enrollFromAudio(name, samples, policy) {}

  /**
   * @param {string} name
   * @param {(Int32Array|Array<number>)} classIds
   * @param {KwsPolicyOptions} [policy]
   * @returns {number}
   */
  enrollFromClasses(name, classIds, policy) {}

  /**
   * @param {string} name
   * @returns {KwsInspection|null}
   */
  inspect(name) {}

  /**
   * @param {string} name
   * @returns {boolean}
   */
  remove(name) {}

  clear() {}

  /**
   * @returns {Array<string>}
   */
  templates() {}

  reset() {}

  /**
   * @param {KwsListenOptions} opts
   */
  listen(opts) {}

  stop() {}

  suspend() {}

  resume() {}

  /**
   * @returns {boolean}
   */
  isActive() {}

  /**
   * @returns {boolean}
   */
  isSuspended() {}

  /**
   * @returns {boolean}
   */
  isLoaded() {}

  /**
   * @returns {number}
   */
  sampleRate() {}

  /**
   * @returns {number}
   */
  prefixProgress() {}

  /**
   * @returns {KwsProgress|null}
   */
  progress() {}

  /**
   * @param {number} [topK=3]
   * @returns {KwsPosterior|null}
   */
  posterior(topK) {}

  /**
   * @returns {KwsStats|null}
   */
  stats() {}

  /**
   * @param {Float32Array} samples
   * @returns {Array<KwsEvent>|null}
   */
  feed(samples) {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * @param {KwsPolicyOptions} opts
 */
bro.kws.load = function(opts) {};

bro.kws.unload = function() {};

/**
 * @param {string} name
 * @param {(Int32Array|Array<number>)} phonemeIds
 * @param {KwsPolicyOptions} [policy]
 * @returns {number}
 */
bro.kws.enroll = function(name, phonemeIds, policy) {};

/**
 * @param {string} name
 * @param {Float32Array} samples
 * @param {KwsPolicyOptions} [policy]
 * @returns {number}
 */
bro.kws.enrollFromAudio = function(name, samples, policy) {};

/**
 * @param {string} name
 * @param {(Int32Array|Array<number>)} classIds
 * @param {KwsPolicyOptions} [policy]
 * @returns {number}
 */
bro.kws.enrollFromClasses = function(name, classIds, policy) {};

/**
 * @param {string} name
 * @returns {KwsInspection|null}
 */
bro.kws.inspect = function(name) {};

/**
 * @param {string} name
 * @returns {boolean}
 */
bro.kws.remove = function(name) {};

bro.kws.clear = function() {};

/**
 * @returns {Array<string>}
 */
bro.kws.templates = function() {};

bro.kws.reset = function() {};

/**
 * @param {KwsListenOptions} opts
 */
bro.kws.listen = function(opts) {};

bro.kws.stop = function() {};

bro.kws.suspend = function() {};

bro.kws.resume = function() {};

/**
 * @returns {boolean}
 */
bro.kws.isActive = function() {};

/**
 * @returns {boolean}
 */
bro.kws.isSuspended = function() {};

/**
 * @returns {boolean}
 */
bro.kws.isLoaded = function() {};

/**
 * @returns {number}
 */
bro.kws.sampleRate = function() {};

/**
 * @returns {number}
 */
bro.kws.prefixProgress = function() {};

/**
 * @returns {KwsProgress|null}
 */
bro.kws.progress = function() {};

/**
 * @param {number} [topK=3]
 * @returns {KwsPosterior|null}
 */
bro.kws.posterior = function(topK) {};

/**
 * @returns {KwsStats|null}
 */
bro.kws.stats = function() {};

/**
 * @param {Float32Array} samples
 * @returns {Array<KwsEvent>|null}
 */
bro.kws.feed = function(samples) {};

