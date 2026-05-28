/**
 * bro.wake — Streaming wake-word detection
 *
 * Always-on detector that fires a JS callback when a target keyword is
 * spoken. Backed by brosoundml::WakeWord (BC-ResNet-style streaming
 * convolutional classifier over log-mel features) and driven by broaudio's
 * low-latency mic tap. Inference cost is ~0.26 ms / 10 ms frame on
 * CPU; the wake model stays loaded for the lifetime of the listen() call.
 *
 * Threading: WakeWord::feed() runs inline on the audio thread for sub-frame
 * latency. The onFire callback runs on the JS main thread — drained once
 * per engine frame by the binding's per-frame pump. You can safely touch
 * the DOM, start a TTS playback, etc. from onFire.
 *
 * Only one wake detector is active at a time. Calling listen() again
 * replaces the previous detector; the binding stops the old one first.
 *
 * Default trained model: weights/wake/computer.bw (the "computer" keyword,
 * trained with WakeWord::set_threshold(0.85) yielding 0% FRR / 0% FPR on
 * the eval set with comfortable margin).
 */


// ── Start listening ─────────────────────────────────────────────────────────

/**
 * Load a wake-word model and begin scanning the mic input. The binding
 * starts mic capture automatically if it isn't already running.
 *
 * @param {Object}   opts
 * @param {string}   opts.weights         - Path to a .bw checkpoint file.
 *                                          Resolved against the app's base
 *                                          path / asset mounts (so
 *                                          '/lib/wake/computer.bw' works).
 * @param {Function} opts.onFire          - Called with no arguments each
 *                                          time the smoothed score crosses
 *                                          the threshold (debounced by
 *                                          refractoryMs).
 * @param {number}   [opts.threshold=0.85]  - Sigmoid score threshold per
 *                                            frame. 0.85 leaves margin
 *                                            against real-world drift;
 *                                            drop to 0.55 for more
 *                                            aggressive triggering.
 * @param {Object}   [opts.smoothing]     - { hits, window } — fire only
 *                                          when `hits` of the last `window`
 *                                          frames cleared the threshold.
 *                                          Defaults from the trained model.
 * @param {number}   [opts.refractoryMs=500] - Ignore re-fires for this long
 *                                             after a hit.
 * @param {string}   [opts.device]        - 'cpu' | 'cuda'. Defaults to CUDA
 *                                          when a CUDA build is available,
 *                                          else CPU. Inference is small
 *                                          enough that CPU is the right
 *                                          default for most apps.
 */
bro.wake.listen({
    weights: '/lib/wake/computer.bw',
    threshold: 0.85,
    smoothing: { hits: 2, window: 3 },
    refractoryMs: 500,
    onFire: () => {
        // Wake word detected — start the utterance capture / STT loop here.
        console.log('wake!', bro.wake.lastScore());
    },
});


// ── Stop / pause ────────────────────────────────────────────────────────────

bro.wake.stop();      // detach the mic callback + free the detector.
                      // After stop(), bro.wake.isActive() === false.

bro.wake.suspend();   // temporarily ignore mic frames (the wake model stays
                      // loaded; no inference runs). Use while TTS is playing
                      // or whenever you want to silence the detector without
                      // tearing it down.

bro.wake.resume();    // re-enable mic-frame processing after suspend().


// ── Inspection ──────────────────────────────────────────────────────────────

bro.wake.lastScore();    // most-recent per-frame sigmoid score, [0, 1].
                         // 0 until the detector has processed at least one
                         // frame; useful for tuning / on-screen meters.

bro.wake.isActive();     // true between listen() and stop().
bro.wake.isSuspended();  // true while suspend() is in effect.

bro.wake.stats();        // diagnostic snapshot over the underlying broaudio
                         // mic tap, or null when no detector is active:
                         //   {
                         //     framesDelivered:  N,    // tap callback invocations
                         //     samplesDelivered: N,    // total resampled samples seen by feed()
                         //     rollingPeak:      0.95, // post-AGC rolling peak observed by the tap
                         //   }
                         // Useful for verifying mic frames reach the detector
                         // (framesDelivered climbing) and that the AGC is
                         // lifting input toward training-distribution loudness
                         // (rollingPeak ≈ 0.95).


// ── Runtime tuning ──────────────────────────────────────────────────────────

bro.wake.setThreshold(0.85);  // change the trigger threshold without
                              // reloading the model. Cheap; takes effect
                              // on the next audio-thread frame.


// ── Manual feed (test / scripted scenarios) ─────────────────────────────────

/**
 * Drive the detector synchronously from JS instead of from the mic. The
 * Float32Array is interpreted as mono 16 kHz PCM (the WakeConfig sample
 * rate). Returns true on a fire frame, same contract as the C++ feed().
 * Calls onFire on the next tickWake() drain, just like the mic path.
 */
const fired = bro.wake.feed(float32Pcm);
