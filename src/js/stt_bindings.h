#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// Install the `bro.stt` namespace — speech-to-text via the brosoundml +
// brolm siblings. Exposes the Whisper encoder/decoder pipeline plus its
// byte-level BPE tokenizer as opaque handle classes (Whisper,
// WhisperTokenizer) and a one-call `bro.stt.loadWhisper(modelDir, opts)`
// loader.
//
// Runs on GPU by default — loadWhisper places the model on CUDA when a GPU
// backend is available (opts.device: 'cuda' | 'cpu' to override). Audio fed to
// transcribe() must be 16 kHz mono FP32; resampling is the caller's
// responsibility (use brokit's resampler or bro.audio).
void installSttBindings(JSContext* ctx);

// Symmetric cleanup hook. No-op today.
void cleanupSttBindings(JSContext* ctx);

}  // namespace bro::js
