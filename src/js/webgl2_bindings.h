#pragma once

#include <quickjs.h>

namespace bro::webgl { class WebGL2RenderingContext; }

namespace bro::js {

class WebGL2Bindings {
public:
    /// Register the WebGL2RenderingContext class and WebGL object classes.
    static void install(JSContext* ctx);

    /// Wrap a C++ WebGL2RenderingContext as a JS object.
    static JSValue wrapContext(JSContext* ctx, webgl::WebGL2RenderingContext* glCtx);

    /// Cleanup (call before destroying JSRuntime).
    static void cleanup(JSContext* ctx);
};

} // namespace bro::js
