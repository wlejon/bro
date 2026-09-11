// Assembly of the WebGL2 context object a bronze-compiled program sees.
// Methods and constants land on the instance with reproducible property shape.
//
// Registration order is FIXED: constants first, then the families in the
// order below, then the instance extras. Every step is a source-ordered
// def()/set() sequence — nothing unordered ever feeds property creation, so
// the object's shape is byte-for-byte reproducible run to run.

#include "bronze_host/gl_internal.h"

namespace bro::bronze_host {

Value createGlContextValue(webgl::WebGL2RenderingContext* c, Value canvasValue) {
    // The canvas object must survive everything the build below allocates.
    ev::Persistent canvas(canvasValue);

    ObjectBuilder b;
    installGlConstants(b);
    installGlState(b, c);
    installGlBuffers(b, c);
    installGlShaders(b, c);
    installGlTextures(b, c);
    installGlFramebuffers(b, c);
    installGlQueries(b, c);

    // gl.canvas — the real host canvas object, so three.js's
    // state.reset()-era reads of gl.canvas.width/height see the live drawing
    // buffer size instead of a snapshot.
    b.set("canvas", canvas.get());

    // drawingBufferWidth/Height, live from the context's FBO size.
    b.accessor("drawingBufferWidth",
               [c](Value, std::span<const Value>) {
                   return ev::fromDouble(live(c)->canvasWidth());
               },
               nullptr);
    b.accessor("drawingBufferHeight",
               [c](Value, std::span<const Value>) {
                   return ev::fromDouble(live(c)->canvasHeight());
               },
               nullptr);

    // three.js sniffs `gl.constructor.name === "WebGL2RenderingContext"`.
    {
        ObjectBuilder ctor;
        Value name = ev::fromUtf8("WebGL2RenderingContext");
        ctor.set("name", name);
        b.set("constructor", ctor.get());
    }

    return b.get();
}

}  // namespace bro::bronze_host
