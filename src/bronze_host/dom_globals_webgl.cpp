// WebGL host globals: WebGL2RenderingContext and WebGLRenderingContext constructor functions.
// Minted as a proper HostClass with attached static constants and prototype branding.

#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_html_interfaces.h"

namespace bro::bronze_host {

namespace {

HostClass g_webgl2RenderingContextClass;

}  // namespace

const HostClass& webgl2RenderingContextHostClass() {
    return g_webgl2RenderingContextClass;
}

void installWebGLGlobals() {
    // WebGL2RenderingContext: proper HostClass constructor function with attached
    // static constants (COLOR_BUFFER_BIT, RGBA, etc.) and prototype constants so
    // `gl instanceof WebGL2RenderingContext` succeeds and class constants are accessible.
    g_webgl2RenderingContextClass.install(
        "WebGL2RenderingContext", 0, nullptr,
        [](ObjectBuilder& proto) {
            installGlConstants(proto);
        });
    installGlConstants(g_webgl2RenderingContextClass);

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
