#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// Install the `bro.rave` namespace — RAVE neural audio autoencoder inference via
// the brosoundml sibling. Exposes the model as an opaque handle class:
//   Rave via bro.rave.loadRave(modelDir) — a converted RAVE v2 model
//     (config.json + model.safetensors, the output of brosoundml's
//     scripts/convert-rave.py).
//
// The handle exposes:
//   rave.encode(audio)          mono Float32Array (at rave.sampleRate) ->
//                               { latent: Float32Array, nLatent, frames }
//                               (latent is channel-major: latent[c*frames + t])
//   rave.decode(latent, frames) latent Float32Array -> { samples, sampleRate }
//
// encode() is deterministic (posterior mean); decode() runs the deterministic
// waveform + loudness branches. The latent axes are PCA-sorted by variance —
// dim 0 ~ loudness, dim 1 ~ pitch, the rest timbre — so editing the per-dim time
// series morphs the audio. Runs on GPU by default (opts.device 'cuda' | 'cpu').
void installRaveBindings(JSContext* ctx);

// Symmetric cleanup hook. No-op today.
void cleanupRaveBindings(JSContext* ctx);

}  // namespace bro::js
