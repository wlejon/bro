#pragma once

// Helper for the `#else` branch of feature-gated bindings (see
// docs/build-options.md). When a subsystem is compiled out (BRO_WITH_X=0),
// its *_bindings.cpp installs a small JS stub instead of the real binding, so
// the JS API surface still exists and apps can feature-detect: the namespace
// is present and reports unavailability, and calling into it throws a clear
// "built without BRO_WITH_X" error rather than being a silent `undefined`.

#include <quickjs.h>

#include <cstring>

namespace bro::js {

// Evaluate a small JS snippet in the global scope. Intended for installing
// unavailable-feature stubs; swallows any exception from the snippet itself.
inline void installFeatureStub(JSContext* ctx, const char* js) {
    JSValue r = JS_Eval(ctx, js, std::strlen(js), "<feature-stub>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeValue(ctx, r);
}

} // namespace bro::js
