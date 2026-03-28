#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::canvas { class CanvasScene; }

namespace bro::js {

class CanvasBindings {
public:
    static void install(JSContext* ctx);
    static void cleanup(JSContext* ctx);
    static JSValue wrapContext2D(JSContext* ctx, canvas::CanvasScene* scene);
};

} // namespace bro::js
