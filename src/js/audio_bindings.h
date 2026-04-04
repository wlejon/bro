#pragma once

extern "C" {
#include "quickjs.h"
}

#include "audio/audio_engine.h"

namespace bro::js {

class AudioBindings {
public:
    /// Register AudioContext constructor and navigator.mediaDevices on the global object.
    static void install(JSContext* ctx, audio::AudioEngine* engine);
    static void cleanup(JSContext* ctx);
};

} // namespace bro::js
