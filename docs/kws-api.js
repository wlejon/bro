/**
 * bro.kws — open-vocabulary streaming keyword spotting
 *           (brosoundml::PhonemeSpotter)
 *
 * The open-vocab sibling of bro.wake: instead of one trained-in keyword,
 * enroll ANY phrase as a phoneme-sequence template and get named spot events
 * from the live mic. PhonemeNet streams per-frame phoneme-class posteriors;
 * the spotter Viterbi-aligns every enrolled template against that stream and
 * fires when an alignment completes above a confidence threshold, gated by a
 * word-boundary entry check, an M-of-N smoother, and a per-template
 * refractory debounce.
 *
 * Three ways to enroll a phrase:
 *   - bro.tts.phonemize(text) ids  →  bro.kws.enroll(name, ids)   (citation)
 *   - reference audio              →  bro.kws.enrollFromAudio(name, samples)
 *   - raw class ids                →  enroll() accepts those too
 *
 * Plumbing: bro.kws is a member of the engine's SHARED listen host — one raw
 * (no-AGC) mic tap + lock-free ring + inference task drive a single PCEN mel
 * front-end (brosoundml::ListenBus) feeding every attached tenant, so running
 * bro.kws alongside bro.sense costs one feature pass and one PhonemeNet
 * forward, and both hear the SAME stream. Result delivery is still bro.kws's
 * own: spot events → onSpot (main thread). No AGC anywhere on this path: the
 * PCEN front-end is loudness-robust by design. bro.wake is a third member of
 * the same host — its AGC-free-trained model hears this stream too.
 *
 * Single-producer rule: enroll/remove/clear/reset share the spotter's feed
 * thread, so they are only allowed while NOT listening — load, enroll, then
 * listen; stop() to change templates.
 *
 * Defaults to CUDA; pass { device: 'cpu' } to force the CPU backend.
 */


// ── Load ────────────────────────────────────────────────────────────────────

/**
 * Load the PhonemeNet checkpoint (.bpm, with its embedded class map) and set
 * the global detector-policy defaults. Synchronous (the model is ~300 K
 * params). Throws while listening.
 *
 * @param {Object} opts
 * @param {string} opts.weights         - PhonemeNet checkpoint path (.bpm).
 * @param {string} [opts.device='cuda'] - 'cuda' or 'cpu'.
 * @param {number} [opts.threshold=0.40]      - geometric-mean posterior over the
 *        matched span must exceed this to fire.
 * @param {number} [opts.refractoryMs=600]    - suppress re-fires of the SAME
 *        template for this long.
 * @param {Object} [opts.smoothing]           - { hits, window }: need `hits`
 *        qualifying frames within the last `window` (M-of-N).
 * @param {number} [opts.minPhonemes=3]       - templates shorter than this
 *        never fire (noise floor).
 * @param {number} [opts.entrySilenceFrames=2]- a match may only begin after
 *        this many recent silence frames (word-boundary entry gate).
 * @param {number} [opts.emissionFloor=0.15]  - per-frame log-posterior floor;
 *        keeps one unreliable transient phoneme (stop burst, glide) from
 *        vetoing a whole citation template. 0 disables.
 * @param {boolean} [opts.enrollGaps=false]   - rhythm templates: when
 *        enrolling FROM AUDIO, keep internal silence runs as TIMED gap states
 *        instead of dropping them. Off, click·gap·click collapses to just
 *        "click"; on, the rhythm itself — sound, a timed gap, sound — is the
 *        template, and a re-performance at the wrong tempo is an illegal
 *        path, not a low score. The matcher is stricter for gap templates
 *        (every state must actually be heard — entry may precede evidence
 *        by a few frames, but floor-riding is bounded), so percussive
 *        gestures usually also want minPhonemes lowered. Enroll rhythm
 *        gestures from a clip recorded in the room they'll be performed in:
 *        the streaming front-end adapts to the ambient, and a mic recording
 *        carries that same ambient into enrollment's offline pass.
 * @param {number} [opts.gapMinFrames=5]      - internal silence shorter than
 *        this (10 ms frames) still collapses out — speech stop closures stay
 *        invisible, so enrollGaps is safe for spoken phrases too.
 * @param {number} [opts.gapTolerance=0.5]    - gap duration window as a
 *        fraction of the enrolled gap g: legal dwell is [g*(1-tol), g*(1+tol)]
 *        frames. Tighten for stricter rhythm matching.
 */
bro.kws.load({ weights: '../brosoundml/weights/phoneme/english.bpm' });
// bro.kws.isLoaded()   === true
// bro.kws.sampleRate() === 16000   (feed()'s expected PCM rate)

/** Drop the spotter, its templates, and any live session. */
// bro.kws.unload();


// ── Enroll ──────────────────────────────────────────────────────────────────

/**
 * Enroll a phrase template from phoneme ids — exactly what
 * bro.tts.phonemize(text) returns. Silence and suprasegmental ids are
 * dropped and duplicate adjacent classes collapsed. Returns the resulting
 * template length (throws if it collapses to empty). Throws while listening.
 *
 * The optional per-template policy object accepts the same keys as load()
 * (threshold, refractoryMs, smoothing, minPhonemes, entrySilenceFrames,
 * emissionFloor, enrollGaps, gapMinFrames, gapTolerance) and overrides the
 * global defaults for this template only.
 *
 * @param {string} name                    - event name passed to onSpot.
 * @param {Int32Array|number[]} phonemeIds - from bro.tts.phonemize(text).
 * @param {Object} [policy]                - per-template policy override.
 * @returns {number} template length in phoneme classes.
 */
