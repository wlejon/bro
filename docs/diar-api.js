// ── Classes & Interfaces ─────────────────────────────────────────────────────

class Sortformer {

  /**
   * Active execution device ('CPU', 'CUDA', 'Vulkan', or 'Metal').
   * @readonly
   * @type {string}
   */
  device;

  /** @readonly @type {boolean} */ loaded;
  /** @readonly @type {number} */ sampleRate;
  /** @readonly @type {number} */ numSpeakers;
  /** @readonly @type {number} */ frameSeconds;
  /** @readonly @type {number} */ fcDModel;
  /** @readonly @type {number} */ tfDModel;

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

  /** @readonly @type {boolean} */ loaded;

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

  /** @readonly @type {boolean} */ loaded;
  /** @readonly @type {number} */ sampleRate;

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
 * Load Sortformer neural diarization model from directory (synchronously, or
 * asynchronously when `opts.onReady` is given). Like loadClusterDiarizer (both
 * directories), it asks read access first; refused, it throws (onReady form
 * included) an Error with code 'EACCES' and `path` (name 'PermissionRevoked'
 * for a revoked grant).
 *
 * @param {string} modelDir
 * @param {Object} [opts]
 * @returns {(Sortformer|AsyncHandle)}
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
 * Load the offline cluster diarizer: Sortformer's activity is the VAD (where
 * anyone speaks), and the Qwen3-TTS Base ECAPA-TDNN speaker encoder embeds
 * each speech span into an x-vector that is then clustered. An optional
 * `xvector_mean.f32` beside the encoder is used as the centering vector.
 * Like the other loaders it returns the ClusterDiarizer synchronously, or an
 * AsyncHandle (loading on a work thread) when `opts.onReady` is given.
 *
 * @param {string} sortformerDir  the Sortformer checkpoint (as loadSortformer)
 * @param {string} speakerEncoderDir  the Qwen3-TTS speaker encoder
 *   (`speaker_encoder.*` weights)
 * @param {Object} [opts]  `device`, `onReady(model)`, `onError(message)`
 * @returns {ClusterDiarizer|AsyncHandle}
 */
bro.diar.loadClusterDiarizer = function(sortformerDir, speakerEncoderDir, opts) {};

/**
 * Asynchronously run clustering diarization on audio clip.
 *
 * @param {ClusterDiarizer} model
 * @param {Float32Array} audio
 * @param {Object} [opts]
 * @returns {AsyncHandle}
 */
bro.diar.clusterDiarize = function(model, audio, opts) {};

