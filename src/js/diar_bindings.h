#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// Install the `bro.diar` namespace — speaker diarization via the brosoundml
// sibling. Exposes the streaming Sortformer diarizer
// (nvidia/diar_streaming_sortformer_4spk-v2.1) as opaque handle classes
// (Sortformer, SortformerSession) plus a one-call
// `bro.diar.loadSortformer(modelDir, opts)` loader.
//
// Runs on GPU by default — loadSortformer places the model on CUDA when a GPU
// backend is available (opts.device: 'cuda' | 'cpu' to override). Audio fed to
// diarize()/feed() must be 16 kHz mono FP32; resampling is the caller's
// responsibility (bro.listen / bro.mic taps already deliver 16 kHz).
void installDiarBindings(JSContext* ctx);

// Symmetric cleanup hook. No-op today.
void cleanupDiarBindings(JSContext* ctx);

}  // namespace bro::js
