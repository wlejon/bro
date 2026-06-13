/**
 * bro.gesture — open-vocabulary NON-SPEECH gesture matching
 *               (brosoundml::GestureSpotter)
 *
 * The tier-0 sibling of bro.kws. PhonemeNet is speech-only: run a whistle or a
 * click train through it and it decodes to unstable garbage phonemes that
 * won't even match their own re-performance. bro.gesture matches enrolled
 * NON-speech gestures over the shared listen host's tier-0 SensorHub stream
 * instead, so clicks, taps, knocks, and whistles fire reliably:
 *
 *   rhythm  >= 2 onsets — the inter-onset intervals are the template. A
 *           re-performance whose taps land at the same spacing (within a tempo
 *           tolerance) fires. A single lone transient is too ambiguous to
 *           enroll (it would fire on every tap), so rhythms need >= 2 hits.
 *   tone    a sustained tonal run — its dominant pitch (and minimum duration)
 *           is the template. A held whistle/hum at the same pitch fires.
 *
 * The kind is decided FROM THE CLIP at enroll: a substantial sustained tonal
 * run makes it a tone (even if its attack tripped an onset); otherwise >= 2
 * onsets make it a rhythm; otherwise it is too sparse and enroll throws.
 *
 * Plumbing: bro.gesture is a member of the engine's shared listen host. It owns
 * no DSP — it reads the SAME per-frame SensorHub snapshot bro.sense produces,
 * so a gesture costs nothing beyond the tier-0 sensors already running. Because
 * of that, bro.gesture only fires while bro.sense is ALSO active (the matcher
 * has no sensors to read otherwise). Result delivery is its own: fired gestures
 * -> onGesture (main thread).
 *
 * Single-producer rule (same as bro.kws): enroll/remove/clear/reset share the
 * matcher's feed thread, so they are only allowed while NOT listening — enroll
 * first, then listen(); stop() to change gestures.
 */


// ── Enroll ────────────────────────────────────────────────────────────────────

/**
 * Enroll a gesture by example: run the reference clip through a private
 * SensorHub, extract its rhythm (onset intervals) or tone (sustained pitch),
 * and store it. Samples must be mono Float32Array at bro.gesture.sampleRate()
 * (16 kHz). Returns the number of beats (rhythm: onsets; tone: 1). Throws if
 * the clip is too sparse to be a gesture. Throws while listening.
 *
 * Record the clip with bro.mic (agc:false, samples:true) in the room the
 * gesture will be performed in — the sensors adapt to the ambient, and the clip
 * carries that same ambient into enrollment.
 *
 * @param {string} name
 * @param {Float32Array} samples
 * @param {Object} [policy]
 * @param {number} [policy.tempoTol=0.40]  - rhythm: each observed inter-onset
 *        interval must be within this fraction of the enrolled one.
 * @param {number} [policy.pitchTol=0.12]  - tone: pitch must be within this
 *        fraction of the enrolled pitch.
 * @param {number} [policy.refractoryFrames=40] - suppress re-fires (~10 ms/frame).
 * @param {number} [policy.minOnsets=2]    - a rhythm needs at least this many onsets.
 * @param {number} [policy.minToneFrames=8]- a tone's run must last at least this long.
 * @returns {number} beats.
 */
// const beats = bro.gesture.enrollFromAudio('double-knock', clip);

/** @returns {boolean} whether the named gesture existed. Throws while listening. */
// bro.gesture.remove('double-knock');
/** Drop all gestures. Throws while listening. */
// bro.gesture.clear();
/** @returns {string[]} enrolled gesture names (snapshot while listening). */
// const names = bro.gesture.templates();
/** Drop streaming match state (onset rings, tone trackers, refractory); keeps
 *  gestures. Throws while listening. */
// bro.gesture.reset();
/** @returns {number} expected PCM rate for enroll clips (16000). */
// bro.gesture.sampleRate();

/**
 * Inspect an enrolled gesture — the legible view of what the clip became, so a
 * tool can show "rhythm · 3 taps · 250/250 ms" or "tone · 1200 Hz".
 *
 * @param {string} name
 * @returns {null | {
 *   name: string,
 *   kind: 'rhythm' | 'tone',
 *   frameMs: number,            // ms per sensor frame
 *   intervalsMs: number[],      // rhythm: inter-onset intervals in ms (empty for tone)
 *   toneHz: number,             // tone: dominant pitch (0 for rhythm)
 *   toneMs: number,             // tone: enrolled run length in ms (0 for rhythm)
 * }} null if no such gesture.
 */
// const view = bro.gesture.inspect('double-knock');


// ── Listen ────────────────────────────────────────────────────────────────────

/**
 * Start matching on the shared listen host. Requires at least one enrolled
 * gesture, and bro.sense.start() must be active for any gesture to fire (the
 * matcher reads the SensorHub snapshot).
 *
 * @param {Object} opts
 * @param {function} opts.onGesture - onGesture(name, confidence, kind, span):
 *        an enrolled gesture re-occurred. confidence is in (0, 1] (1 == exact
 *        reproduction); kind is 'rhythm' or 'tone'. `span` is the matched region
 *        on the SensorHub frames axis: { startFrame, endFrame, matchedFrames }
 *        (rhythm: first..last matched onset; tone: run start..fire) — align with
 *        bro.sense.snapshot().frames to mark it. Backward-compatible: existing
 *        (name, confidence, kind) handlers ignore the 4th arg.
 */
// bro.sense.start({});
// bro.gesture.listen({
//     onGesture: (name, confidence, kind, span) => {
//         console.log('gesture', name, '(' + kind + ')', confidence.toFixed(2),
//                     'frames', span.startFrame, '-', span.endFrame);
//     },
// });

/** Stop matching. Keeps the gestures — re-enroll or listen() again. */
// bro.gesture.stop();
// bro.gesture.isActive();
