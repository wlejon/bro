/**
 * bro.mic, Live mic chunk consumer (fixed-size frames)
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
 * Companion windowed demo: ../broworkshop/demos/mic-chunks (a scrolling
 * per-chunk level meter). Headless smoke test: tests/smoke_mic_chunks.js.
 */


// ── Start ─────────────────────────────────────────────────────────────────────

/**
 * Register a mic tap and begin delivering fixed-size chunks. Replaces any prior
 * bro.mic consumer (the previous tap is removed first). Starts mic capture
 * automatically unless opts.live is false.
 *
 * @param {Object}   [opts]
 * @param {number}   [opts.chunkFrames=160] - Samples per chunk, measured at
 *                                            targetRate. 160 @ 16 kHz = 10 ms.
 *                                            0 = deliver the resampler's natural
 *                                            cadence (variable size).
 * @param {number}   [opts.targetRate=16000] - Rate handed to the callback.
 *                                             0 = the engine's native mic rate
 *                                             (no resampling).
 * @param {boolean}  [opts.agc=false]       - Enable broaudio's peak-track AGC
 *                                            (lifts quiet input toward a target
 *                                            loudness). Off by default so the
 *                                            meter shows true input level.
 * @param {boolean}  [opts.live=true]       - Open the recording device. Set
 *                                            false for headless/offline use and
 *                                            drive the tap with bro.mic.feed().
 * @param {boolean}  [opts.samples=false]   - Deliver each chunk's raw PCM:
 *                                            onChunk's argument gains a
 *                                            `samples` Float32Array
 *                                            (chunkFrames mono samples at
 *                                            targetRate). Requires a fixed
 *                                            chunkFrames (> 0). This is the
 *                                            capture path for recording /
 *                                            STT consumers: concatenate the
 *                                            chunks for the utterance.
 * @param {Function} [opts.onChunk]         - Called per chunk on the JS thread
 *                                            with { index, peak, rms, samples? }.
 *                                            index is the absolute chunk counter
 *                                            (gaps indicate dropped chunks).
 * @param {number}   [opts.targetPeak]      - AGC: target peak (default 0.95).
 * @param {number}   [opts.halfLifeSec]     - AGC: running-peak decay half-life.
 * @param {number}   [opts.noiseGate]       - AGC: chunks below this don't raise
 *                                            the running peak (ignores hiss).
 * @param {number}   [opts.maxGain]         - AGC: clamp on applied gain.
 *
 * @example
 *   bro.mic.start({
 *     chunkFrames: 160, targetRate: 16000, agc: false,
 *     onChunk: (c) => meter.push(c.peak),
 *   });
 */
bro.mic.start = function (opts) {};


// ── Stop ──────────────────────────────────────────────────────────────────────

/**
 * Remove the tap and free the onChunk callback. Does NOT stop mic capture,
 * other consumers (e.g. bro.wake) may share the device. Safe to call when not
 * started.
 */
bro.mic.stop = function () {};


// ── Offline / headless feed ─────────────────────────────────────────────────

/**
 * Push synthetic mic-rate audio through the active tap exactly as the recording
 * callback would (resample → AGC → chunk → onChunk / ring). For headless tests
 * and offline replay. Throws if live capture is active (feed and the recording
 * callback would race the same per-tap state): pair it with start({ live:false }).
 *
 * @param {Float32Array} samples       - Mono PCM at the engine mic rate.
 * @param {number}       [sampleRate]  - If given, must equal bro.mic.engineRate().
 *
 * @example
 *   bro.mic.start({ chunkFrames: 160, targetRate: 16000, live: false });
 *   bro.mic.feed(synth, bro.mic.engineRate());
 *   const s = bro.mic.stats();   // s.chunkCount, s.rollingPeak, ...
 */
bro.mic.feed = function (samples, sampleRate) {};


// ── Introspection ─────────────────────────────────────────────────────────────

/**
 * @returns {boolean} Whether a tap is currently registered.
 */
bro.mic.isActive = function () {};

/**
 * @returns {number} The engine's native mic sample rate (Hz). bro.mic.feed
 *                   expects samples at this rate.
 */
bro.mic.engineRate = function () {};

/**
 * Diagnostic snapshot of the underlying broaudio tap plus the binding's chunk
 * ring. Returns null when no tap is installed.
 *
 * @returns {?{
 *   framesDelivered:  number,  // callback invocations (== chunks, post-slicing)
 *   samplesDelivered: number,  // total samples handed to the callback
 *   rollingPeak:      number,  // tap's rolling peak (post-AGC), [0, ~1]
 *   chunkCount:       number,  // total chunks published to the ring
 *   dropped:          number,  // chunks the main-thread drain had to skip
 *   chunkFrames:      number,  // configured frames per chunk
 * }}
 */
bro.mic.stats = function () {};

/**
 * Snapshot of the most recent chunk peaks, oldest-first, for a polling level
 * meter. Each value is a peak in [0, ~1].
 *
 * @param {number} [maxCount] - Cap the number returned (default: all buffered).
 * @returns {number[]}
 */
bro.mic.levels = function (maxCount) {};
