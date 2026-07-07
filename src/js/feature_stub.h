#pragma once

// Helper for the `#else` branch of feature-gated bindings (see
// docs/build-options.md). When a subsystem is compiled out (BRO_WITH_X=0),
// its *_bindings.cpp installs a small JS stub instead of the real binding, so
// the JS API surface still exists and apps can feature-detect: the namespace
// is present and reports unavailability, and calling into it throws a clear
// "built without BRO_WITH_X" error rather than being a silent `undefined`.

#include <quickjs.h>

#include <cstring>
#include <string>

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

// Install `bro.<name>` as an unavailable-feature stub: an object reporting
// `available === false`, wrapped in a Proxy so any *other* property resolves
// to a function that throws "bro.<name> ... compiled without <flag>". Lets
// apps feature-detect (`bro.<name>.available`) and get a clear error if they
// call into a subsystem this build was compiled without.
inline void installUnavailableNamespace(JSContext* ctx, const char* name, const char* flag) {
    std::string js;
    js.reserve(320);
    js += "(function(){var b=(globalThis.bro=globalThis.bro||{});b['";
    js += name;
    js += "']=new Proxy({available:false},{get:function(t,p){"
          "if(p in t)return t[p];"
          "return function(){throw new Error('bro.";
    js += name;
    js += " is unavailable: this build was compiled without ";
    js += flag;
    js += "');};}});})();";
    installFeatureStub(ctx, js.c_str());
}

} // namespace bro::js
