#pragma once

// Shared plumbing for the WebGL2 binding (webgl_*.cpp, one file per call
// family): the cells behind the object values JS holds, their ===-stable
// wrappers, and the backend each call runs against.
//
// A call goes to the context's client (webgl::WebGLClient, client(c)), which
// encodes it for the Vulkan backend (webgl::vk::WebGLVkContext) or, for a
// call that answers, runs it on the backend in a sync() round trip. The
// families not yet encoded call the backend through live(c), which replays
// everything the client holds first, so every call reaches the backend in API
// order. While the context is lost there is neither, and each method does
// nothing and answers what the WebGL spec says a lost context answers: null
// for objects and queries, false for the is* predicates and isEnabled, -1 for
// the attribute and fragment-data locations, WAIT_FAILED and
// FRAMEBUFFER_UNSUPPORTED for the two waits and statuses.
//
// The upload contract: a pointer from embed::typedArrayInfo() lives until
// the next bronze allocation (host_builder.h). An encoded call copies the
// bytes into its record in the statement that reads the pointer; a live(c)
// call or a sync() callback hands it to the backend, which copies it (or, for
// a readback, writes it) before returning, in that statement.

#include "bronze_host/host_builder.h"
#include "bronze_host/host_class.h"

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/webgl2_context.h"
#include "webgl/webgl_objects.h"
#include "webgl/webgl_types.h"

#include <functional>
#include <string>
#include <type_traits>

