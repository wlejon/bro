/**
 * bro.rave — RAVE neural audio autoencoder (ACIDS / IRCAM)
 *
 * RAVE is a small (<20M-param), faster-than-realtime audio autoencoder. A
 * multiband (PQMF) variational convolutional encoder compresses a waveform to a
 * low-rate latent z of shape (nLatent x frames); a residual upsampling decoder
 * resynthesises a waveform from z. Backed by brosoundml (audio-ML inference) on
 * top of brotensor. Defaults to CUDA; pass { device: 'cpu' } to force CPU.
 *
 * The latent axes are PCA-sorted by variance, so they carry interpretable,
 * editable controls:
 *   dim 0  ≈ loudness
 *   dim 1  ≈ pitch / spectral centroid
 *   dim 2+ ≈ timbre
 * Encode a clip, plot the nLatent rows as time-series curves, edit them
 * (pen-tool / LFO / smoothing), then decode the edited latent to morph the
 * audio — the rave-lab use case (generalises kokoro-lab's F0/energy editor to
 * N latent dimensions).
 *
 * Models ship as exported RAVE v2 TorchScript (.ts) and are converted offline to
 * the directory layout this loader reads (config.json + model.safetensors) by
 * brosoundml/scripts/convert-rave.py.
 *
 * Audio is plain mono FP32 in [-1, 1] at the model's rate (rave.sampleRate, e.g.
 * 48000) — resample/downmix before encode(). decode() returns the same shape the
 * other bro audio APIs use: { samples: Float32Array, sampleRate }.
 *
 * Determinism: encode() uses the posterior mean (no sampling); decode() runs the
 * deterministic waveform + loudness branches by default, so encode→decode
 * round-trips reproducibly. Pass { addNoise: true } to also run RAVE's stochastic
 * FFT filtered-noise synthesizer — the breathy / unvoiced / textural energy the
 * deterministic branch can't make. The noise is resampled each call, so the
 * output then varies unless you pin it with a fixed `seed`.
 *
 * Stereo: { channels: 2 } returns an interleaved 2-channel buffer. RAVE has no
 * stereo decoder — it decodes the mono latent once per channel and the two
 * channels decorrelate only via an independent N(0,1) pad on the discarded latent
 * dims (the `stereoWidth` knob). Pin `seed` to make the width reproducible.
 */


// ── Load ──────────────────────────────────────────────────────────────────

/**
 * Load a converted RAVE v2 model from a weights directory.
 *
 * @param {string} modelDir          - directory with config.json + model.safetensors.
 * @param {Object} [opts]
 * @param {string} [opts.device='cuda'] - 'cuda', 'metal', or 'cpu'.
 * @param {function} [opts.onReady]     - async load: onReady(rave).
 * @param {function} [opts.onError]     - async load: onError(message).
 * @returns {Rave|AsyncHandle} - the model (sync), or an AsyncHandle (async, when
 *          opts.onReady is a function).
 *
 * Two modes (same convention as the other bro model loaders):
 *   - Sync (no onReady): blocks until loaded, returns the Rave handle.
 *   - Async (onReady is a function): the heavy load (file IO + GPU upload) runs
 *     on a background thread; onReady(rave) / onError(message) fire later on the
 *     JS thread. Returns an AsyncHandle with .cancel() immediately.
 */
const rave = bro.rave.loadRave('../brosoundml-data/rave/magnets_z8');

// Async load:
// bro.rave.loadRave('../brosoundml-data/rave/magnets_z8', {
//     onReady: (r) => { rave = r; },
//     onError: (msg) => console.error('rave load failed:', msg),
// });

/**
 * Force the brotensor backend probe (CUDA/Metal registration). Loaders call this
 * for you; exposed for parity with the other bro.* model namespaces.
 */
bro.rave.init();


// ── Handle properties ───────────────────────────────────────────────────────

rave.loaded;      // boolean — model is loaded
rave.sampleRate;  // number  — model I/O rate, Hz (e.g. 48000)
rave.nLatent;     // number  — kept latent dims (the curve count, e.g. 8)
rave.fullLatent;  // number  — encoder distribution width pre-PCA-crop (e.g. 128)
rave.nBand;       // number  — PQMF band count (e.g. 16)
rave.totalRatio;  // number  — samples per latent frame (e.g. 2048); a clip of N
                  //           samples encodes to ceil(N / totalRatio) frames


