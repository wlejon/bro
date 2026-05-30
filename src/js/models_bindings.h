#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// Install the `bro.models` namespace — on-demand model-weight acquisition.
//
// Apps declare the model files they need (typically in their bro.json
// "models" array) and call `bro.models.ensure(specs, { onProgress })` at
// boot; ensure() downloads any that aren't already present into a shared
// user-data cache (`<userDataDir>/bro/models`, override via BRO_MODELS_DIR)
// straight from the artifacts' upstream Hugging Face homes, then returns a
// map of { id: resolved-path } to feed the existing model loaders
// (bro.lm.loadQwen, bro.stt.loadWhisper, bro.wake.listen, ...).
//
// `bro.models.resolve(spec)` returns a path without downloading, preferring an
// existing dev sibling (`spec.dev`) so a source checkout never re-pulls the
// large weights it already has.
//
// Implemented as a bundled JS module (it orchestrates brokit's fetch / fs /
// crypto) evaluated onto the `bro` global at context init.
void installModelsBindings(JSContext* ctx);

}  // namespace bro::js
