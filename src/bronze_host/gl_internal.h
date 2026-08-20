#pragma once

// Shared plumbing for the bronze-side WebGL2 binding (src/bronze_host).
//
// This is the bronze twin of src/js/webgl2_bindings_util.h: the same
// webgl::WebGL2RenderingContext is wrapped, the same call surface is exposed,
// but the values crossing the boundary are bronze embed Values instead of
// QuickJS JSValues. The QuickJS files remain the reference for the call
// surface and for every GL constant value — nothing here is written from
// memory.
//
// GC DISCIPLINE (the one rule of this layer): bronze's heap is a moving
// semispace collector. A Value held in a plain C++ variable is stale after
// the next allocating embed call; anything held across one lives in a
// bronze::embed::Persistent. Pointers from embed::typedArrayInfo() die at the
// next bronze allocation — every function here that takes one hands it to a
// GL call (which copies into the driver) before any embed call that could
// allocate, and never stores it.

#include "bronze_host/gl_profile.h"

#include "webgl/webgl2_context.h"
#include "webgl/webgl_objects.h"
#include "embed/embed.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// ---------------------------------------------------------------------------
// GL object handles
// ---------------------------------------------------------------------------

// The payload behind every WebGL object value the binding hands the program
// (WebGLBuffer, WebGLTexture, ... WebGLUniformLocation). One struct for all
// kinds, so there is exactly one finalizer; the kind tag is what unwrap
// checks, standing in for the per-kind JSClassID the QuickJS layer uses.
//
// TEARDOWN ORDER, decided here: the finalizer NEVER touches GL. bronze's
// collector may prove a texture handle dead long after the Engine — and with
// it the GL context — has been destroyed, and the finalizer runs
// mid-collection where even a live context must not be called into. So GL
// object lifetime is owned elsewhere: the program deletes explicitly
// (three.js's dispose() path calls gl.deleteTexture and friends), and
// whatever it never deleted dies wholesale when the Engine destroys the
// WebGL2RenderingContext. The finalizer's whole job is freeing this struct.
struct GlCell {
    enum Kind : uint32_t {
        Buffer = 1,
        Texture,
        Program,
        Shader,
        Framebuffer,
        Renderbuffer,
        Vao,
        UniformLoc,
        Sampler,
        Sync,
        Query,
        TransformFeedback,
    };
    uint32_t kind = 0;
    GLuint id = 0;          // GL name (every kind but UniformLoc and Sync)
    GLsync sync = nullptr;  // Sync only
    GLenum shaderType = 0;  // Shader only
    GLint location = -1;    // UniformLoc only
    GLuint program = 0;     // UniformLoc only
};

inline void glCellDtor(void* p) {
    // Mid-collection, possibly after the GL context is gone: free host
    // memory, nothing else (see GlCell).
    delete static_cast<GlCell*>(p);
}

// ALLOCATES (makeHandle).
inline Value wrapGlObj(uint32_t kind, GLuint id, GLenum shaderType = 0) {
    auto* cell = new GlCell{};
    cell->kind = kind;
    cell->id = id;
    cell->shaderType = shaderType;
    return ev::makeHandle(cell, glCellDtor);
}

// ALLOCATES (makeHandle). null for the -1 location, which is what three.js's
// `location === null` checks expect — matching wrapUniformLocation's callers
// in the QuickJS layer, where getUniformLocation answers JS null.
inline Value wrapUniformLocation(webgl::WebGLUniformLocation loc) {
    if (loc.location < 0) return ev::null();
    auto* cell = new GlCell{};
    cell->kind = GlCell::UniformLoc;
    cell->location = loc.location;
    cell->program = loc.program;
    return ev::makeHandle(cell, glCellDtor);
}

inline Value wrapSampler(webgl::WebGLSampler s) {
    if (!s.id) return ev::null();
    return wrapGlObj(GlCell::Sampler, s.id);
}

inline Value wrapSync(webgl::WebGLSync s) {
    if (!s.sync) return ev::null();
    auto* cell = new GlCell{};
    cell->kind = GlCell::Sync;
    cell->sync = s.sync;
    return ev::makeHandle(cell, glCellDtor);
}

inline Value wrapQuery(webgl::WebGLQuery q) {
    if (!q.id) return ev::null();
    return wrapGlObj(GlCell::Query, q.id);
}

// nullptr for null/undefined/foreign values and kind mismatches — the same
// id-0 fail-soft the QuickJS unwrap helpers answer, so a wrong argument is a
// GL no-op rather than a crash.
inline GlCell* cellOf(Value v, uint32_t kind) {
    auto* cell = static_cast<GlCell*>(ev::handleData(v));
    if (!cell || cell->kind != kind) return nullptr;
    return cell;
}

inline GLuint idOf(Value v, uint32_t kind) {
    auto* cell = cellOf(v, kind);
    return cell ? cell->id : 0;
}

