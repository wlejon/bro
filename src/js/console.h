#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

class Console {
public:
    /// Register the global `console` object (log, warn, error, info, debug, clear).
    static void install(JSContext* ctx);
};

} // namespace bro::js