const len = bro.kws.enroll('lights-on', bro.tts.phonemize('turn on the lights'));

/**
 * Enroll by example: run reference audio through the model and use its argmax
 * class sequence as the template. Samples must be mono Float32Array at
 * bro.kws.sampleRate(). Throws while listening.
 *
 * @param {string} name
 * @param {Float32Array} samples
 * @param {Object} [policy]
 * @returns {number} template length.
 */
// bro.kws.enrollFromAudio('jingle', refSamples);

/** @returns {boolean} whether the named template existed. Throws while listening. */
// bro.kws.remove('lights-on');
/** Drop all templates. Throws while listening. */
// bro.kws.clear();
/** @returns {string[]} enrolled template names (snapshot while listening). */
const names = bro.kws.templates();
/** Drop all streaming state (posterior ring, DP state, smoothing, refractory).
 *  Keeps weights + templates. Throws while listening. */
// bro.kws.reset();


// ── Listen ──────────────────────────────────────────────────────────────────

/**
 * Start live spotting on the mic. Requires a loaded spotter with at least one
 * enrolled template. The mic tap resamples to the model rate on the audio
 * thread; inference runs on the engine's audio-inference worker; onSpot fires
 * on the main thread.
 *
 * @param {Object} opts
 * @param {function} opts.onSpot - onSpot(name, confidence): an enrolled
 *        template completed an alignment. confidence is the geometric-mean
 *        posterior over the matched span, (0, 1].
 */
bro.kws.listen({
    onSpot: (name, confidence) => {
        console.log('spotted', name, 'at', confidence.toFixed(2));
    },
});

/** Stop listening. Keeps the spotter and its templates — re-enroll or
 *  listen() again without reloading weights. */
// bro.kws.stop();

/**
 * Gate onSpot delivery without freezing the stream (the posterior window and
 * every template's alignment state keep rolling, so resume() never faces a
 * cold matcher). Mirrors bro.wake.suspend/resume.
 */
// bro.kws.suspend();  bro.kws.resume();
// bro.kws.isActive();  bro.kws.isSuspended();

/**
 * Best current prefix progress across all templates, in [0, 1] — how far the
 * furthest-advanced template has matched. Lock-free; poll it from UI code
 * while live (e.g. a "heard so far" meter).
 * @returns {number}
 */
const best = bro.kws.prefixProgress();

/**
 * Per-template alignment telemetry — the spotter's contribution to the fused
 * listening surface. One coherent lock-free snapshot, every entry taken after
 * the SAME posterior frame, pollable from any thread while live. Where onSpot
 * reports a completed match after the fact, this reports partial evidence as
 * it accumulates: poll it alongside bro.sense.snapshot() to fuse "voice is
 * live AND 'hello there' is 5/7 deep, scoring 0.6" seconds before any event
 * fires (gate a heavier tier, light a per-phrase UI meter).
 *
 * `confidence` is the geometric-mean posterior over the matched prefix — the
 * same statistic the firing threshold tests on completion, so it is directly
 * comparable to the template's threshold. `completions` and the last-*frame
 * indices are monotonic (they survive bro.kws.reset()), so diff them between
 * polls like bro.sense's counters. `frames` counts posterior frames over the
 * spotter's life; `generation` bumps whenever the template set changes.
 *
 * @returns {null | {
 *   frames: number, generation: number,
 *   templates: Array<{
 *     name: string,
 *     matched: number,           // prefix depth reached (phonemes)
 *     length: number,            // template length
 *     progress: number,          // matched / length, [0,1]
 *     confidence: number,        // geometric-mean posterior over the prefix
 *     completions: number,       // fires since enroll (monotonic)
 *     lastAdvanceFrame: number,  // `frames` value when matched last grew (-1 = never)
 *     lastFireFrame: number,     // `frames` value of the latest fire (-1 = never)
 *   }>
 * }} null until weights are loaded.
 */
const prog = bro.kws.progress();

/**
 * Diagnostics over the SHARED listen-host mic tap (cf. bro.wake.stats), or
 * null. bro.sense.stats() reports the same tap while both are live.
 * @returns {{framesDelivered, samplesDelivered, rollingPeak}|null}
 */
const stats = bro.kws.stats();

/**
 * Manual feed for tests / scripted scenarios (headless). Samples must be at
 * bro.kws.sampleRate(). Refused while live mic capture is active. The listen
 * host carries ONE stream, so this advances every attached tenant — audio fed
 * here also moves bro.sense's sensors, and vice versa.
 *   - Headless: runs the shared bus synchronously and returns the events
 *     fired during this call as [{ name, confidence }] (onSpot also fires on
 *     the next tick unless suspended).
 *   - Windowed/threaded: writes the live shared ring (events surface via
 *     onSpot) and returns undefined.
 * @param {Float32Array} samples
 * @returns {Array<{name: string, confidence: number}>|undefined}
 */
// const events = bro.kws.feed(chunk);
