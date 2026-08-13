// Framebuffers and renderbuffers — the bronze twin of
// src/js/webgl2_bindings_framebuffers.cpp for the render-target surface
// three.js drives: create/bind/attach/status, renderbuffer storage (plain and
// multisampled), the resolve blit, MRT draw-buffer selection, and readPixels
// into a caller-supplied typed array.
//
// readPixels is the one place GL WRITES into the bronze heap: the destination
// pointer comes from embed::typedArrayInfo and is consumed by the synchronous
// GL call with no bronze allocation in between — the same one-statement
// lifetime every upload pointer in this layer has, in the other direction.

#include "bronze_host/gl_internal.h"

namespace bro::bronze_host {

void installGlFramebuffers(ObjectBuilder& b, webgl::WebGL2RenderingContext* c) {
    // --- Framebuffers ---
    b.def("createFramebuffer", 0, [c](Value, std::span<const Value>) {
        return wrapGlObj(GlCell::Framebuffer, live(c)->createFramebuffer().id);
    });
    b.def("deleteFramebuffer", 1, [c](Value, std::span<const Value> a) {
        live(c)->deleteFramebuffer({idOf(argAt(a, 0), GlCell::Framebuffer)});
        return ev::undefined();
    });
    b.def("bindFramebuffer", 2, [c](Value, std::span<const Value> a) {
        live(c)->bindFramebuffer(u32At(a, 0), {idOf(argAt(a, 1), GlCell::Framebuffer)});
        return ev::undefined();
    });
    b.def("isFramebuffer", 1, [c](Value, std::span<const Value> a) {
        return ev::fromBool(
            live(c)->isFramebuffer({idOf(argAt(a, 0), GlCell::Framebuffer)}) != GL_FALSE);
    });
    b.def("framebufferTexture2D", 5, [c](Value, std::span<const Value> a) {
        live(c)->framebufferTexture2D(u32At(a, 0), u32At(a, 1), u32At(a, 2),
                                      {idOf(argAt(a, 3), GlCell::Texture)}, i32At(a, 4));
        return ev::undefined();
    });
    b.def("framebufferRenderbuffer", 4, [c](Value, std::span<const Value> a) {
        live(c)->framebufferRenderbuffer(u32At(a, 0), u32At(a, 1), u32At(a, 2),
                                         {idOf(argAt(a, 3), GlCell::Renderbuffer)});
        return ev::undefined();
    });
    b.def("checkFramebufferStatus", 1, [c](Value, std::span<const Value> a) {
        return ev::fromDouble(live(c)->checkFramebufferStatus(u32At(a, 0)));
    });

    // readPixels(x, y, w, h, format, type, dstView). The WebGL-level
    // destination validation runs first, exactly as the QuickJS binding runs
    // it: a too-small view records the synthetic INVALID_OPERATION and the
    // driver is never handed an overrunnable pointer.
    b.def("readPixels", 7, [c](Value, std::span<const Value> a) {
        auto info = ev::typedArrayInfo(argAt(a, 6));
        if (!info) return ev::undefined();
        GLint x = i32At(a, 0), y = i32At(a, 1);
        GLsizei w = i32At(a, 2), h = i32At(a, 3);
        GLenum format = u32At(a, 4), type = u32At(a, 5);
        if (live(c)->validateReadPixels(w, h, format, type, info.byteLength)) {
            live(c)->readPixels(x, y, w, h, format, type, info.data);
        }
        return ev::undefined();
    });

    b.def("readBuffer", 1, [c](Value, std::span<const Value> a) {
        live(c)->readBuffer(u32At(a, 0));
        return ev::undefined();
    });

    // drawBuffers(sequence<GLenum>). three.js passes a plain JS array here,
    // which uint32Data copies into host storage via embed element reads —
    // the copy is what the GL call consumes, so the reads' allocations are
    // harmless.
    b.def("drawBuffers", 1, [c](Value, std::span<const Value> a) {
        std::vector<uint32_t> storage;
        const uint32_t* p = nullptr;
        size_t n = 0;
        if (uint32Data(argAt(a, 0), storage, &p, &n) && n > 0) {
            live(c)->drawBuffers(static_cast<GLsizei>(n),
                                 reinterpret_cast<const GLenum*>(p));
        }
        return ev::undefined();
    });

    b.def("blitFramebuffer", 10, [c](Value, std::span<const Value> a) {
        live(c)->blitFramebuffer(i32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3),
                                 i32At(a, 4), i32At(a, 5), i32At(a, 6), i32At(a, 7),
                                 u32At(a, 8), u32At(a, 9));
        return ev::undefined();
    });

    b.def("invalidateFramebuffer", 2, [c](Value, std::span<const Value> a) {
        live(c);
        if (!glad_glInvalidateFramebuffer) {
            return ev::throwTypeError("WebGL2RenderingContext.invalidateFramebuffer is not supported by the underlying GL driver");
        }
        GLenum target = u32At(a, 0);
        std::vector<uint32_t> storage;
        const uint32_t* p = nullptr;
        size_t n = 0;
        if (uint32Data(argAt(a, 1), storage, &p, &n) && n > 0) {
            glad_glInvalidateFramebuffer(target, static_cast<GLsizei>(n),
                                         reinterpret_cast<const GLenum*>(p));
        }
        return ev::undefined();
    });

    // --- Renderbuffers ---
    b.def("createRenderbuffer", 0, [c](Value, std::span<const Value>) {
        return wrapGlObj(GlCell::Renderbuffer, live(c)->createRenderbuffer().id);
    });
    b.def("deleteRenderbuffer", 1, [c](Value, std::span<const Value> a) {
        live(c)->deleteRenderbuffer({idOf(argAt(a, 0), GlCell::Renderbuffer)});
        return ev::undefined();
    });
    b.def("bindRenderbuffer", 2, [c](Value, std::span<const Value> a) {
        live(c)->bindRenderbuffer(u32At(a, 0), {idOf(argAt(a, 1), GlCell::Renderbuffer)});
        return ev::undefined();
    });
    b.def("isRenderbuffer", 1, [c](Value, std::span<const Value> a) {
        return ev::fromBool(
            live(c)->isRenderbuffer({idOf(argAt(a, 0), GlCell::Renderbuffer)}) != GL_FALSE);
    });
    b.def("renderbufferStorage", 4, [c](Value, std::span<const Value> a) {
        live(c)->renderbufferStorage(u32At(a, 0), u32At(a, 1), i32At(a, 2), i32At(a, 3));
        return ev::undefined();
    });
    b.def("renderbufferStorageMultisample", 5, [c](Value, std::span<const Value> a) {
        live(c)->renderbufferStorageMultisample(u32At(a, 0), i32At(a, 1), u32At(a, 2),
                                                i32At(a, 3), i32At(a, 4));
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
