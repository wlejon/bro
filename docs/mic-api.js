/**
 * =============================================================================
 * bro.mic — live mic chunk consumer (fixed-size frames)
 * =============================================================================
 *
 * A general-purpose live-microphone consumer built on broaudio's multi-consumer
 * mic-tap dispatch. Where bro.wake feeds a model that does its own internal
 * framing, bro.mic asks broaudio for FIXED-SIZE frames (chunkFrames) and hands
 * each one to JS at a steady cadence: the worked example of broaudio's
 * chunkFrames feature. A 16 kHz / 160-frame tap yields exactly one onChunk per
 * 10 ms of audio (100 chunks/sec).
 *
 * broaudio owns the whole capture-side DSP: the polyphase resampler (mic rate →
 * targetRate), the AGC, and the fixed-size chunk slicing. The tap callback runs
 * on the audio thread and only summarises each frame (peak + RMS) into a
 * lock-free ring; onChunk is drained on the JS main thread once per engine
 * frame, so you can safely touch the DOM from it.
 *
 * Multiple mic consumers coexist: bro.mic and bro.wake each register their own
 * tap and both fan out from the same captured audio, so there are no
 * last-writer-wins races over the mic.
 *
 * @example
 *   // Start live mic capture delivering 10ms chunks
 *   bro.mic.start({
 *     chunkFrames: 160,
 *     targetRate: 16000,
 *     agc: false,
 *     onChunk: (c) => console.log("Chunk index: " + c.index + " peak: " + c.peak),
 *   });
 *
 * @example
 *   // Headless / synthetic feed
 *   bro.mic.start({ chunkFrames: 160, targetRate: 16000, live: false });
 *   const synth = new Float32Array(16000);
 *   bro.mic.feed(synth, bro.mic.engineRate());
 *   const s = bro.mic.stats();
 *   if (s) console.log("Total chunks: " + s.chunkCount);
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * Options for configuring and starting live mic stream capture.
 * @typedef {Object} MicStartOptions
 * @property {number} [chunkFrames] -  Samples per chunk measured at targetRate (default 160 = 10ms @ 16kHz). 0 = resampler cadence.
 * @property {number} [targetRate] -  Sample rate delivered to callback in Hz (default 16000). 0 = engine native rate.
 * @property {boolean} [agc] -  Enable broaudio peak-track AGC (default false).
 * @property {boolean} [live] -  Open recording device (default true). Set false for offline/feed testing.
 * @property {boolean} [samples] -  Deliver raw Float32Array PCM samples in onChunk callback (default false).
 * @property {Function} [onChunk] -  Callback invoked per chunk on the main thread: (chunk: MicChunk) => void.
 * @property {number} [targetPeak] -  AGC target peak level in [0, 1] (default 0.95).
 * @property {number} [halfLifeSec] -  AGC running-peak decay half-life in seconds.
 * @property {number} [noiseGate] -  AGC noise gate threshold (chunks below this do not raise running peak).
 * @property {number} [maxGain] -  AGC maximum gain clamp.
 */

/**
 * Diagnostic statistics for the active microphone tap and chunk ring.
 * @typedef {Object} MicStats
 * @property {number} [framesDelivered] -  Total callback invocations / chunks delivered.
 * @property {number} [samplesDelivered] -  Total audio samples handed to callback.
 * @property {number} [rollingPeak] -  Rolling peak level post-AGC in range [0, 1].
 * @property {number} [chunkCount] -  Total chunks published to lock-free ring.
 * @property {number} [dropped] -  Number of chunks dropped due to main-thread backlog.
 * @property {number} [chunkFrames] -  Configured frame count per chunk.
 */

/**
 * Individual audio chunk structure delivered to onChunk callbacks.
 * @typedef {Object} MicChunk
 * @property {number} [index] -  Absolute monotonic chunk sequence counter.
 * @property {number} [peak] -  Peak amplitude for this chunk in range [0, 1].
 * @property {number} [rms] -  Root Mean Square (RMS) energy level for this chunk.
 * @property {Float32Array} [samples] -  Raw PCM sample buffer (present when opts.samples = true).
 */

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * Real-time microphone audio capture and fixed-size chunk streaming namespace.
 */
/**
 * Register a microphone tap and start chunk delivery.
 *
 * @param {MicStartOptions} [opts] - Capture configuration options
 */
bro.mic.start = function(opts) {};

/**
 * Stop active microphone capture tap and free callback references.
 */
bro.mic.stop = function() {};

/**
 * Check whether a microphone capture tap is currently registered and active.
 * @returns {boolean} Whether capture is active
 */
bro.mic.isActive = function() {};

/**
 * Query the engine's native microphone sample rate in Hz.
 * @returns {number} Native sample rate
 */
bro.mic.engineRate = function() {};

/**
 * Query diagnostic snapshot of tap statistics and ring buffers.
 * @returns {MicStats|null} Tap statistics snapshot or null if inactive
 */
bro.mic.stats = function() {};

/**
 * Get array of recent peak amplitude levels for meter rendering.
 *
 * @param {number} [maxCount] - Maximum number of recent level samples to return
 * @returns {Array<number>} Array of floating-point peak levels
 */
bro.mic.levels = function(maxCount) {};

/**
 * Feed synthetic microphone audio samples for offline and headless testing.
 *
 * @param {Float32Array} samples - Float32Array PCM samples at engine rate
 * @param {number} [sampleRate] - Optional expected sample rate (must match engine rate)
 */
bro.mic.feed = function(samples, sampleRate) {};

