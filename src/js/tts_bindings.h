#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// Install the `bro.tts` namespace — text-to-speech via the brosoundml sibling.
// Exposes the Kokoro-82M pipeline as opaque handle classes (Kokoro, Voice)
// and a `bro.tts.loadKokoro(modelDir)` loader.
//
// Kokoro takes already-tokenized phoneme ids — bundling a G2P frontend (the
// upstream misaki G2P) is out of scope for this binding. Output is a mono
// 24 kHz Float32Array, drop-in for an AudioBuffer or a WAV write.
//
// CPU-only today — brosoundml::Kokoro::load throws on non-CPU devices.
void installTtsBindings(JSContext* ctx);

// Symmetric cleanup hook. No-op today.
void cleanupTtsBindings(JSContext* ctx);

}  // namespace bro::js
