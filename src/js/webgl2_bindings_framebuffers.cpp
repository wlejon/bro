#include "js/webgl2_bindings_util.h"

namespace bro::js::webgl2 {

// ===========================================================================
// Framebuffers
// ===========================================================================

static JSValue js_createFramebuffer(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* gl = getCtx(this_val); if (!gl) return JS_NULL;
    return wrapFramebuffer(ctx, gl->createFramebuffer());
}

static JSValue js_deleteFramebuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    gl->deleteFramebuffer(unwrapFramebuffer(argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_bindFramebuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    uint32_t target; JS_ToUint32(ctx, &target, argv[0]);
    // null means bind the default canvas FBO
    auto fbo = unwrapFramebuffer(argv[1]);
    gl->bindFramebuffer(target, fbo);
    return JS_UNDEFINED;
}

static JSValue js_framebufferTexture2D(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 5) return JS_UNDEFINED;
    uint32_t target, attachment, textarget; int level;
    JS_ToUint32(ctx, &target, argv[0]);
    JS_ToUint32(ctx, &attachment, argv[1]);
    JS_ToUint32(ctx, &textarget, argv[2]);
    JS_ToInt32(ctx, &level, argv[4]);
    gl->framebufferTexture2D(target, attachment, textarget, unwrapTexture(argv[3]), level);
    return JS_UNDEFINED;
}

static JSValue js_framebufferRenderbuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_UNDEFINED;
    uint32_t target, attachment, rbtarget;
    JS_ToUint32(ctx, &target, argv[0]);
    JS_ToUint32(ctx, &attachment, argv[1]);
    JS_ToUint32(ctx, &rbtarget, argv[2]);
    gl->framebufferRenderbuffer(target, attachment, rbtarget, unwrapRenderbuffer(argv[3]));
    return JS_UNDEFINED;
}

static JSValue js_checkFramebufferStatus(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t target; JS_ToUint32(ctx, &target, argv[0]);
    return JS_NewUint32(ctx, gl->checkFramebufferStatus(target));
}

static JSValue js_readPixels(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 7) return JS_UNDEFINED;
    int x, y, w, h;
    uint32_t format, type;
    JS_ToInt32(ctx, &x, argv[0]); JS_ToInt32(ctx, &y, argv[1]);
    JS_ToInt32(ctx, &w, argv[2]); JS_ToInt32(ctx, &h, argv[3]);
    JS_ToUint32(ctx, &format, argv[4]); JS_ToUint32(ctx, &type, argv[5]);

    // WebGL2 offset overload: readPixels(..., GLintptr offset) into the
    // bound PIXEL_PACK_BUFFER (bounds-checked against the PBO size).
    if (JS_IsNumber(argv[6])) {
        int64_t offset; JS_ToInt64(ctx, &offset, argv[6]);
        gl->readPixelsToPBO(x, y, w, h, format, type, (GLintptr)offset);
        return JS_UNDEFINED;
    }

    // argv[6] is a TypedArray — get writable pointer
    const uint8_t* data = nullptr;
    size_t len = 0;
    if (getBufferData(ctx, argv[6], &data, &len)) {
        // WebGL2: the client-memory overload is INVALID_OPERATION while a
        // PIXEL_PACK buffer is bound (raw GL would misread the pointer as a
        // PBO offset and scribble into the app's buffer).
        if (gl->pixelPackBuffer()) {
            gl->setSyntheticError(0x0502 /* GL_INVALID_OPERATION */);
            return JS_UNDEFINED;
        }
        // WebGL: a destination too small for the result is INVALID_OPERATION,
        // never an out-of-bounds write.
        if (gl->validateReadPixels(w, h, format, type, len)) {
            gl->readPixels(x, y, w, h, format, type, (void*)data);
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_readBuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t src; JS_ToUint32(ctx, &src, argv[0]);
    gl->readBuffer(src);
    return JS_UNDEFINED;
}

static JSValue js_drawBuffers(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;

    // argv[0] is a JS array of GLenum values
    JSValue lenVal = JS_GetPropertyStr(ctx, argv[0], "length");
    uint32_t count = 0;
    JS_ToUint32(ctx, &count, lenVal);
    JS_FreeValue(ctx, lenVal);

    std::vector<GLenum> bufs(count);
    for (uint32_t i = 0; i < count; i++) {
        JSValue elem = JS_GetPropertyUint32(ctx, argv[0], i);
        uint32_t v; JS_ToUint32(ctx, &v, elem);
        bufs[i] = v;
        JS_FreeValue(ctx, elem);
    }
    gl->drawBuffers((GLsizei)count, bufs.data());
    return JS_UNDEFINED;
}

static JSValue js_blitFramebuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 10) return JS_UNDEFINED;
    int sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1;
    uint32_t mask, filter;
    JS_ToInt32(ctx, &sx0, argv[0]); JS_ToInt32(ctx, &sy0, argv[1]);
    JS_ToInt32(ctx, &sx1, argv[2]); JS_ToInt32(ctx, &sy1, argv[3]);
    JS_ToInt32(ctx, &dx0, argv[4]); JS_ToInt32(ctx, &dy0, argv[5]);
    JS_ToInt32(ctx, &dx1, argv[6]); JS_ToInt32(ctx, &dy1, argv[7]);
    JS_ToUint32(ctx, &mask, argv[8]); JS_ToUint32(ctx, &filter, argv[9]);
    gl->blitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
    return JS_UNDEFINED;
}

