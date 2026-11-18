// WebGL host globals: the WebGL2RenderingContext and WebGLRenderingContext
// interfaces (each prototype carries every method, accessor and constant of
// its version; each context is an instance of one) and the WebGL object
// interfaces.

#include "bronze_host/webgl_internal.h"
#include "bronze_host/host_class.h"
#include "bronze_host/host_web_globals.h"
#include "bronze_host/host_html_interfaces.h"

namespace bro::bronze_host {

namespace {

HostClass g_webgl2RenderingContextClass;
HostClass g_webglRenderingContextClass;

// One prototype's members: every family, then the drawing-buffer size.
void installContextPrototype(ObjectBuilder& proto, bool webgl1) {
    installWebGLConstants(proto, webgl1);
    installWebGLState(proto);
    installWebGLBuffers(proto);
    installWebGLShaders(proto);
    installWebGLTextures(proto);
    installWebGLFramebuffers(proto);
    installWebGLQueries(proto);
    installWebGLTransformFeedback(proto);
    // drawingBufferWidth/Height, live from the drawing buffer's size.
    proto.accessor("drawingBufferWidth",
        [](Value self, std::span<const Value>) -> Value {
            webgl::WebGL2RenderingContext* c = contextOf(self);
            if (!c) return ev::throwTypeError("Illegal invocation");
            return ev::fromDouble(c->canvasWidth());
        },
        nullptr);
    proto.accessor("drawingBufferHeight",
        [](Value self, std::span<const Value>) -> Value {
            webgl::WebGL2RenderingContext* c = contextOf(self);
            if (!c) return ev::throwTypeError("Illegal invocation");
            return ev::fromDouble(c->canvasHeight());
        },
        nullptr);
    if (webgl1) removeWebGL2OnlyMethods(proto.get());
}

}  // namespace

const HostClass& webgl2RenderingContextHostClass() {
    return g_webgl2RenderingContextClass;
}

const HostClass& webglRenderingContextHostClass() {
    return g_webglRenderingContextClass;
}

void installWebGLGlobals() {
    // WebGL2RenderingContext and WebGLRenderingContext: no constructor; the
    // constants on both the interface and its prototype, as a browser has
    // them. Two unrelated interfaces, as in a browser (neither prototype
    // inherits the other): a getContext('webgl') context is an instance of
    // the second, with WebGL 1's members only.
    g_webgl2RenderingContextClass.install(
        "WebGL2RenderingContext", 0, nullptr,
        [](ObjectBuilder& proto) { installContextPrototype(proto, false); });
    installWebGLConstants(g_webgl2RenderingContextClass);
    g_webglRenderingContextClass.install(
        "WebGLRenderingContext", 0, nullptr,
        [](ObjectBuilder& proto) { installContextPrototype(proto, true); });
    installWebGLConstants(g_webglRenderingContextClass, true);
    installWebGLObjectClasses();
}

}  // namespace bro::bronze_host
