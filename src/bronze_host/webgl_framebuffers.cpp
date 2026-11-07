// Framebuffers and renderbuffers for the render-target surface
// three.js drives: create/bind/attach/status, renderbuffer storage (plain and
// multisampled), the resolve blit, MRT draw-buffer selection, and readPixels
// into a caller-supplied typed array.
//
// readPixels is the one place the backend WRITES into the bronze heap: the
// destination pointer comes from embed::typedArrayInfo and is consumed by the
// synchronous readback with no bronze allocation in between — the same one-statement
// lifetime every upload pointer in this layer has, in the other direction.

#include "bronze_host/webgl_internal.h"

namespace bro::bronze_host {

void installWebGLFramebuffers(ObjectBuilder& b) {
    // --- Framebuffers ---
    defMethod(b, "createFramebuffer", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        auto* gl = live(c);
        return gl ? webglObject(c, WebGLCell::Framebuffer, gl->createFramebuffer(c->newObjectId()).id) : ev::null();
    });
    defMethod(b, "deleteFramebuffer", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        const GLuint id = idOf(argAt(a, 0), WebGLCell::Framebuffer);
        if (auto* gl = live(c)) gl->deleteFramebuffer({id});
        forgetWebGLObject(c, WebGLCell::Framebuffer, id);
        return ev::undefined();
    });
    defMethod(b, "bindFramebuffer", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->bindFramebuffer(u32At(a, 0), {idOf(argAt(a, 1), WebGLCell::Framebuffer)});
        return ev::undefined();
    });
    defMethod(b, "isFramebuffer", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* gl = live(c);
        return ev::fromBool(gl && gl->isFramebuffer({idOf(argAt(a, 0), WebGLCell::Framebuffer)}) != GL_FALSE);
    });
    defMethod(b, "framebufferTexture2D", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->framebufferTexture2D(u32At(a, 0), u32At(a, 1), u32At(a, 2),
                                     {idOf(argAt(a, 3), WebGLCell::Texture)}, i32At(a, 4));
        return ev::undefined();
    });
    defMethod(b, "framebufferTextureLayer", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->framebufferTextureLayer(u32At(a, 0), u32At(a, 1), {idOf(argAt(a, 2), WebGLCell::Texture)},
                                        i32At(a, 3), i32At(a, 4));
        return ev::undefined();
    });
    defMethod(b, "framebufferRenderbuffer", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->framebufferRenderbuffer(u32At(a, 0), u32At(a, 1), u32At(a, 2),
                                        {idOf(argAt(a, 3), WebGLCell::Renderbuffer)});
        return ev::undefined();
    });
    defMethod(b, "checkFramebufferStatus", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* gl = live(c);
        return ev::fromDouble(gl ? gl->checkFramebufferStatus(u32At(a, 0)) : 0x8CDD /* FRAMEBUFFER_UNSUPPORTED */);
    });

    // readPixels(x, y, w, h, format, type, dstView) into a typed array —
    // the backend checks the view holds the result before writing — or
    // readPixels(..., offset) into the bound PIXEL_PACK_BUFFER.
    defMethod(b, "readPixels", 7, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* gl = live(c);
        if (!gl) return ev::undefined();
        GLint x = i32At(a, 0), y = i32At(a, 1);
        GLsizei w = i32At(a, 2), h = i32At(a, 3);
        GLenum format = u32At(a, 4), type = u32At(a, 5);
        Value dest = argAt(a, 6);
        if (ev::isNumber(dest)) {
            gl->readPixelsToPBO(x, y, w, h, format, type, static_cast<GLintptr>(i64At(a, 6)));
            return ev::undefined();
        }
        if (auto info = ev::typedArrayInfo(dest))
            gl->readPixels(x, y, w, h, format, type, info.data, info.byteLength);
        else
            gl->setSyntheticError(GL_INVALID_VALUE);
        return ev::undefined();
    });

    defMethod(b, "readBuffer", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->readBuffer(u32At(a, 0));
        return ev::undefined();
    });

    // drawBuffers(sequence<GLenum>). three.js passes a plain JS array here,
    // which uint32Data copies into host storage via embed element reads —
    // the copy is what the backend consumes, so the reads' allocations are
    // harmless.
    defMethod(b, "drawBuffers", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        SmallBuffer<uint32_t, 16> storage;
        const uint32_t* p = nullptr;
        size_t n = 0;
        if (uint32Data(argAt(a, 0), storage, &p, &n) && n > 0) {
            if (auto* gl = live(c))
                gl->drawBuffers(static_cast<GLsizei>(n),
                                reinterpret_cast<const GLenum*>(p));
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
            if (auto* gl = live(c)) {
                if (n < off + need) gl->setSyntheticError(GL_INVALID_VALUE);
                else gl->clearBufferfv(buffer, i32At(a, 1), p + off);
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
            if (auto* gl = live(c)) {
                if (n < off + need) gl->setSyntheticError(GL_INVALID_VALUE);
                else gl->clearBufferiv(buffer, i32At(a, 1), p + off);
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
            if (auto* gl = live(c)) {
                if (n < off + need) gl->setSyntheticError(GL_INVALID_VALUE);
                else gl->clearBufferuiv(buffer, i32At(a, 1), p + off);
            }
        }
        return ev::undefined();
    });
    defMethod(b, "clearBufferfi", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->clearBufferfi(u32At(a, 0), i32At(a, 1),
                              static_cast<GLfloat>(numAt(a, 2)), i32At(a, 3));
        return ev::undefined();
    });

    defMethod(b, "blitFramebuffer", 10, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->blitFramebuffer(i32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3),
                                i32At(a, 4), i32At(a, 5), i32At(a, 6), i32At(a, 7),
                                u32At(a, 8), u32At(a, 9));
        return ev::undefined();
    });

    // invalidate{Sub}Framebuffer(target, attachments[, x, y, width, height]).
    defMethod(b, "invalidateFramebuffer", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        SmallBuffer<uint32_t, 16> storage;
        const uint32_t* p = nullptr;
        size_t n = 0;
        if (!uint32Data(argAt(a, 1), storage, &p, &n)) p = nullptr, n = 0;
        if (auto* gl = live(c)) gl->invalidateFramebuffer(u32At(a, 0), std::span<const GLenum>(p, n));
        return ev::undefined();
    });
    defMethod(b, "invalidateSubFramebuffer", 6, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        SmallBuffer<uint32_t, 16> storage;
        const uint32_t* p = nullptr;
        size_t n = 0;
        if (!uint32Data(argAt(a, 1), storage, &p, &n)) p = nullptr, n = 0;
        if (auto* gl = live(c))
            gl->invalidateSubFramebuffer(u32At(a, 0), std::span<const GLenum>(p, n), i32At(a, 2), i32At(a, 3),
                                         i32At(a, 4), i32At(a, 5));
        return ev::undefined();
    });

    // --- Renderbuffers ---
    defMethod(b, "createRenderbuffer", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        auto* gl = live(c);
        return gl ? webglObject(c, WebGLCell::Renderbuffer, gl->createRenderbuffer(c->newObjectId()).id) : ev::null();
    });
    defMethod(b, "deleteRenderbuffer", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        const GLuint id = idOf(argAt(a, 0), WebGLCell::Renderbuffer);
        if (auto* gl = live(c)) gl->deleteRenderbuffer({id});
        forgetWebGLObject(c, WebGLCell::Renderbuffer, id);
        return ev::undefined();
    });
    defMethod(b, "bindRenderbuffer", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->bindRenderbuffer(u32At(a, 0), {idOf(argAt(a, 1), WebGLCell::Renderbuffer)});
        return ev::undefined();
    });
    defMethod(b, "isRenderbuffer", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* gl = live(c);
        return ev::fromBool(gl && gl->isRenderbuffer({idOf(argAt(a, 0), WebGLCell::Renderbuffer)}) != GL_FALSE);
    });
    defMethod(b, "renderbufferStorage", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->renderbufferStorage(u32At(a, 0), u32At(a, 1), i32At(a, 2), i32At(a, 3));
        return ev::undefined();
    });
    defMethod(b, "renderbufferStorageMultisample", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->renderbufferStorageMultisample(u32At(a, 0), i32At(a, 1), u32At(a, 2),
                                               i32At(a, 3), i32At(a, 4));
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
