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
 * deterministic waveform + loudness branches (the model's stochastic FFT
 * noise-synth branch is not applied), so encode→decode round-trips reproducibly.
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
 * Decode a latent back to a waveform. Runs the deterministic synthesis branches.
 *
 * @param {Float32Array} latent - nLatent * frames floats, channel-major
 *        (latent[c * frames + t]) — the same layout encode() returns. nLatent is
 *        inferred as latent.length / frames and must equal rave.nLatent.
 * @param {number} frames       - frame count (the encode() result's `frames`).
 * @returns {{ samples: Float32Array, sampleRate: number }} - mono PCM,
 *        frames * totalRatio samples at rave.sampleRate.
 */
const out = rave.decode(latent, frames);
// out.samples.length === frames * rave.totalRatio
// out.sampleRate === rave.sampleRate


// ── Morph: encode → edit a curve → decode ────────────────────────────────────

const enc = rave.encode(audio);
const z = enc.latent;                 // mutable copy of the latent grid
// Boost loudness (dim 0) by +1.5 across the whole clip:
for (let t = 0; t < enc.frames; t++) z[0 * enc.frames + t] += 1.5;
// Add slow vibrato to pitch (dim 1):
for (let t = 0; t < enc.frames; t++) z[1 * enc.frames + t] += 0.8 * Math.sin(t * 0.4);
const morphed = rave.decode(z, enc.frames);
// morphed.samples — louder, vibrato'd resynthesis of the original clip