namespace bro::bronze_host {

using WebGLBackend = webgl::vk::WebGLVkContext;

// ---------------------------------------------------------------------------
// Object cells
// ---------------------------------------------------------------------------

// The payload behind every WebGL object value the binding hands the program
// (WebGLBuffer, WebGLTexture, ... WebGLUniformLocation). One struct for all
// kinds, so there is exactly one finalizer; the kind tag is checked on unwrap.
//
// TEARDOWN ORDER, decided here: the finalizer NEVER touches the backend.
// bronze's collector may prove a texture handle dead long after the Engine —
// and the context with it — has been destroyed, and the finalizer runs
// mid-collection. So deletion remains explicit via deleteBuffer/etc.
struct WebGLCell {
    enum Kind : uint32_t {
        Buffer = 1,
        Texture,
        Program,
        Shader,
        Framebuffer,
        Renderbuffer,
        VertexArray,
        UniformLoc,
        Sampler,
        Sync,
        TransformFeedback,
        Query,
    };
    uint32_t kind = 0;
    GLuint id = 0;
    GLenum shaderType = 0;
    int32_t location = -1;
    uint32_t program = 0;
};

// The one wrapper a live object has, so everything that answers the object —
// its create call, getParameter(*_BINDING), getQuery,
// getFramebufferAttachmentParameter — answers the same value (WebGL's ===
// identity). Held from creation until the object is deleted or the context
// is torn down. null for id 0. ALLOCATES on first sight.
Value webglObject(webgl::WebGL2RenderingContext* c, uint32_t kind, GLuint id, GLenum shaderType = 0);
// Drop the wrapper of a deleted object; a later lookup makes a new one.
void forgetWebGLObject(webgl::WebGL2RenderingContext* c, uint32_t kind, GLuint id);
// Drop every object wrapper: the context was lost, and its objects with it.
void forgetWebGLObjects(webgl::WebGL2RenderingContext* c);
// Detach every ArrayBuffer mapBufferRange handed out (webgl_buffers.cpp):
// the memory behind them is going away.
void detachWebGLMappings(webgl::WebGL2RenderingContext* c);
// The object getExtension(name) answers, the same one every time; `make`
// builds it the first time. ALLOCATES on first sight.
Value webglExtension(webgl::WebGL2RenderingContext* c, const std::string& name,
                     const std::function<Value()>& make);

// ALLOCATES (makeHandle). null for the -1 location, which is what three.js's
// `location === null` checks expect — where getUniformLocation answers null.
Value wrapUniformLocation(webgl::WebGLUniformLocation loc);

// nullptr for null/undefined/foreign values and kind mismatches — id-0
// fail-soft, so a wrong argument is a no-op rather than a crash.
inline WebGLCell* cellOf(Value v, uint32_t kind) {
    auto* cell = static_cast<WebGLCell*>(ev::handleData(v));
    if (!cell || cell->kind != kind) return nullptr;
    return cell;
}

inline GLuint idOf(Value v, uint32_t kind) {
    auto* cell = cellOf(v, kind);
    return cell ? cell->id : 0;
}

inline webgl::WebGLUniformLocation locOf(Value v) {
    auto* cell = cellOf(v, WebGLCell::UniformLoc);
    if (!cell) return {-1, 0};
    return {cell->location, cell->program};
}

inline webgl::WebGLProgram programOf(Value v) { return {idOf(v, WebGLCell::Program)}; }
inline webgl::WebGLSampler samplerOf(Value v) { return {idOf(v, WebGLCell::Sampler)}; }

// ---------------------------------------------------------------------------
// The live backend
// ---------------------------------------------------------------------------

// What an encoded call goes to: null while the context is lost.
inline webgl::WebGLClient* client(webgl::WebGL2RenderingContext* c) { return c->client(); }

// What a call not yet encoded runs against, every command before it replayed:
// null while the context is lost.
inline WebGLBackend* live(webgl::WebGL2RenderingContext* c) {
    return c ? c->backend() : nullptr;
}

// ---------------------------------------------------------------------------
// Family installers
// ---------------------------------------------------------------------------

// The payload of a context object: the tag (host_class.h) in WebGLCell::kind's
// position, so neither unwrap mistakes one for the other, and the context.
// `c` outlives the program: the Engine owns it until teardown, and nothing
// bronze finalizes ever dereferences it.
struct WebGLContextCell {
    uint32_t tag = kHostWebGLContextTag;
    webgl::WebGL2RenderingContext* context = nullptr;
};

// The context a WebGL2RenderingContext object stands for; nullptr for any
// other value.
inline webgl::WebGL2RenderingContext* contextOf(Value v) {
    auto* cell = static_cast<WebGLContextCell*>(ev::handleData(v));
    return cell && cell->tag == kHostWebGLContextTag ? cell->context : nullptr;
}

// A method of WebGL2RenderingContext.prototype: `fn(c, args)` runs against
// the context `this` is, and a call on anything else is the TypeError a
// browser throws for one ("Illegal invocation").
//
// Every method is installed on the fast host-call path (ObjectBuilder::
// defFast): three.js makes ~15k of these calls a frame. Its contract holds for
// every family, the allocating ones included (create*, get*, getParameter,
// the uploads): the arguments in `a` stay current across allocations as they
// do under def(), and nothing here roots through bronze's runtime::Rooted<>.
template <typename F>
Value callWebGLMethod(F& fn, Value self, std::span<const Value> a) {
    webgl::WebGL2RenderingContext* c = contextOf(self);
    if (!c) return ev::throwTypeError("Illegal invocation");
    return fn(c, a);
}

template <typename F>
void defMethod(ObjectBuilder& b, const char* name, uint32_t arity, F fn) {
    if constexpr (std::is_empty_v<F> && std::is_default_constructible_v<F>) {
        // A captureless body (nearly all of them): the installed callable is
        // captureless too, so the function carries no boxed state to load.
        b.defFast(name, arity, [](Value self, std::span<const Value> a) -> Value {
            F body{};
            return callWebGLMethod(body, self, a);
        });
    } else {
        b.defFast(name, arity, [fn](Value self, std::span<const Value> a) mutable -> Value {
            return callWebGLMethod(fn, self, a);
        });
    }
}

// The families of WebGL2RenderingContext.prototype. dom_globals_webgl.cpp
// calls them in one fixed order; the order of the definitions inside each is
// likewise fixed, so the prototype's shape is reproducible run to run.
void installWebGLConstants(ObjectBuilder& b);
void installWebGLConstants(const HostClass& cls);
void installWebGLState(ObjectBuilder& b);
void installWebGLBuffers(ObjectBuilder& b);
void installWebGLShaders(ObjectBuilder& b);
void installWebGLTextures(ObjectBuilder& b);
void installWebGLFramebuffers(ObjectBuilder& b);
void installWebGLQueries(ObjectBuilder& b);
void installWebGLTransformFeedback(ObjectBuilder& b);

// The context's object: a WebGL2RenderingContext instance for `c`, whose one
// own property, `canvas`, is `canvasValue` (the host canvas object, so
// gl.canvas reads the live width/height). ALLOCATES.
Value createWebGLContextValue(webgl::WebGL2RenderingContext* c, Value canvasValue);
// The WebGLBuffer, WebGLTexture, ... WebGLUniformLocation interfaces, one per
// WebGLCell kind, installed into the current realm: no constructor, the
// prototype each object of that kind is born on.
void installWebGLObjectClasses();

const HostClass& webgl2RenderingContextHostClass();
void installWebGLGlobals();

}  // namespace bro::bronze_host
