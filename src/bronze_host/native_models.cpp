// `__bro_native.models` — the C entry points behind bro.models (docs/models-api.js):
// where model weights live. The search list is util/model_dirs.h's: BRO_MODELS,
// then the `models.dirs` setting, then the per-user models directory. The public
// shape is js/models.js's.
//
// Main realm only: the setting is the engine's settings store, which only the
// engine's thread reads. A Worker is handed the directory the page found.

#include "bronze_host/host_runtime.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"
#include "engine/settings.h"
#include "natives/models/native_models_decl.h"
#include "util/model_dirs.h"

#include <string>
#include <vector>

namespace {

std::vector<std::string> currentDirs() {
    std::string setting;
    if (auto* eng = bro::bronze_host::hostEngine()) {
        if (auto* s = eng->settings()) setting = s->getString("models.dirs");
    }
    return bro::util::modelDirs(setting);
}

}  // namespace

extern "C" {

const char* bro_models_dirs(void) {
    std::string joined;
    for (const auto& d : currentDirs()) {
        if (!joined.empty()) joined += '\n';
        joined += d;
    }
    return bro::bronze_host::natives::strResult(std::move(joined));
}

const char* bro_models_find(const char* name) {
    return bro::bronze_host::natives::strResult(
        bro::util::findModel(name ? name : "", currentDirs()));
}

}  // extern "C"

namespace bro::bronze_host {

bool registerNatives_models(std::string* error);

bool registerModelsNatives(std::string* error) {
    return registerNatives_models(error);
}

}  // namespace bro::bronze_host
