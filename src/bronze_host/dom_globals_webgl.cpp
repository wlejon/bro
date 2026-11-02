// WebGL host globals: the WebGL2RenderingContext interface (its prototype
// carries every method, accessor and constant; each context is an instance),
// the WebGL object interfaces, and the WebGLRenderingContext name.

#include "bronze_host/webgl_internal.h"
#include "bronze_host/host_class.h"
#include "bronze_host/host_web_globals.h"
#include "bronze_host/host_html_interfaces.h"

namespace bro::bronze_host {

namespace {

HostClass g_webgl2RenderingContextClass;

}  // namespace

const HostClass& webgl2RenderingContextHostClass() {
    return g_webgl2RenderingContextClass;
}

void installWebGLGlobals() {
    // WebGL2RenderingContext: no constructor; the constants on both the
    // interface and its prototype, as a browser has them.
    g_webgl2RenderingContextClass.install(
        "WebGL2RenderingContext", 0, nullptr,
        [](ObjectBuilder& proto) {
            installWebGLConstants(proto);
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
        });
    installWebGLConstants(g_webgl2RenderingContextClass);
    installWebGLObjectClasses();

    // WebGLRenderingContext: branded constructor
    // One constructor for both the registry and globalThis, minted before
    // globalThis is read: the mint allocates, and an argument list does not
    // fix which of the two is evaluated first.
    ev::Persistent ctor(makeBrandConstructor("WebGLRenderingContext"));
    ev::registerGlobal("WebGLRenderingContext", ctor.get());
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && !gt.value.isUndefined() && ev::isObject(gt.value)) {
        ev::setProperty(gt.value, "WebGLRenderingContext", ctor.get());
    }
}

}  // namespace bro::bronze_host