// clearBuffer{fv,iv,uiv}(buffer, drawbuffer, values [, srcOffset]) — the
// spec's length rule: COLOR needs 4 values past srcOffset, DEPTH/STENCIL 1;
// short is INVALID_VALUE and no call.
static bool clearBufferCount(webgl::WebGL2RenderingContext* gl, uint32_t buffer,
                             size_t count, uint32_t srcOffset) {
    const size_t need = (buffer == GL_COLOR) ? 4 : 1;
    if (count < srcOffset + need) {
        gl->setSyntheticError(GL_INVALID_VALUE);
        return false;
    }
    return true;
}

static JSValue js_clearBufferfv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t buffer, srcOffset = 0; int drawbuffer;
    JS_ToUint32(ctx, &buffer, argv[0]);
    JS_ToInt32(ctx, &drawbuffer, argv[1]);
    if (argc > 3) JS_ToUint32(ctx, &srcOffset, argv[3]);
    std::vector<float> storage; const float* data = nullptr; size_t count = 0;
    if (getFloatArray(ctx, argv[2], storage, &data, &count) &&
        clearBufferCount(gl, buffer, count, srcOffset))
        gl->clearBufferfv(buffer, drawbuffer, data + srcOffset);
    return JS_UNDEFINED;
}

static JSValue js_clearBufferiv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t buffer, srcOffset = 0; int drawbuffer;
    JS_ToUint32(ctx, &buffer, argv[0]);
    JS_ToInt32(ctx, &drawbuffer, argv[1]);
    if (argc > 3) JS_ToUint32(ctx, &srcOffset, argv[3]);
    std::vector<int32_t> storage; const int32_t* data = nullptr; size_t count = 0;
    if (getInt32Array(ctx, argv[2], storage, &data, &count) &&
        clearBufferCount(gl, buffer, count, srcOffset))
        gl->clearBufferiv(buffer, drawbuffer, data + srcOffset);
    return JS_UNDEFINED;
}

static JSValue js_clearBufferuiv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t buffer, srcOffset = 0; int drawbuffer;
    JS_ToUint32(ctx, &buffer, argv[0]);
    JS_ToInt32(ctx, &drawbuffer, argv[1]);
    if (argc > 3) JS_ToUint32(ctx, &srcOffset, argv[3]);
    std::vector<uint32_t> storage; const uint32_t* data = nullptr; size_t count = 0;
    if (getUint32Array(ctx, argv[2], storage, &data, &count) &&
        clearBufferCount(gl, buffer, count, srcOffset))
        gl->clearBufferuiv(buffer, drawbuffer, data + srcOffset);
    return JS_UNDEFINED;
}

static JSValue js_clearBufferfi(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_UNDEFINED;
    uint32_t buffer; int drawbuffer, stencil; double depth;
    JS_ToUint32(ctx, &buffer, argv[0]);
    JS_ToInt32(ctx, &drawbuffer, argv[1]);
    JS_ToFloat64(ctx, &depth, argv[2]);
    JS_ToInt32(ctx, &stencil, argv[3]);
    gl->clearBufferfi(buffer, drawbuffer, (GLfloat)depth, stencil);
    return JS_UNDEFINED;
}

