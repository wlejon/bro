#pragma once

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "quickjs.h"
}

#include <broaudio/engine.h>

namespace bro::js {

class AudioBindings {
public:
    static void install(JSContext* ctx, broaudio::Engine* engine);
    static void cleanup(JSContext* ctx);
};

} // namespace bro::js
