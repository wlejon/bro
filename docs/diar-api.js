/**
 * bro.diar — Speaker diarization (streaming Sortformer)
 *
 * "Who spoke when": given 16 kHz mono audio, emits per 80 ms frame an
 * independent activity probability for up to four speakers, with speaker labels
 * assigned in arrival-time order. One model:
 *
 *   - Sortformer (nvidia/diar_streaming_sortformer_4spk-v2.1): a NEST /
 *     FastConformer acoustic encoder (the same backbone Parakeet uses) feeding
 *     an 18-layer Transformer head + a two-layer sigmoid head. End-to-end —
 *     no clustering, no separate VAD or speaker-embedding step.
 *
 * Backed by brosoundml on top of brotensor. Defaults to CUDA; pass
 * { device: 'cpu' } to force the CPU backend.
 *
 * Two ways to drive it:
 *   - Offline: bro.diar.diarize(model, audio, { onDone }) runs the whole clip
 *     in one pass on a background thread. model.diarize(audio) is the blocking
 *     synchronous variant for short clips / tests.
 *   - Streaming: model.createSession() gives a per-stream Arrival-Order Speaker
 *     Cache; session.feed(audio, isLast) accumulates PCM and, on isLast, runs
 *     the streaming loop over the buffered audio, continuing the session's
 *     cache so speaker labels stay stable across calls. A live consumer flushes
 *     a short window each tick (isLast=true) for rolling, low-latency output.
 *
 * Audio is { samples: Float32Array, sampleRate } in [-1, 1] (a bare
 * Float32Array is assumed 16 kHz). The model expects 16 kHz mono — bro.listen /
 * bro.mic taps already deliver that; resample/downmix anything else first.
 *
 * Every result is a Diarization object:
 *   { numFrames, numSpeakers, frameSeconds, probs }
 * where probs is a Float32Array row-major (numFrames, numSpeakers); probs[t*S+s]
 * is the probability speaker s is active in frame t (in [0, 1]), and frame t
 * starts at t * frameSeconds (0.08 s) seconds.
 */


// ── Load ──────────────────────────────────────────────────────────────────

/**
 * Load a Sortformer model from a converted checkpoint directory (config.json +
 * model.safetensors — brosoundml/scripts/convert-sortformer.py).
 *
 * Synchronous unless opts.onReady is given, in which case the load runs on a
 * background thread and returns an AsyncHandle.
 *
 * @param {string} dir                  - checkpoint directory.
 * @param {Object} [opts]
 * @param {string} [opts.device='cuda'] - 'cuda' | 'metal' | 'cpu'.
 * @param {function(Sortformer)} [opts.onReady] - async: model ready.
 * @param {function(string)}     [opts.onError] - async: load failed.
 * @returns {Sortformer|AsyncHandle}
 */
bro.diar.loadSortformer = function (dir, opts) {};

/** Force brotensor backend init (probe GPU). Loaders call this for you. */
bro.diar.init = function () {};


// ── Offline (async) ─────────────────────────────────────────────────────────

/**
 * Diarize a whole clip on a background thread (the JS thread stays responsive).
 * The forward is monolithic — .cancel() drops the result rather than
 * interrupting mid-clip.
 *
 * @param {Sortformer} model
 * @param {{samples:Float32Array, sampleRate:number}|Float32Array} audio
 * @param {Object} [opts]
 * @param {function(Diarization|null, {cancelled:boolean, error?:string})} [opts.onDone]
 * @returns {AsyncHandle}  // .cancel()
 */
bro.diar.diarize = function (model, audio, opts) {};


// ── Sortformer (model handle) ───────────────────────────────────────────────

/**
 * @typedef {Object} Sortformer
 * @property {boolean} loaded
 * @property {number}  sampleRate    - 16000
 * @property {number}  numSpeakers   - 4
 * @property {number}  frameSeconds  - 0.08
 * @property {number}  fcDModel      - FastConformer encoder width (512)
 * @property {number}  tfDModel      - Transformer-head width (192)
 */

/**
 * Synchronous offline diarization (blocks the JS thread — prefer bro.diar.diarize
 * for anything but short clips / tests).
 * @returns {Diarization}
 */
Sortformer.prototype.diarize = function (audio) {};

/** Allocate a streaming session with its own Arrival-Order Speaker Cache. */
Sortformer.prototype.createSession = function () {};


// ── SortformerSession (streaming) ───────────────────────────────────────────

/**
 * @typedef {Object} SortformerSession
 * @property {boolean} loaded
 * @property {number}  numSpeakers
 * @property {number}  frameSeconds
 */

/**
 * Feed the next block of 16 kHz mono PCM. The session accumulates audio; on
 * isLast=true it runs the streaming loop over the buffered PCM — continuing this
 * session's speaker cache so labels stay stable — and returns the activity for
 * the finalized frames. With isLast=false it only buffers (numFrames=0).
 *
 * @param {{samples:Float32Array, sampleRate:number}|Float32Array} audio
 * @param {boolean} [isLast=false]
 * @returns {Diarization}
 */
SortformerSession.prototype.feed = function (audio, isLast) {};

/** Reset the Arrival-Order Speaker Cache for a fresh, unrelated stream. */
SortformerSession.prototype.reset = function () {};