inline webgl::WebGLUniformLocation locOf(Value v) {
    auto* cell = cellOf(v, GlCell::UniformLoc);
    if (!cell) return {-1, 0};
    return {cell->location, cell->program};
}

inline webgl::WebGLSampler samplerOf(Value v) {
    return {idOf(v, GlCell::Sampler)};
}

inline webgl::WebGLSync syncOf(Value v) {
    auto* cell = cellOf(v, GlCell::Sync);
    return cell ? webgl::WebGLSync{cell->sync} : webgl::WebGLSync{nullptr};
}

inline webgl::WebGLQuery queryOf(Value v) {
    return {idOf(v, GlCell::Query)};
}

// ---------------------------------------------------------------------------
// The live context
// ---------------------------------------------------------------------------

// Every wrapped call funnels through here, exactly as the QuickJS layer's
// getCtx() does: makeCurrent() re-applies this context's shadow state if
// another canvas (or the engine's own compositing) touched GL since — a
// pointer compare in the common single-canvas case.
inline webgl::WebGL2RenderingContext* live(webgl::WebGL2RenderingContext* c) {
    if (c) c->makeCurrent();
    return c;
}

// ---------------------------------------------------------------------------
// Argument readers
// ---------------------------------------------------------------------------

// Defensive by design: embed::toDouble on an OBJECT is a hard runtime error
// (rt_convert.cpp), and a padded missing argument arrives as undefined (NaN).
// GL argument decoding must never take the process down over a bad call, so
// objects and NaN read as 0 — the same fail-soft the JS_ToInt32 paths in the
// QuickJS files have.
inline double numAt(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return 0.0;
    Value v = args[i];
    if (ev::isObject(v)) return 0.0;
    double d = ev::toDouble(v);
    return std::isnan(d) ? 0.0 : d;
}

inline int32_t i32At(std::span<const Value> args, size_t i) {
    return static_cast<int32_t>(static_cast<int64_t>(numAt(args, i)));
}

inline uint32_t u32At(std::span<const Value> args, size_t i) {
    // GL enums arrive as exact small doubles; the int64 detour keeps values
    // above INT32_MAX (0x8xxxxxxx bitmasks, should one appear) well-defined.
    return static_cast<uint32_t>(static_cast<int64_t>(numAt(args, i)));
}

inline int64_t i64At(std::span<const Value> args, size_t i) {
    return static_cast<int64_t>(numAt(args, i));
}

inline bool boolAt(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return false;
    return ev::toBool(args[i]);
}

inline Value argAt(std::span<const Value> args, size_t i) {
    return i < args.size() ? args[i] : ev::undefined();
}

// ---------------------------------------------------------------------------
// Bulk-data readers (uniform*v, bufferData, texImage2D, ...)
// ---------------------------------------------------------------------------

// A typed array answers its heap bytes directly — VALID ONLY UNTIL THE NEXT
// BRONZE ALLOCATION, so the caller's GL call must be the very next thing that
// happens. A plain JS array (three.js hands those to uniform*fv and
// drawBuffers) is copied element by element into `storage` via embed reads;
// the copy is host memory and stable. False when the value is neither.
// Shared plain-array walk behind the three element flavours below. The
// receiver rides in a Persistent for the whole loop because every
// getProperty/getElement read ALLOCATES (the key string) and would leave a
// raw Value copy pointing into dead from-space — the exact stale-pointer bug
// the GC contract at the top of this header exists to prevent.
template <typename T, typename Convert>
inline bool plainArrayData(Value v, std::vector<T>& storage, Convert convert,
                           const T** outData, size_t* outCount) {
    if (!ev::isObject(v)) return false;
    ev::Persistent root(v);
    Value lenV = ev::getProperty(root.get(), "length");
    if (ev::isUndefined(lenV) || ev::isObject(lenV)) return false;
    uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
    storage.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        Value e = ev::getElement(root.get(), i);
        // NaN (a hole, an undefined pad) converts as 0 for the integer
        // flavours — casting NaN to an integer is UB, not 0.
        double d = ev::isObject(e) ? 0.0 : ev::toDouble(e);
        storage[i] = std::isnan(d) ? T{} : convert(d);
    }
    *outData = storage.data();
    *outCount = n;
    return true;
}

inline bool floatData(Value v, std::vector<float>& storage,
                      const float** outData, size_t* outCount) {
    if (auto info = ev::typedArrayInfo(v)) {
        *outData = reinterpret_cast<const float*>(info.data);
        *outCount = info.byteLength / sizeof(float);
        return true;
    }
    return plainArrayData<float>(
        v, storage, [](double d) { return static_cast<float>(d); }, outData, outCount);
}

