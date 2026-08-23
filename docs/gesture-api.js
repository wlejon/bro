// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * @typedef {Object} GesturePolicyOptions
 * @property {number} [tempoTol=0.4]
 * @property {number} [pitchTol=0.12]
 * @property {number} [pitchStabilityTol=0.06]
 * @property {number} [shapeTol=0.3]
 * @property {number} [refractoryFrames=40]
 * @property {number} [minOnsets=2]
 * @property {number} [minToneFrames=8]
 * @property {number} [onsetSigFrames=5]
 */

/**
 * @typedef {Object} GestureListenOptions
 * @property {Function} onGesture
 */

/**
 * @typedef {Object} GestureOnsetSignature
 * @property {number} [voiced]
 * @property {number} [pitchHz]
 * @property {number} [bright]
 */

/**
 * @typedef {Object} GestureInspection
 * @property {string} [name]
 * @property {string} [kind]
 * @property {number} [frameMs]
 * @property {Array<number>} [intervalsMs]
 * @property {Array<GestureOnsetSignature>} [onsets]
 * @property {number} [toneHz]
 * @property {number} [toneMs]
 * @property {number} [toneSpread]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

class GestureStreamView {

  /**
   * @readonly
   * @type {boolean}
   */
  active;

  /**
   * @param {string} name
   * @param {Float32Array} samples
   * @param {GesturePolicyOptions} [policy]
   * @returns {number}
   */
  enrollFromAudio(name, samples, policy) {}

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

  /**
   * @param {string} name
   * @returns {GestureInspection|null}
   */
  inspect(name) {}

  reset() {}

  /**
   * @param {GestureListenOptions} opts
   */
  listen(opts) {}

  stop() {}

  /**
   * @returns {boolean}
   */
  isActive() {}

  /**
   * @returns {number}
   */
  sampleRate() {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * @param {string} name
 * @param {Float32Array} samples
 * @param {GesturePolicyOptions} [policy]
 * @returns {number}
 */
bro.gesture.enrollFromAudio = function(name, samples, policy) {};

/**
 * @param {string} name
 * @returns {boolean}
 */
bro.gesture.remove = function(name) {};

bro.gesture.clear = function() {};

/**
 * @returns {Array<string>}
 */
bro.gesture.templates = function() {};

/**
 * @param {string} name
 * @returns {GestureInspection|null}
 */
bro.gesture.inspect = function(name) {};

bro.gesture.reset = function() {};

/**
 * @param {GestureListenOptions} opts
 */
bro.gesture.listen = function(opts) {};

bro.gesture.stop = function() {};

/**
 * @returns {boolean}
 */
bro.gesture.isActive = function() {};

/**
 * @returns {number}
 */
bro.gesture.sampleRate = function() {};

