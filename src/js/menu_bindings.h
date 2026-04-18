#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::engine { class Engine; }

namespace bro::js {

class MenuBindings {
public:
    static void install(JSContext* ctx, engine::Engine* engine);
};

} // namespace bro::js
