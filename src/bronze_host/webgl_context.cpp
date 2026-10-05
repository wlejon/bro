// Assembly of the WebGL2 context object a bronze-compiled program sees, and
// the object wrappers it hands out. Methods and constants land on the
// instance with reproducible property shape.
//
// Registration order is FIXED: constants first, then the families in the
// order below, then the instance extras. Every step is a source-ordered
// def()/set() sequence — nothing unordered ever feeds property creation, so
// the object's shape is byte-for-byte reproducible run to run.

#include "bronze_host/webgl_internal.h"
#include "bronze_host/host_internal.h"

#include <string>
#include <unordered_map>

namespace bro::bronze_host {

namespace {

uint64_t objectKey(uint32_t kind, GLuint id) { return static_cast<uint64_t>(kind) << 32 | id; }

void cellDtor(void* data) { delete static_cast<WebGLCell*>(data); }

// ALLOCATES (makeHandle).
Value makeCell(const WebGLCell& cell) { return ev::makeHandle(new WebGLCell(cell), cellDtor); }

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

Value webglObject(webgl::WebGL2RenderingContext* c, uint32_t kind, GLuint id, GLenum shaderType) {
    if (id == 0 || !c) return ev::null();
    auto& objects = wrappersOf(c).objects;
    auto it = objects.find(objectKey(kind, id));
    if (it != objects.end()) return it->second.get();
    WebGLCell cell;
    cell.kind = kind;
    cell.id = id;
    cell.shaderType = shaderType;
    ev::Persistent wrapper(makeCell(cell));
    Value v = wrapper.get();
    objects.insert_or_assign(objectKey(kind, id), std::move(wrapper));
    return v;
}

void forgetWebGLObject(webgl::WebGL2RenderingContext* c, uint32_t kind, GLuint id) {
    auto it = s_wrappers.find(c);
    if (it != s_wrappers.end()) it->second.objects.erase(objectKey(kind, id));
}

void forgetWebGLObjects(webgl::WebGL2RenderingContext* c) {
    auto it = s_wrappers.find(c);
    if (it != s_wrappers.end()) it->second.objects.clear();
}

Value webglExtension(webgl::WebGL2RenderingContext* c, const std::string& name,
                     const std::function<Value()>& make) {
    auto& extensions = wrappersOf(c).extensions;
    auto it = extensions.find(name);
    if (it != extensions.end()) return it->second.get();
    ev::Persistent wrapper(make());
    Value v = wrapper.get();
    extensions.insert_or_assign(name, std::move(wrapper));
    return v;
}

Value wrapUniformLocation(webgl::WebGLUniformLocation loc) {
    if (loc.location < 0) return ev::null();
    WebGLCell cell;
    cell.kind = WebGLCell::UniformLoc;
    cell.location = loc.location;
    cell.program = loc.program;
    return makeCell(cell);
}

Value createWebGLContextValue(webgl::WebGL2RenderingContext* c, Value canvasValue) {
    // The canvas object must survive everything the build below allocates.
    ev::Persistent canvas(canvasValue);

    ObjectBuilder b;
    installWebGLConstants(b);
    installWebGLState(b, c);
    installWebGLBuffers(b, c);
    installWebGLShaders(b, c);
    installWebGLTextures(b, c);
    installWebGLFramebuffers(b, c);
    installWebGLQueries(b, c);
    installWebGLTransformFeedback(b, c);

    // gl.canvas — the real host canvas object, so three.js's
    // state.reset()-era reads of gl.canvas.width/height see the live drawing
    // buffer size instead of a snapshot.
    b.set("canvas", canvas.get());

    // drawingBufferWidth/Height, live from the drawing buffer's size.
    b.accessor("drawingBufferWidth",
               [c](Value, std::span<const Value>) { return ev::fromDouble(c->canvasWidth()); },
               nullptr);
    b.accessor("drawingBufferHeight",
               [c](Value, std::span<const Value>) { return ev::fromDouble(c->canvasHeight()); },
               nullptr);

    // Branded with WebGL2RenderingContext prototype:
    // `gl instanceof WebGL2RenderingContext === true` and
    // `gl.constructor.name === "WebGL2RenderingContext"`.
    return ev::setPrototype(b.get(), webgl2RenderingContextHostClass().prototype());
}

}  // namespace bro::bronze_host
