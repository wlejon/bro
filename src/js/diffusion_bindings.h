#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// Install the `bro.diffusion` namespace — Stable Diffusion 1.5 inference via
// the brodiffusion sibling library. Exposes the Pipeline / PipelineState
// opaque-handle classes plus createPipeline / init.
//
// Safe to call in the main JS context and in each worker context (a worker
// owns its own Pipeline; only plain cloneable data crosses postMessage).
// CPU-only safe — brodiffusion's CPU backend is always built, so this binding
// is always real, never a stub.
void installDiffusionBindings(JSContext* ctx);

// Symmetric cleanup hook. No-op today: qjsbind owns the class finalizers and
// bro.diffusion is reached from globalThis, so runtime teardown sweeps it.
// Wired for install/cleanup parity with the other binding modules.
void cleanupDiffusionBindings(JSContext* ctx);

} // namespace bro::js
