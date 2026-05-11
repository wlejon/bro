#include "js/image_gpu_bindings.h"

#include "image_gpu.js.h"   // generates `static const char js_image_gpu[] = ...`

#include <cstring>

namespace bro::js {

void ImageGPUBindings::install(JSContext* ctx) {
    JSValue r = JS_Eval(ctx, js_image_gpu, std::strlen(js_image_gpu),
                        "<bro.image.gpu>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        // Surface the error to the JS console rather than swallowing it.
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        if (msg) {
            // Re-throw via a regular JS_Throw so the JS host's error handler
            // logs it consistently with other binding failures.
            JS_ThrowInternalError(ctx, "bro.image.gpu install failed: %s", msg);
            JS_FreeCString(ctx, msg);
        }
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, r);
}

} // namespace bro::js
