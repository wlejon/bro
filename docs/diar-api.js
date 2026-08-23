// ── Classes & Interfaces ─────────────────────────────────────────────────────

class Sortformer {

  /**
   * Active execution device ('CPU', 'CUDA', or 'Metal').
   * @readonly
   * @type {string}
   */
  device;

  /**
   * True if an asynchronous inference job is active on this model.
   * @readonly
   * @type {boolean}
   */
  busy;

  /**
   * Synchronously run speaker diarization on 16 kHz mono FP32 PCM audio.
   *
   * @param {Float32Array} audio
   * @returns {Object}
   */
  diarize(audio) {}

  /**
   * Create an online streaming diarization session.
   * @returns {SortformerSession}
   */
  createSession() {}

}

class SortformerSession {

  /**
   * Active execution device.
   * @readonly
   * @type {string}
   */
  device;

  /**
   * True if an asynchronous inference job is active.
   * @readonly
   * @type {boolean}
   */
  busy;

  /**
   * Feed streaming 16 kHz PCM chunk and return rolling speaker probabilities.
   *
   * @param {Float32Array} audio
   * @param {boolean} [isLast=true]
   * @returns {Object}
   */
  feed(audio, isLast) {}

  /**
   * Reset session cache state.
   */
  reset() {}

}

class ClusterDiarizer {

  /**
   * Active execution device.
   * @readonly
   * @type {string}
   */
  device;

  /**
   * True if an asynchronous inference job is active.
   * @readonly
   * @type {boolean}
   */
  busy;

  /**
   * Synchronously run clustering diarization with custom threshold options.
   *
   * @param {Float32Array} audio
   * @param {Object} [opts]
   * @returns {Object}
   */
  diarize(audio, opts) {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro.diar — neural speaker diarization (Sortformer & Clustering Diarizer)
 * =============================================================================
 *
 * Speaker diarization and audio separation engine using streaming Sortformer (4-speaker
 * Conformer-Transformer architecture) and offline embedding cluster diarizers.
 * @example
 * bro.diar.loadSortformer("weights/sortformer", {
 *     onReady: (model) => {
 *       const session = model.createSession();
 *       const res = session.feed(pcm16kAudio, true);
 *       console.log(`Detected ${res.numSpeakers} speakers across ${res.numFrames} frames`);
 *     }
 *   });
 */
/**
 * Initialize brotensor runtime device handles.
 */
bro.diar.init = function() {};

/**
 * Asynchronously load Sortformer neural diarization model from directory.
 *
 * @param {string} modelDir
 * @param {Object} [opts]
 * @returns {AsyncHandle}
 */
bro.diar.loadSortformer = function(modelDir, opts) {};

/**
 * Asynchronously run offline diarization on audio clip.
 *
 * @param {Sortformer} model
 * @param {Float32Array} audio
 * @param {Object} [opts]
 * @returns {AsyncHandle}
 */
bro.diar.diarize = function(model, audio, opts) {};

/**
 * Asynchronously load offline cluster diarizer models (embedding + VAD).
 *
 * @param {string} embeddingDir
 * @param {string} vadDir
 * @param {Object} [opts]
 * @returns {AsyncHandle}
 */
bro.diar.loadClusterDiarizer = function(embeddingDir, vadDir, opts) {};

/**
 * Asynchronously run clustering diarization on audio clip.
 *
 * @param {ClusterDiarizer} model
 * @param {Float32Array} audio
 * @param {Object} [opts]
 * @returns {AsyncHandle}
 */
bro.diar.clusterDiarize = function(model, audio, opts) {};

