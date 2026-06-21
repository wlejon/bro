#pragma once

#include <string>

extern "C" {
#include "quickjs.h"
}

namespace bro::util { class AssetMounts; }

namespace bro::js {

// Install the `bro.diffusion` namespace — diffusion-model inference via the
// brodiffusion sibling library. Exposes the Pipeline / PipelineState
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

// Set the app-relative path-resolution context (base directory + engine mounts)
// used by loadModel / loadWeights / applyLora / addControlNet /
// loadControlDictionary. Lets apps reference bundled assets with `/app/...` and
// app-relative paths instead of absolute machine paths. Called by the engine on
// app load and by each worker after install (workers inherit the same mounts).
void setDiffusionAppContext(const std::string& basePath,
                            const util::AssetMounts* mounts);

} // namespace bro::js
