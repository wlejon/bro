// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * @typedef {Object} SenseStartOptions
 * @property {number} [vadFloorDb=-55]
 * @property {number} [vadSnrDb=8]
 * @property {number} [vadRiseDbps=6]
 * @property {number} [vadHangFrames=25]
 * @property {number} [onsetRatio=2.5]
 * @property {number} [onsetAbs=0.05]
 * @property {number} [onsetEma=0.05]
 * @property {number} [onsetRefractoryFrames=5]
 * @property {number} [tonalMinPeriodicity=0.6]
 * @property {number} [tonalFminHz=80]
 * @property {number} [tonalFmaxHz=4000]
 */

/**
 * @typedef {Object} SenseSnapshot
 * @property {number} [frames]
 * @property {number} [t]
 * @property {number} [rms]
 * @property {number} [peak]
 * @property {number} [db]
 * @property {boolean} [voice]
 * @property {number} [noiseFloorDb]
 * @property {number} [snrDb]
 * @property {number} [voiceFrames]
 * @property {number} [voiceEvents]
 * @property {number} [lastVoiceFrame]
 * @property {number} [flux]
 * @property {boolean} [onset]
 * @property {number} [onsets]
 * @property {number} [lastOnsetFrame]
 * @property {number} [periodicity]
 * @property {number} [dominantHz]
 * @property {boolean} [tonal]
 * @property {number} [tonalFrames]
 * @property {number} [tonalEvents]
 * @property {number} [lastTonalFrame]
 * @property {number} [centroid]
 */

/**
 * @typedef {Object} SenseStats
 * @property {number} [framesDelivered]
 * @property {number} [samplesDelivered]
 * @property {number} [rollingPeak]
 */

/**
 * @typedef {Object} SenseAnalysis
 * @property {number} [frames]
 * @property {number} [hop]
 * @property {number} [win]
 * @property {number} [rate]
 * @property {number} [frameMs]
 * @property {Float32Array} [db]
 * @property {Float32Array} [dominantHz]
 * @property {Float32Array} [periodicity]
 * @property {Float32Array} [centroid]
 * @property {Int32Array} [flags]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

class SenseStreamView {

  /**
   * @readonly
   * @type {boolean}
   */
  active;

  /**
   * @param {SenseStartOptions} [opts]
   */
  start(opts) {}

  stop() {}

  /**
   * @returns {boolean}
   */
  isActive() {}

  /**
   * @returns {SenseSnapshot|null}
   */
  snapshot() {}

  /**
   * @returns {number}
   */
  sampleRate() {}

  /**
   * @returns {SenseStats|null}
   */
  stats() {}

  /**
   * @param {Float32Array} samples
   * @returns {SenseSnapshot|null}
   */
  feed(samples) {}

  /**
   * @param {Float32Array} samples
   * @param {SenseStartOptions} [opts]
   * @returns {SenseAnalysis|null}
   */
  analyze(samples, opts) {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * @param {SenseStartOptions} [opts]
 */
bro.sense.start = function(opts) {};

bro.sense.stop = function() {};

/**
 * @returns {boolean}
 */
bro.sense.isActive = function() {};

/**
 * @returns {SenseSnapshot|null}
 */
bro.sense.snapshot = function() {};

/**
 * @returns {number}
 */
bro.sense.sampleRate = function() {};

/**
 * @returns {SenseStats|null}
 */
bro.sense.stats = function() {};

/**
 * @param {Float32Array} samples
 * @returns {SenseSnapshot|null}
 */
bro.sense.feed = function(samples) {};

/**
 * @param {Float32Array} samples
 * @param {SenseStartOptions} [opts]
 * @returns {SenseAnalysis|null}
 */
bro.sense.analyze = function(samples, opts) {};

