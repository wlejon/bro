#pragma once

extern "C" {
#include "quickjs.h"
}

namespace broaudio { class Engine; }

namespace bro::js {

// Install the `bro.mic` namespace — a general live-mic chunk consumer built on
// broaudio's mic-tap dispatch. bro.mic.start() registers a tap configured for a
// target rate, fixed chunk size (chunkFrames), and optional AGC; broaudio owns
// the resampler + AGC + chunk slicing and hands the binding fixed-size frames
// on the audio thread. Each frame's peak/RMS is published into a lock-free ring;
// tickMic() drains it on the main thread and fires the JS onChunk callback.
//
// This is the demonstrable consumer of broaudio's chunkFrames feature: a 16 kHz
// / 160-frame (10 ms) tap yields exactly one onChunk per 10 ms of audio.
//
// The engine owns the broaudio::Engine; the binding holds a raw pointer for the
// life of the JS runtime (broaudio::Engine outlives the JS context).
void installMicBindings(JSContext* ctx, broaudio::Engine* audioEngine);

// Per-frame pump. Drains the per-chunk ring filled by the audio thread and
// invokes the stored JS onChunk callback once per new chunk. Cheap when nothing
// is pending. Call from the main thread per frame.
void tickMic(JSContext* ctx);

// Symmetric cleanup hook. Removes the mic tap and frees the onChunk reference.
// Safe to call multiple times.
void cleanupMicBindings(JSContext* ctx);

}  // namespace bro::js