inline bool int32Data(Value v, std::vector<int32_t>& storage,
                      const int32_t** outData, size_t* outCount) {
    if (auto info = ev::typedArrayInfo(v)) {
        *outData = reinterpret_cast<const int32_t*>(info.data);
        *outCount = info.byteLength / sizeof(int32_t);
        return true;
    }
    return plainArrayData<int32_t>(
        v, storage,
        [](double d) { return static_cast<int32_t>(static_cast<int64_t>(d)); },
        outData, outCount);
}

inline bool uint32Data(Value v, std::vector<uint32_t>& storage,
                       const uint32_t** outData, size_t* outCount) {
    if (auto info = ev::typedArrayInfo(v)) {
        *outData = reinterpret_cast<const uint32_t*>(info.data);
        *outCount = info.byteLength / sizeof(uint32_t);
        return true;
    }
    return plainArrayData<uint32_t>(
        v, storage,
        [](double d) { return static_cast<uint32_t>(static_cast<int64_t>(d)); },
        outData, outCount);
}

// Raw bytes of a typed array OR an ArrayBuffer, with the element size the
// WebGL2 srcOffset/length overloads count in (1 for a bare buffer) — the
// bronze twin of getBufferDataEx(). The pointer is heap-borrowed: consume it
// in the very next GL call, allocate nothing in between.
inline bool bufferBytes(Value v, const uint8_t** outData, size_t* outLen,
                        size_t* outElemSize) {
    if (auto info = ev::typedArrayInfo(v)) {
        *outData = info.data;
        *outLen = info.byteLength;
        *outElemSize = info.bytesPerElement ? info.bytesPerElement : 1;
        return true;
    }
    if (auto buf = ev::arrayBufferInfo(v)) {
        *outData = buf.data;
        *outLen = buf.byteLength;
        *outElemSize = 1;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Deterministic property registration
// ---------------------------------------------------------------------------

// Builds the context object property by property. The Persistent is the GC
// anchor: setProperty allocates and may move the object, so every def() both
// re-reads the current address and stores the post-call one. Registration
// order is exactly source order — a fixed sequence of def() calls, never an
// iteration over an unordered container — which is what keeps the object's
// shape (and bronze's inline caches over it) deterministic run to run.
struct ObjectBuilder {
    ev::Persistent obj;

    ObjectBuilder() : obj(ev::createObject()) {}
    explicit ObjectBuilder(Value existing) : obj(existing) {}

    void set(const char* name, Value v) {
        // `v` is not held across any allocation here: setProperty roots its
        // arguments internally, and obj.get() does not allocate.
        obj.set(ev::setProperty(obj.get(), name, v));
    }

    void def(const char* name, uint32_t arity, ev::NativeFn fn) {
        // hostProfileWrap is the identity unless BRO_GL_PROFILE=1 (gl_profile.h);
        // makeFunction allocates, so it runs BEFORE obj.get() is read.
        Value f = ev::makeFunction(hostProfileWrap(name, std::move(fn)), arity);
        obj.set(ev::setProperty(obj.get(), name, f));
    }

    void accessor(const char* name, ev::NativeFn getter, ev::NativeFn setter) {
        // The getter must survive the setter's allocation, hence the
        // Persistent bridge between the two makeFunction calls.
        ev::Persistent g(ev::makeFunction(hostProfileWrap(name, std::move(getter)), 0));
        Value s = setter ? ev::makeFunction(hostProfileWrap(name, std::move(setter)), 1)
                         : ev::undefined();
        obj.set(ev::defineAccessor(obj.get(), name, g.get(), s, /*enumerable=*/true));
    }

    Value get() const { return obj.get(); }
};

// ---------------------------------------------------------------------------
// Family installers (one per file, mirroring src/js/webgl2_bindings_*.cpp)
// ---------------------------------------------------------------------------

// Each takes the under-construction context object and the wrapped context.
// gl_context.cpp calls them in one fixed order; the order of def() calls
// inside each is likewise fixed. `c` outlives the program: the Engine owns it
// until teardown, and nothing bronze finalizes ever dereferences it.
void installGlConstants(ObjectBuilder& b);
void installGlState(ObjectBuilder& b, webgl::WebGL2RenderingContext* c);
void installGlBuffers(ObjectBuilder& b, webgl::WebGL2RenderingContext* c);
void installGlShaders(ObjectBuilder& b, webgl::WebGL2RenderingContext* c);
void installGlTextures(ObjectBuilder& b, webgl::WebGL2RenderingContext* c);
void installGlFramebuffers(ObjectBuilder& b, webgl::WebGL2RenderingContext* c);
void installGlQueries(ObjectBuilder& b, webgl::WebGL2RenderingContext* c);

// The whole context object: constants + every family + gl.canvas +
// drawingBufferWidth/Height + the constructor-name shim three.js sniffs.
// `canvasValue` is the host canvas object (dom_globals.cpp) so gl.canvas
// answers live width/height. ALLOCATES heavily; returns the finished object.
Value createGlContextValue(webgl::WebGL2RenderingContext* c, Value canvasValue);

}  // namespace bro::bronze_host