// ── ClusterDiarizer — telling apart similar-sounding voices ─────────────────
//
// Sortformer (and its NeMo reference, confirmed bit-for-bit) collapses
// acoustically SIMILAR voices — e.g. two women in the same pitch range — into a
// single speaker slot: a limit of its 4-slot end-to-end head, which has no
// control over how different two voices must be to count as two people. There is
// no threshold to turn up; the head simply emits one speaker.
//
// ClusterDiarizer is the alternative design for that case. It splits the two jobs
// Sortformer fuses:
//   - WHERE is speech  — Sortformer's per-frame activity, reused purely as a VAD.
//   - WHO is speaking   — each speech window is embedded with an ECAPA-TDNN
//                         x-vector, the embeddings are mean-centered against a
//                         fixed population mean (raw x-vectors sit in a narrow
//                         cone where every cosine is ~0.95; centering makes them
//                         speaker-discriminative), then clustered by cosine. The
//                         cosine threshold is the knob Sortformer lacks.
//
// Trade-off vs Sortformer: one speaker per window, so it does NOT split
// overlapped speech, and it needs ~2.5 s of audio per window for a stable
// x-vector. In return it resolves similar voices Sortformer merges. Speaker count
// is discovered from the threshold; labels are arrival-ordered. Offline only for
// now (whole-clip). Feed it CLEAN 16 kHz mono — bro.listen / bro.mic already
// deliver that; avoid decode/resample round-trips, which blur the speaker margin.

/**
 * Load a clustering diarizer: a Sortformer checkpoint dir (used as the VAD) plus
 * the standalone speaker-encoder artifact dir (config.json + model.safetensors +
 * xvector_mean.f32). Synchronous unless opts.onReady is given.
 *
 * @param {string} sortformerDir        - Sortformer model dir (VAD).
 * @param {string} speakerEncoderDir    - speaker-encoder artifact dir.
 * @param {Object} [opts]
 * @param {string} [opts.device='cuda'] - 'cuda' | 'metal' | 'cpu'.
 * @param {function(ClusterDiarizer)} [opts.onReady] - async: ready.
 * @param {function(string)}          [opts.onError] - async: failed.
 * @returns {ClusterDiarizer|AsyncHandle}
 */
bro.diar.loadClusterDiarizer = function (sortformerDir, speakerEncoderDir, opts) {};

/**
 * Diarize a whole clip on a background thread.
 * @param {ClusterDiarizer} model
 * @param {{samples:Float32Array, sampleRate:number}|Float32Array} audio  // clean 16 kHz
 * @param {Object} [opts]  // Config knobs (below) + onDone
 * @param {function(Diarization|null, {cancelled:boolean, error?:string})} [opts.onDone]
 * @returns {AsyncHandle}  // .cancel()
 */
bro.diar.clusterDiarize = function (model, audio, opts) {};

/**
 * @typedef {Object} ClusterDiarizer
 * @property {boolean} loaded
 */

/**
 * Synchronous whole-clip diarization (blocks the JS thread — prefer
 * bro.diar.clusterDiarize for long clips). Returns a Diarization where probs is
 * one-hot per speech frame and numSpeakers is the discovered count.
 *
 * @param {{samples:Float32Array, sampleRate:number}|Float32Array} audio
 * @param {Object} [cfg]
 * @param {number} [cfg.clusterThreshold=0.40] - centered-cosine merge cut; LOWER
 *        = more speakers (splits more readily), HIGHER = fewer. The main knob.
 * @param {number} [cfg.vadThreshold=0.40]     - speech gate on P(any speaker).
 * @param {number} [cfg.windowSeconds=2.50]    - embedding window (≥~2 s for a
 *        stable x-vector; shorter merges similar voices).
 * @param {number} [cfg.hopSeconds=1.00]       - window shift.
 * @param {number} [cfg.minWindowSeconds=0.60] - skip shorter speech runs.
 * @param {number} [cfg.minSpeakerSeconds=1.00]- fold away clusters this small.
 * @param {number} [cfg.maxSpeakers=8]         - hard cap on discovered speakers.
 * @returns {Diarization}
 */
ClusterDiarizer.prototype.diarize = function (audio, cfg) {};


// ── Example: live diarization over the system audio output ──────────────────

bro.diar.loadSortformer('weights/sortformer/4spk-v2.1', {
  onReady(model) {
    const stream = bro.listen.open('system');   // or 'mic' / { process: pid }
    stream.retain(180);                          // raw-audio ring to pull from
    const session = model.createSession();
    const hop = stream.info().hop;
    let cursor = stream.frame();

    setInterval(() => {
      const newest = stream.frame();
      if ((newest - cursor) * hop < 16000 * 0.8) return;   // ~0.8 s windows
      const pcm = stream.audio(cursor, newest);
      cursor = newest;
      const d = session.feed({ samples: pcm, sampleRate: 16000 }, /*isLast*/ true);
      for (let t = 0; t < d.numFrames; t++) {
        for (let s = 0; s < d.numSpeakers; s++) {
          if (d.probs[t * d.numSpeakers + s] > 0.5)
            console.log('speaker ' + (s + 1) + ' active');
        }
      }
    }, 250);
  },
});
