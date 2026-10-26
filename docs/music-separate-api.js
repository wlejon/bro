/**
 * =============================================================================
 * bro.music.loadSeparator — music source separation (HTDemucs htdemucs_ft)
 * =============================================================================
 *
 * Music in, four stems out: drums, bass, other, vocals. The model is Demucs
 * v4's htdemucs_ft (Meta; MIT code and weights), run natively by brosoundml
 * on brotensor (brosoundml/htdemucs.h). It is a bag of four hybrid-transformer
 * networks, one fine-tuned per stem:
 *
 *   - stem k comes from network k alone, so asking for `vocals` runs one
 *     network rather than four;
 *   - loading with { stems: ['vocals'] } loads only that one (about 170 MB of
 *     weights on the device instead of 670).
 *
 * Each network sees 7.8 s segments at 44.1 kHz through two branches joined by
 * a cross transformer:
 *   - a spectral branch (STFT 4096 / hop 1024, complex as channels);
 *   - a temporal branch (strided convs over the waveform).
 *
 * separate() does what demucs.apply.apply_model does under
 * yue2-research's sep.py:
 *   1. resample to 44.1 kHz (torchaudio's default kernel);
 *   2. normalise by the mono mix's mean and std;
 *   3. cut 7.8 s segments every 5.85 s, overlap-add them with triangular
 *      weights;
 *   4. de-normalise and resample back to the input rate.
 * The stems come back at the input's rate and length. They sum to about the
 * mix, not exactly.
 *
 * Precision and speed:
 *   - FP32 throughout. The stems match the reference (demucs 4.0.1 in strict
 *     FP32 on CUDA) to about 100 dB SDR per stem.
 *   - On an RTX 4090, 10 s of 48 kHz stereo takes about 80 ms for vocals
 *     alone and 300 ms for all four stems.
 *   - The first call after loading is slower (device allocator and per-batch
 *     constant tables warming up).
 *
 * Weights: the folder brosoundml's tools/htdemucs_convert.py writes:
 *   - config.json;
 *   - drums / bass / other / vocals .safetensors (FP32, about 168 MB each).
 *
 * The separator loads on the GPU by default (opts.device overrides). It runs
 * one job at a time (`busy`).
 *
 * Audio is planar: samples holds channel 0's `frames` samples, then
 * channel 1's. Mono input is separated as the same signal in both channels.
 *
 * @example
 *   // The radio's vocal layer: pull the vocals out of a 10 s YuE2 chunk.
 *   const sep = bro.music.loadSeparator(bro.models.find('htdemucs_ft'),
 *                                       { stems: ['vocals'] });
 *   sep.separate(chunk, {                 // chunk: { samples, channels: 2, sampleRate: 48000 }
 *     stems: 'vocals',
 *     onDone: (r, info) => {
 *       if (info.error || info.cancelled) return;
 *       playVocals(r.stems.vocals);       // { samples, channels: 2, frames, sampleRate: 48000 }
 *     },
 *   });
 */

/**
 * @typedef {Object} SeparatorLoadOptions
 * @property {string|string[]} [stems] - the networks to load; default all four
 * @property {'cuda'|'vulkan'|'metal'|'cpu'} [device] - default: the GPU
 * @property {function(Separator)} [onReady] - load on a work thread; loadSeparator then returns an AsyncHandle
 * @property {function(string)} [onError] - with onReady: the load failed
 */

/**
 * @typedef {Object} SeparateOptions
 * @property {string|string[]} [stems] - the stems to produce, by name, in this order; default every loaded stem in source order
 * @property {number} [sampleRate=48000] - the input's rate when `audio` does not say (a bare Float32Array); overrides audio.sampleRate when given
 * @property {function({done: number, total: number})} [onProgress] - async only: after each network forward (one per stem per batch of two segments)
 * @property {function(?SeparationResult, {cancelled: boolean, error?: string})} [onDone] - run on a work thread; separate() then returns an AsyncHandle
 */

/**
 * @typedef {Object} StemAudio
 * @property {Float32Array} samples - planar stereo, 2 * frames
 * @property {number} channels - always 2
 * @property {number} frames - the input's length
 * @property {number} sampleRate - the input's rate
 */

/**
 * @typedef {Object} SeparationResult
 * @property {string[]} names - the stems produced, in the order asked
 * @property {Object<string, StemAudio>} stems - by name
 */

/**
 * A loaded HTDemucs separator.
 * @class Separator
 */
class Separator {
    /** @readonly @type {boolean} */
    get loaded() {}
    /** @readonly @type {string} 'CUDA' | 'Vulkan' | 'Metal' | 'CPU' */
    get device() {}
    /** @readonly @type {boolean} an async separate() is running */
    get busy() {}
    /** @readonly @type {string[]} every source the model has: ['drums', 'bass', 'other', 'vocals'] */
    get sources() {}
    /** @readonly @type {string[]} the stems this instance can produce (its loaded networks) */
    get stems() {}
    /** @readonly @type {number} the networks' rate, 44100 (input at any rate is resampled) */
    get sampleRate() {}

    /**
     * Separate a mix into stems.
     * @param {{samples: Float32Array, channels?: number, sampleRate?: number}|Float32Array} audio -
     *   planar audio (channels 1 or 2, default 1); a bare Float32Array is mono
     * @param {SeparateOptions} [opts]
     * @returns {SeparationResult|{cancel: function(), wait: function()}} the stems, or an AsyncHandle with opts.onDone
     * @throws {TypeError} a stem this separator did not load, or malformed audio
     * @example
     *   const r = sep.separate(mix, { stems: ['bass', 'vocals'] });
     *   r.names;                 // ['bass', 'vocals']
     *   r.stems.vocals.frames;   // mix's frames
     */
    separate(audio, opts) {}
}

/**
 * Load the HTDemucs htdemucs_ft separator from its folder. Asks read access for
 * the folder first; refused, it throws (onReady form included) an Error with
 * code 'EACCES' and `path` (name 'PermissionRevoked' for a revoked grant).
 * @param {string} modelDir - config.json + the four per-stem safetensors
 * @param {SeparatorLoadOptions} [opts]
 * @returns {Separator|{cancel: function(), wait: function()}} the separator, or an AsyncHandle with opts.onReady
 */
bro.music.loadSeparator = function(modelDir, opts) {};
