#pragma once

// Shared plumbing for the WebGL2 binding (webgl_*.cpp, one file per call
// family): the cells behind the object values JS holds, their ===-stable
// wrappers, and the backend each call runs against.
//
// Every call goes straight to the context's Vulkan backend
// (webgl::vk::WebGLVkContext). While the context is lost there is no
// backend, and each method does nothing and answers what the WebGL spec says
// a lost context answers: null for objects and queries, false for the is*
// predicates and isEnabled, -1 for the attribute and fragment-data
// locations, WAIT_FAILED and FRAMEBUFFER_UNSUPPORTED for the two waits and
// statuses.
//
// The upload contract: a pointer from embed::typedArrayInfo() lives until
// the next bronze allocation (host_builder.h), and every binding hands it to
// the backend, which copies it, in the statement that reads it.

#include "bronze_host/host_builder.h"

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/webgl2_context.h"
#include "webgl/webgl_objects.h"
#include "webgl/webgl_types.h"

#include <functional>
#include <string>

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

// What a call runs against: null while the context is lost.
inline WebGLBackend* live(webgl::WebGL2RenderingContext* c) {
    return c ? c->backend() : nullptr;
}

// ---------------------------------------------------------------------------
// Family installers
// ---------------------------------------------------------------------------

// Each takes the under-construction context object and the context.
// webgl_context.cpp calls them in one fixed order; the order of def() calls
// inside each is likewise fixed. `c` outlives the program: the Engine owns it
// until teardown, and nothing bronze finalizes ever dereferences it.
void installWebGLConstants(ObjectBuilder& b);
void installWebGLConstants(const HostClass& cls);
void installWebGLState(ObjectBuilder& b, webgl::WebGL2RenderingContext* c);
void installWebGLBuffers(ObjectBuilder& b, webgl::WebGL2RenderingContext* c);
void installWebGLShaders(ObjectBuilder& b, webgl::WebGL2RenderingContext* c);
void installWebGLTextures(ObjectBuilder& b, webgl::WebGL2RenderingContext* c);
void installWebGLFramebuffers(ObjectBuilder& b, webgl::WebGL2RenderingContext* c);
void installWebGLQueries(ObjectBuilder& b, webgl::WebGL2RenderingContext* c);
void installWebGLTransformFeedback(ObjectBuilder& b, webgl::WebGL2RenderingContext* c);

// The whole context object: constants + every family + gl.canvas +
// drawingBufferWidth/Height + prototype branded with WebGL2RenderingContext.
// `canvasValue` is the host canvas object (dom_globals.cpp) so gl.canvas
// answers live width/height. ALLOCATES heavily; returns the finished object.
Value createWebGLContextValue(webgl::WebGL2RenderingContext* c, Value canvasValue);

const HostClass& webgl2RenderingContextHostClass();
void installWebGLGlobals();

}  // namespace bro::bronze_host
