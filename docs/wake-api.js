// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * @typedef {Object} WakeSmoothingOptions
 * @property {number} [hits]
 * @property {number} [window]
 */

/**
 * @typedef {Object} WakeListenOptions
 * @property {string} [weights]
 * @property {Function} onFire
 * @property {number} [threshold=0.85]
 * @property {WakeSmoothingOptions} [smoothing]
 * @property {number} [refractoryMs=500]
 * @property {string} [device]
 */

/**
 * @typedef {Object} WakeLoadOptions
 * @property {string} weights
 * @property {string} [device]
 */

/**
 * @typedef {Object} WakeStats
 * @property {number} [framesDelivered]
 * @property {number} [samplesDelivered]
 * @property {number} [rollingPeak]
 * @property {number} [scoreMax]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

class WakeStreamView {

  /**
   * @readonly
   * @type {boolean}
   */
  active;

  /**
   * @param {WakeListenOptions} opts
   */
  listen(opts) {}

  stop() {}

  suspend() {}

  resume() {}

  /**
   * @returns {number}
   */
  lastScore() {}

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
   * @param {number} threshold
   */
  setThreshold(threshold) {}

  /**
   * @returns {WakeStats|null}
   */
  stats() {}

  /**
   * @param {Float32Array} samples
   * @param {number} [sampleRate]
   * @returns {*}
   */
  feed(samples, sampleRate) {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * @param {WakeLoadOptions} opts
 */
bro.wake.load = function(opts) {};

bro.wake.unload = function() {};

/**
 * @param {WakeListenOptions} opts
 */
bro.wake.listen = function(opts) {};

bro.wake.stop = function() {};

bro.wake.suspend = function() {};

bro.wake.resume = function() {};

/**
 * @returns {number}
 */
bro.wake.lastScore = function() {};

/**
 * @returns {boolean}
 */
bro.wake.isActive = function() {};

/**
 * @returns {boolean}
 */
bro.wake.isSuspended = function() {};

/**
 * @returns {boolean}
 */
bro.wake.isLoaded = function() {};

/**
 * @param {number} threshold
 */
bro.wake.setThreshold = function(threshold) {};

/**
 * @returns {WakeStats|null}
 */
bro.wake.stats = function() {};

/**
 * @param {Float32Array} samples
 * @param {number} [sampleRate]
 * @returns {*}
 */
bro.wake.feed = function(samples, sampleRate) {};

