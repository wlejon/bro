#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

class AIBindings {
public:
    static void install(JSContext* ctx);
    static void cleanup(JSContext* ctx);
};

} // namespace bro::js
