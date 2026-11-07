// Framebuffers and renderbuffers for the render-target surface
// three.js drives: create/bind/attach/status, renderbuffer storage (plain and
// multisampled), the resolve blit, MRT draw-buffer selection, and readPixels
// into a caller-supplied typed array.
//
// Every call is encoded by the context's client but checkFramebufferStatus,
// the is* predicates and readPixels into a typed array: readPixels is the one
// place the backend WRITES into the bronze heap, inside a sync() round trip
// with the destination pointer from embed::typedArrayInfo taken just before
// it and no bronze allocation in between — the same one-statement lifetime
// every upload pointer in this layer has, in the other direction.

#include "bronze_host/webgl_internal.h"

namespace bro::bronze_host {

void installWebGLFramebuffers(ObjectBuilder& b) {
    using webgl::WebGLFramebuffer;
    using webgl::WebGLRenderbuffer;
    using webgl::WebGLTexture;
    // --- Framebuffers ---
    defMethod(b, "createFramebuffer", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const GLuint id = c->newObjectId();
        cl->createFramebuffer(id);
        return webglObject(c, WebGLCell::Framebuffer, id);
    });
    defMethod(b, "deleteFramebuffer", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        const GLuint id = idOf(argAt(a, 0), WebGLCell::Framebuffer);
        if (auto* cl = client(c)) cl->deleteFramebuffer(WebGLFramebuffer{id});
        forgetWebGLObject(c, WebGLCell::Framebuffer, id);
        return ev::undefined();
    });
    defMethod(b, "bindFramebuffer", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->bindFramebuffer(u32At(a, 0), WebGLFramebuffer{idOf(argAt(a, 1), WebGLCell::Framebuffer)});
        return ev::undefined();
    });
    defMethod(b, "isFramebuffer", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const WebGLFramebuffer fb{idOf(argAt(a, 0), WebGLCell::Framebuffer)};
        return ev::fromBool(cl && cl->sync([fb](WebGLBackend& gl) { return gl.isFramebuffer(fb); }) != GL_FALSE);
    });
    defMethod(b, "framebufferTexture2D", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->framebufferTexture2D(u32At(a, 0), u32At(a, 1), u32At(a, 2),
                                     WebGLTexture{idOf(argAt(a, 3), WebGLCell::Texture)}, i32At(a, 4));
        return ev::undefined();
    });
    defMethod(b, "framebufferTextureLayer", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->framebufferTextureLayer(u32At(a, 0), u32At(a, 1), WebGLTexture{idOf(argAt(a, 2), WebGLCell::Texture)},
                                        i32At(a, 3), i32At(a, 4));
        return ev::undefined();
    });
    defMethod(b, "framebufferRenderbuffer", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->framebufferRenderbuffer(u32At(a, 0), u32At(a, 1), u32At(a, 2),
                                        WebGLRenderbuffer{idOf(argAt(a, 3), WebGLCell::Renderbuffer)});
        return ev::undefined();
    });
    defMethod(b, "checkFramebufferStatus", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const GLenum target = u32At(a, 0);
        return ev::fromDouble(cl ? cl->sync([target](WebGLBackend& gl) { return gl.checkFramebufferStatus(target); })
                                 : 0x8CDD /* FRAMEBUFFER_UNSUPPORTED */);
    });

    // readPixels(x, y, w, h, format, type, dstView) into a typed array —
    // the backend checks the view holds the result before writing — or
    // readPixels(..., offset) into the bound PIXEL_PACK_BUFFER (encoded: a
    // GPU copy, nothing comes back).
    defMethod(b, "readPixels", 7, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::undefined();
        GLint x = i32At(a, 0), y = i32At(a, 1);
        GLsizei w = i32At(a, 2), h = i32At(a, 3);
        GLenum format = u32At(a, 4), type = u32At(a, 5);
        Value dest = argAt(a, 6);
        if (ev::isNumber(dest)) {
            cl->readPixelsToPBO(x, y, w, h, format, type, static_cast<GLintptr>(i64At(a, 6)));
            return ev::undefined();
        }
        if (auto info = ev::typedArrayInfo(dest)) {
            // The view's pointer is good while this thread waits in sync().
            cl->sync([&](WebGLBackend& gl) { gl.readPixels(x, y, w, h, format, type, info.data, info.byteLength); });
        } else {
            cl->syntheticError(GL_INVALID_VALUE);
        }
        return ev::undefined();
    });

    defMethod(b, "readBuffer", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->readBuffer(u32At(a, 0));
        return ev::undefined();
    });

    // drawBuffers(sequence<GLenum>). three.js passes a plain JS array here,
    // which uint32Data copies into host storage via embed element reads —
    // the copy is what the client encodes, so the reads' allocations are
    // harmless.
    defMethod(b, "drawBuffers", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        SmallBuffer<uint32_t, 16> storage;
        const uint32_t* p = nullptr;
        size_t n = 0;
        if (uint32Data(argAt(a, 0), storage, &p, &n) && n > 0) {
            if (auto* cl = client(c)) cl->drawBuffers(static_cast<GLsizei>(n), reinterpret_cast<const GLenum*>(p), n);
        }
        return ev::undefined();
    });

    // clearBuffer{fv,iv,uiv}(buffer, drawbuffer, values [, srcOffset]) — the
    // spec's length rule: COLOR needs 4 values past srcOffset, DEPTH/STENCIL
    // 1; short is INVALID_VALUE and no call. Same host-copy note as
    // drawBuffers above.
    defMethod(b, "clearBufferfv", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        SmallBuffer<float, 16> storage;
        const float* p = nullptr;
        size_t n = 0;
        const GLenum buffer = u32At(a, 0);
        const size_t off = a.size() > 3 ? static_cast<size_t>(u32At(a, 3)) : 0;
        const size_t need = buffer == GL_COLOR ? 4 : 1;
        if (floatData(argAt(a, 2), storage, &p, &n)) {
            if (auto* cl = client(c)) {
                if (n < off + need) cl->syntheticError(GL_INVALID_VALUE);
                else cl->clearBufferfv(buffer, i32At(a, 1), p + off, need);
            }
        }
        return ev::undefined();
    });
    defMethod(b, "clearBufferiv", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        SmallBuffer<int32_t, 16> storage;
        const int32_t* p = nullptr;
        size_t n = 0;
        const GLenum buffer = u32At(a, 0);
        const size_t off = a.size() > 3 ? static_cast<size_t>(u32At(a, 3)) : 0;
        const size_t need = buffer == GL_COLOR ? 4 : 1;
        if (int32Data(argAt(a, 2), storage, &p, &n)) {
            if (auto* cl = client(c)) {
                if (n < off + need) cl->syntheticError(GL_INVALID_VALUE);
                else cl->clearBufferiv(buffer, i32At(a, 1), p + off, need);
            }
        }
        return ev::undefined();
    });
    defMethod(b, "clearBufferuiv", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        SmallBuffer<uint32_t, 16> storage;
        const uint32_t* p = nullptr;
        size_t n = 0;
        const GLenum buffer = u32At(a, 0);
        const size_t off = a.size() > 3 ? static_cast<size_t>(u32At(a, 3)) : 0;
        const size_t need = buffer == GL_COLOR ? 4 : 1;
        if (uint32Data(argAt(a, 2), storage, &p, &n)) {
            if (auto* cl = client(c)) {
                if (n < off + need) cl->syntheticError(GL_INVALID_VALUE);
                else cl->clearBufferuiv(buffer, i32At(a, 1), p + off, need);
            }
        }
        return ev::undefined();
    });
    defMethod(b, "clearBufferfi", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->clearBufferfi(u32At(a, 0), i32At(a, 1), static_cast<GLfloat>(numAt(a, 2)), i32At(a, 3));
        return ev::undefined();
    });

    defMethod(b, "blitFramebuffer", 10, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->blitFramebuffer(i32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3), i32At(a, 4), i32At(a, 5),
                                i32At(a, 6), i32At(a, 7), u32At(a, 8), u32At(a, 9));
        return ev::undefined();
    });

    // invalidate{Sub}Framebuffer(target, attachments[, x, y, width, height]).
    defMethod(b, "invalidateFramebuffer", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        SmallBuffer<uint32_t, 16> storage;
        const uint32_t* p = nullptr;
        size_t n = 0;
        if (!uint32Data(argAt(a, 1), storage, &p, &n)) p = nullptr, n = 0;
        if (auto* cl = client(c)) cl->invalidateFramebuffer(u32At(a, 0), reinterpret_cast<const GLenum*>(p), n);
        return ev::undefined();
    });
    defMethod(b, "invalidateSubFramebuffer", 6, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        SmallBuffer<uint32_t, 16> storage;
        const uint32_t* p = nullptr;
        size_t n = 0;
        if (!uint32Data(argAt(a, 1), storage, &p, &n)) p = nullptr, n = 0;
        if (auto* cl = client(c))
            cl->invalidateSubFramebuffer(u32At(a, 0), reinterpret_cast<const GLenum*>(p), n, i32At(a, 2), i32At(a, 3),
                                         i32At(a, 4), i32At(a, 5));
        return ev::undefined();
    });

    // --- Renderbuffers ---
    defMethod(b, "createRenderbuffer", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const GLuint id = c->newObjectId();
        cl->createRenderbuffer(id);
        return webglObject(c, WebGLCell::Renderbuffer, id);
    });
    defMethod(b, "deleteRenderbuffer", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        const GLuint id = idOf(argAt(a, 0), WebGLCell::Renderbuffer);
        if (auto* cl = client(c)) cl->deleteRenderbuffer(WebGLRenderbuffer{id});
        forgetWebGLObject(c, WebGLCell::Renderbuffer, id);
        return ev::undefined();
    });
    defMethod(b, "bindRenderbuffer", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->bindRenderbuffer(u32At(a, 0), WebGLRenderbuffer{idOf(argAt(a, 1), WebGLCell::Renderbuffer)});
        return ev::undefined();
    });
    defMethod(b, "isRenderbuffer", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const WebGLRenderbuffer rb{idOf(argAt(a, 0), WebGLCell::Renderbuffer)};
        return ev::fromBool(cl && cl->sync([rb](WebGLBackend& gl) { return gl.isRenderbuffer(rb); }) != GL_FALSE);
    });
    defMethod(b, "renderbufferStorage", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->renderbufferStorage(u32At(a, 0), u32At(a, 1), i32At(a, 2), i32At(a, 3));
        return ev::undefined();
    });
    defMethod(b, "renderbufferStorageMultisample", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->renderbufferStorageMultisample(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3), i32At(a, 4));
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