// ── Encode ──────────────────────────────────────────────────────────────────

/**
 * Encode a waveform to its latent representation. Deterministic (posterior mean).
 *
 * @param {Float32Array} audio - mono PCM at rave.sampleRate, nominally [-1, 1].
 *        Right-padded internally to a whole frame (a multiple of totalRatio).
 * @returns {{ latent: Float32Array, nLatent: number, frames: number }}
 *        latent is channel-major: latent[c * frames + t] is dim c at frame t.
 *        So row c (the editable curve for latent dim c) is
 *        latent.subarray(c * frames, (c + 1) * frames).
 */
const { latent, nLatent, frames } = rave.encode(audio);

// Pull out the per-dimension curves for plotting / editing:
const curves = [];
for (let c = 0; c < nLatent; c++) {
    curves.push(latent.subarray(c * frames, (c + 1) * frames));  // length === frames
}
// curves[0] ≈ loudness over time, curves[1] ≈ pitch, ...


// ── Decode ──────────────────────────────────────────────────────────────────

/**
 * Decode a latent back to a waveform.
 *
 * @param {Float32Array} latent - nLatent * frames floats, channel-major
 *        (latent[c * frames + t]) — the same layout encode() returns. nLatent is
 *        inferred as latent.length / frames and must equal rave.nLatent.
 * @param {number} frames       - frame count (the encode() result's `frames`).
 * @param {Object} [opts]
 * @param {boolean} [opts.addNoise=false] - run the stochastic noise-synth branch
 *        for breathy / textural realism (off = deterministic & reproducible).
 * @param {number}  [opts.seed=0]         - RNG seed for the noise AND the stereo
 *        latent pad; fixing it makes the noisy / stereo output reproducible too.
 * @param {number}  [opts.channels=1]     - >1 returns an INTERLEAVED multi-channel
 *        buffer (samples[t*channels + c]). RAVE has no stereo decoder: it runs the
 *        mono decoder once per channel and the channels decorrelate only via the
 *        per-channel latent pad below.
 * @param {number}  [opts.stereoWidth]    - std of the independent N(0,1) pad on
 *        the discarded latent dims, per channel — the sole source of L/R width.
 *        RAVE-native is 1.0 (the default when channels>1). Larger = wider/looser,
 *        0 = both channels identical.
 * @returns {{ samples: Float32Array, sampleRate: number, channels: number }} -
 *        PCM at rave.sampleRate; frames * totalRatio samples PER channel
 *        (interleaved when channels>1).
 */
const out = rave.decode(latent, frames);
// out.samples.length === frames * rave.totalRatio
// out.sampleRate === rave.sampleRate ; out.channels === 1

// Add breathy/unvoiced texture (varies per call):
const noisy = rave.decode(latent, frames, { addNoise: true });
// Reproducible noisy decode (pin the RNG):
const noisyFixed = rave.decode(latent, frames, { addNoise: true, seed: 42 });

// Stereo decode — interleaved L/R, reproducible width via the seed:
const stereo = rave.decode(latent, frames, { channels: 2, stereoWidth: 1.0, seed: 1 });
// stereo.channels === 2 ; stereo.samples.length === frames * rave.totalRatio * 2
// (interleaved: samples[t*2] = L, samples[t*2 + 1] = R). Feed straight to a
// 2-channel sink, e.g. audioCtx.createClip(stereo.samples, 2).


// ── Morph: encode → edit a curve → decode ────────────────────────────────────

const enc = rave.encode(audio);
const z = enc.latent;                 // mutable copy of the latent grid
// Boost loudness (dim 0) by +1.5 across the whole clip:
for (let t = 0; t < enc.frames; t++) z[0 * enc.frames + t] += 1.5;
// Add slow vibrato to pitch (dim 1):
for (let t = 0; t < enc.frames; t++) z[1 * enc.frames + t] += 0.8 * Math.sin(t * 0.4);
const morphed = rave.decode(z, enc.frames);
// morphed.samples — louder, vibrato'd resynthesis of the original clip
