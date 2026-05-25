#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// Install the `bro.lm` namespace — language-model inference via the brolm
// sibling library. Exposes the Qwen3 decoder LM and its tokenizer as opaque
// handle classes (LMModel, LMTokenizer) plus `bro.lm.loadQwen(ggufPath)` which
// builds both from a single GGUF file.
//
// CPU-safe: brolm's CPU backend is always built, so this binding is always
// real. GGUF Q4_K / Q6_K / Q8_0 weights dispatch through brotensor's fused
// dequant matmuls and are GPU-only — calling generate() on a quantised model
// with no GPU backend throws at the first matmul.
void installLmBindings(JSContext* ctx);

// Symmetric cleanup hook. No-op today: qjsbind owns the class finalizers and
// bro.lm is reached from globalThis, so runtime teardown sweeps it.
void cleanupLmBindings(JSContext* ctx);

}  // namespace bro::js
