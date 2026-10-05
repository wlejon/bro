// Assembly of the WebGL2 context object a bronze-compiled program sees.
// Methods and constants land on the instance with reproducible property shape.
//
// Registration order is FIXED: constants first, then the families in the
// order below, then the instance extras. Every step is a source-ordered
// def()/set() sequence — nothing unordered ever feeds property creation, so
// the object's shape is byte-for-byte reproducible run to run.

#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include <string>
#include <unordered_map>

namespace bro::bronze_host {

namespace {

uint64_t objectKey(uint32_t kind, GLuint id) { return static_cast<uint64_t>(kind) << 32 | id; }

// A context's wrappers: its objects by (kind, name), its extensions by name.
struct Wrappers {
    std::unordered_map<uint64_t, ev::Persistent> objects;
    std::unordered_map<std::string, ev::Persistent> extensions;
};
std::unordered_map<webgl::WebGL2RenderingContext*, Wrappers> s_wrappers;

Wrappers& wrappersOf(webgl::WebGL2RenderingContext* c) {
    auto it = s_wrappers.find(c);
    if (it == s_wrappers.end()) {
        it = s_wrappers.emplace(c, Wrappers{}).first;
        c->addTeardownCallback([](webgl::WebGL2RenderingContext* ctx) { s_wrappers.erase(ctx); });
    }
    return it->second;
}

}  // namespace

Value glObject(webgl::WebGL2RenderingContext* c, uint32_t kind, GLuint id, GLenum shaderType) {
    if (id == 0 || !c) return ev::null();
    auto& objects = wrappersOf(c).objects;
    auto it = objects.find(objectKey(kind, id));
    if (it != objects.end()) return it->second.get();
    ev::Persistent wrapper(wrapGlObj(kind, id, shaderType));
    Value v = wrapper.get();
    objects.insert_or_assign(objectKey(kind, id), std::move(wrapper));
    return v;
}

void forgetGlObject(webgl::WebGL2RenderingContext* c, uint32_t kind, GLuint id) {
    auto it = s_wrappers.find(c);
    if (it != s_wrappers.end()) it->second.objects.erase(objectKey(kind, id));
}

void forgetGlObjects(webgl::WebGL2RenderingContext* c) {
    auto it = s_wrappers.find(c);
    if (it != s_wrappers.end()) it->second.objects.clear();
}

Value glExtension(webgl::WebGL2RenderingContext* c, const std::string& name, const std::function<Value()>& make) {
    auto& extensions = wrappersOf(c).extensions;
    auto it = extensions.find(name);
    if (it != extensions.end()) return it->second.get();
    ev::Persistent wrapper(make());
    Value v = wrapper.get();
    extensions.insert_or_assign(name, std::move(wrapper));
    return v;
}

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
    installGlTransformFeedback(b, c);

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

    // Branded with WebGL2RenderingContext prototype:
    // `gl instanceof WebGL2RenderingContext === true` and
    // `gl.constructor.name === "WebGL2RenderingContext"`.
    return ev::setPrototype(b.get(), webgl2RenderingContextHostClass().prototype());
}

}  // namespace bro::bronze_host
