#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// Install the `bro.tts` namespace — text-to-speech via the brosoundml sibling.
// Exposes the Kokoro-82M pipeline as opaque handle classes (Kokoro, Voice)
// and a `bro.tts.loadKokoro(modelDir)` loader.
//
// Kokoro takes already-tokenized phoneme ids. A G2P frontend is wired in via
// bro.tts.phonemize(text) (brosoundml's in-tree English Phonemizer) and via
// the Kokoro handle's encodePhonemes(ipa) (just the IPA-codepoint adapter).
// Output of synthesize() is a mono 24 kHz Float32Array, drop-in for an
// AudioBuffer or a WAV write.
//
// Runs on GPU by default — loadKokoro places the model on CUDA when a GPU
// backend is available (opts.device: 'cuda' | 'cpu' to override).
void installTtsBindings(JSContext* ctx);

// Symmetric cleanup hook. No-op today.
void cleanupTtsBindings(JSContext* ctx);

}  // namespace bro::js
