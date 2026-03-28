#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::audio { class AudioEngine; }

namespace bro::js {

class AudioBindings {
public:
    /// Register AudioContext constructor on the global object.
    static void install(JSContext* ctx, audio::AudioEngine* engine);
    static void cleanup(JSContext* ctx);
};

} // namespace bro::js
