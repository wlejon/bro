#include "models_bindings.h"

#include <cstdio>
#include <cstring>

#include "models.js.h"  // generated: js_models (embed_js in CMakeLists)

namespace bro::js {

// `bro.models` is implemented in JS (src/js/js/models.js) because it just
// orchestrates brokit's fetch / fs / crypto rather than touching the QuickJS C
// API — mirroring how brokit ships its own JS modules. Evaluated once per
// context here.
void installModelsBindings(JSContext* ctx) {
    JSValue r = JS_Eval(ctx, js_models, std::strlen(js_models),
                        "bro:models.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, e);
        std::fprintf(stderr, "[ERROR] [models] install failed: %s\n",
                     msg ? msg : "(no message)");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
}

}  // namespace bro::js
