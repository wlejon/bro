// The WebGL2 context object a program sees and the object wrappers it hands
// out. The context is a WebGL2RenderingContext instance: its methods,
// accessors and constants are the prototype's (dom_globals_webgl.cpp), and
// its one own property is `canvas`. Each object is an instance of its interface
// (WebGLBuffer, WebGLTexture, ... WebGLUniformLocation), born on that
// interface's prototype.

#include "bronze_host/webgl_internal.h"
#include "bronze_host/host_class.h"

#include <string>
#include <unordered_map>

namespace bro::bronze_host {

namespace {

uint64_t objectKey(uint32_t kind, GLuint id) { return static_cast<uint64_t>(kind) << 32 | id; }

void cellDtor(void* data) { delete static_cast<WebGLCell*>(data); }

// One interface per WebGLCell kind, indexed by the kind.
HostClass g_objectClasses[WebGLCell::Query + 1];
constexpr const char* kObjectClassNames[WebGLCell::Query + 1] = {
    nullptr,
    "WebGLBuffer",
    "WebGLTexture",
    "WebGLProgram",
    "WebGLShader",
    "WebGLFramebuffer",
    "WebGLRenderbuffer",
    "WebGLVertexArrayObject",
    "WebGLUniformLocation",
    "WebGLSampler",
    "WebGLSync",
    "WebGLTransformFeedback",
    "WebGLQuery",
};

// ALLOCATES (make).
Value makeCell(const WebGLCell& cell) {
    return g_objectClasses[cell.kind].make(new WebGLCell(cell), cellDtor);
}

void contextCellDtor(void* data) { delete static_cast<WebGLContextCell*>(data); }

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

void installWebGLObjectClasses() {
    for (uint32_t kind = WebGLCell::Buffer; kind <= WebGLCell::Query; ++kind)
        g_objectClasses[kind].install(kObjectClassNames[kind], 0, nullptr, nullptr);
}

Value createWebGLContextValue(webgl::WebGL2RenderingContext* c, Value canvasValue) {
    // The canvas object must survive the instance's allocation.
    ev::Persistent canvas(canvasValue);
    c->addTeardownCallback([](webgl::WebGL2RenderingContext* ctx) { detachWebGLMappings(ctx); });
    auto* cell = new WebGLContextCell;
    cell->context = c;
    ObjectBuilder b(webgl2RenderingContextHostClass().make(cell, contextCellDtor));
    // gl.canvas, an own data property as CanvasRenderingContext2D's is: the
    // real host canvas object, so three.js's reads of gl.canvas.width/height
    // see the live drawing buffer size. It is also the collector's edge from
    // the context to its canvas: a program holding the context holds the
    // canvas, as a plain property and no root.
    b.set("canvas", canvas.get());
    return b.get();
}

}  // namespace bro::bronze_host