// ===========================================================================
// Renderbuffers
// ===========================================================================

static JSValue js_createRenderbuffer(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* gl = getCtx(this_val); if (!gl) return JS_NULL;
    return wrapRenderbuffer(ctx, gl->createRenderbuffer());
}

static JSValue js_deleteRenderbuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    gl->deleteRenderbuffer(unwrapRenderbuffer(argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_bindRenderbuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    uint32_t target; JS_ToUint32(ctx, &target, argv[0]);
    gl->bindRenderbuffer(target, unwrapRenderbuffer(argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_renderbufferStorage(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_UNDEFINED;
    uint32_t target, internalformat; int w, h;
    JS_ToUint32(ctx, &target, argv[0]);
    JS_ToUint32(ctx, &internalformat, argv[1]);
    JS_ToInt32(ctx, &w, argv[2]); JS_ToInt32(ctx, &h, argv[3]);
    gl->renderbufferStorage(target, internalformat, w, h);
    return JS_UNDEFINED;
}

static JSValue js_renderbufferStorageMultisample(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 5) return JS_UNDEFINED;
    uint32_t target, internalformat; int samples, w, h;
    JS_ToUint32(ctx, &target, argv[0]);
    JS_ToInt32(ctx, &samples, argv[1]);
    JS_ToUint32(ctx, &internalformat, argv[2]);
    JS_ToInt32(ctx, &w, argv[3]); JS_ToInt32(ctx, &h, argv[4]);
    gl->renderbufferStorageMultisample(target, samples, internalformat, w, h);
    return JS_UNDEFINED;
}

static JSValue js_isFramebuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_FALSE;
    return JS_NewBool(ctx, gl->isFramebuffer(unwrapFramebuffer(argv[0])));
}

static JSValue js_isRenderbuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_FALSE;
    return JS_NewBool(ctx, gl->isRenderbuffer(unwrapRenderbuffer(argv[0])));
}

// ===========================================================================
// Exported function list
// ===========================================================================

const JSCFunctionListEntry webgl2_framebuffer_funcs[] = {
    JS_CFUNC_DEF("createFramebuffer", 0, js_createFramebuffer),
    JS_CFUNC_DEF("deleteFramebuffer", 1, js_deleteFramebuffer),
    JS_CFUNC_DEF("bindFramebuffer", 2, js_bindFramebuffer),
    JS_CFUNC_DEF("framebufferTexture2D", 5, js_framebufferTexture2D),
    JS_CFUNC_DEF("framebufferRenderbuffer", 4, js_framebufferRenderbuffer),
    JS_CFUNC_DEF("checkFramebufferStatus", 1, js_checkFramebufferStatus),
    JS_CFUNC_DEF("readPixels", 7, js_readPixels),
    JS_CFUNC_DEF("readBuffer", 1, js_readBuffer),
    JS_CFUNC_DEF("drawBuffers", 1, js_drawBuffers),
    JS_CFUNC_DEF("blitFramebuffer", 10, js_blitFramebuffer),
    JS_CFUNC_DEF("clearBufferfv", 3, js_clearBufferfv),
    JS_CFUNC_DEF("clearBufferiv", 3, js_clearBufferiv),
    JS_CFUNC_DEF("clearBufferuiv", 3, js_clearBufferuiv),
    JS_CFUNC_DEF("clearBufferfi", 4, js_clearBufferfi),
    JS_CFUNC_DEF("createRenderbuffer", 0, js_createRenderbuffer),
    JS_CFUNC_DEF("deleteRenderbuffer", 1, js_deleteRenderbuffer),
    JS_CFUNC_DEF("bindRenderbuffer", 2, js_bindRenderbuffer),
    JS_CFUNC_DEF("renderbufferStorage", 4, js_renderbufferStorage),
    JS_CFUNC_DEF("renderbufferStorageMultisample", 5, js_renderbufferStorageMultisample),
    JS_CFUNC_DEF("isFramebuffer", 1, js_isFramebuffer),
    JS_CFUNC_DEF("isRenderbuffer", 1, js_isRenderbuffer),
};
const int webgl2_framebuffer_funcs_count = sizeof(webgl2_framebuffer_funcs) / sizeof(webgl2_framebuffer_funcs[0]);

} // namespace bro::js::webgl2
