#include "js/webgl2_bindings_util.h"

namespace bro::js::webgl2 {

// ===========================================================================
// Buffers
// ===========================================================================

static JSValue js_createBuffer(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* gl = getCtx(this_val); if (!gl) return JS_NULL;
    return wrapBuffer(ctx, gl->createBuffer());
}

static JSValue js_deleteBuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    gl->deleteBuffer(unwrapBuffer(argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_bindBuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    uint32_t target; JS_ToUint32(ctx, &target, argv[0]);
    gl->bindBuffer(target, unwrapBuffer(argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_bufferData(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t target, usage;
    JS_ToUint32(ctx, &target, argv[0]);
    JS_ToUint32(ctx, &usage, argv[2]);

    // Two signatures: bufferData(target, size, usage) or bufferData(target, data, usage)
    const uint8_t* data = nullptr;
    size_t len = 0;
    if (getBufferData(ctx, argv[1], &data, &len)) {
        gl->bufferData(target, (GLsizeiptr)len, data, usage);
    } else {
        // Treat as size
        int64_t size; JS_ToInt64(ctx, &size, argv[1]);
        gl->bufferData(target, (GLsizeiptr)size, nullptr, usage);
    }
    return JS_UNDEFINED;
}

static JSValue js_bufferSubData(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t target; int64_t offset;
    JS_ToUint32(ctx, &target, argv[0]);
    JS_ToInt64(ctx, &offset, argv[1]);

    const uint8_t* data = nullptr;
    size_t len = 0;
    if (getBufferData(ctx, argv[2], &data, &len)) {
        // Optional srcOffset and length parameters (WebGL2)
        size_t srcOffset = 0;
        if (argc >= 4) { uint32_t so; JS_ToUint32(ctx, &so, argv[3]); srcOffset = so; }
        size_t length = len - srcOffset;
        if (argc >= 5) { uint32_t l; JS_ToUint32(ctx, &l, argv[4]); length = l; }
        gl->bufferSubData(target, (GLintptr)offset, (GLsizeiptr)length, data + srcOffset);
    }
    return JS_UNDEFINED;
}

static JSValue js_copyBufferSubData(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 5) return JS_UNDEFINED;
    uint32_t rt, wt; int64_t ro, wo, sz;
    JS_ToUint32(ctx, &rt, argv[0]); JS_ToUint32(ctx, &wt, argv[1]);
    JS_ToInt64(ctx, &ro, argv[2]); JS_ToInt64(ctx, &wo, argv[3]); JS_ToInt64(ctx, &sz, argv[4]);
    gl->copyBufferSubData(rt, wt, (GLintptr)ro, (GLintptr)wo, (GLsizeiptr)sz);
    return JS_UNDEFINED;
}

static JSValue js_getBufferSubData(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t target; int64_t srcOffset;
    JS_ToUint32(ctx, &target, argv[0]); JS_ToInt64(ctx, &srcOffset, argv[1]);

    const uint8_t* data = nullptr;
    size_t len = 0;
    // argv[2] is the destination TypedArray — we write into it
    if (getBufferData(ctx, argv[2], &data, &len)) {
        gl->getBufferSubData(target, (GLintptr)srcOffset, (void*)data, (GLsizeiptr)len);
    }
    return JS_UNDEFINED;
}

static JSValue js_bindBufferBase(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t target, index;
    JS_ToUint32(ctx, &target, argv[0]); JS_ToUint32(ctx, &index, argv[1]);
    gl->bindBufferBase(target, index, unwrapBuffer(argv[2]));
    return JS_UNDEFINED;
}

static JSValue js_bindBufferRange(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 5) return JS_UNDEFINED;
    uint32_t target, index; int64_t offset, size;
    JS_ToUint32(ctx, &target, argv[0]); JS_ToUint32(ctx, &index, argv[1]);
    JS_ToInt64(ctx, &offset, argv[3]); JS_ToInt64(ctx, &size, argv[4]);
    gl->bindBufferRange(target, index, unwrapBuffer(argv[2]), (GLintptr)offset, (GLsizeiptr)size);
    return JS_UNDEFINED;
}

// ===========================================================================
// VAO
// ===========================================================================

static JSValue js_createVertexArray(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* gl = getCtx(this_val); if (!gl) return JS_NULL;
    return wrapVAO(ctx, gl->createVertexArray());
}

static JSValue js_deleteVertexArray(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    gl->deleteVertexArray(unwrapVAO(argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_bindVertexArray(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    gl->bindVertexArray(unwrapVAO(argv[0]));
    return JS_UNDEFINED;
}

// ===========================================================================
// Vertex attributes
// ===========================================================================

static JSValue js_vertexAttribPointer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 6) return JS_UNDEFINED;
    uint32_t index, type; int size, stride; int64_t offset;
    JS_ToUint32(ctx, &index, argv[0]); JS_ToInt32(ctx, &size, argv[1]);
    JS_ToUint32(ctx, &type, argv[2]);
    bool normalized = JS_ToBool(ctx, argv[3]);
    JS_ToInt32(ctx, &stride, argv[4]); JS_ToInt64(ctx, &offset, argv[5]);
    gl->vertexAttribPointer(index, size, type, normalized ? GL_TRUE : GL_FALSE, stride, (GLintptr)offset);
    return JS_UNDEFINED;
}

static JSValue js_vertexAttribIPointer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 5) return JS_UNDEFINED;
    uint32_t index, type; int size, stride; int64_t offset;
    JS_ToUint32(ctx, &index, argv[0]); JS_ToInt32(ctx, &size, argv[1]);
    JS_ToUint32(ctx, &type, argv[2]);
    JS_ToInt32(ctx, &stride, argv[3]); JS_ToInt64(ctx, &offset, argv[4]);
    gl->vertexAttribIPointer(index, size, type, stride, (GLintptr)offset);
    return JS_UNDEFINED;
}

static JSValue js_enableVertexAttribArray(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t index; JS_ToUint32(ctx, &index, argv[0]);
    gl->enableVertexAttribArray(index);
    return JS_UNDEFINED;
}

static JSValue js_disableVertexAttribArray(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t index; JS_ToUint32(ctx, &index, argv[0]);
    gl->disableVertexAttribArray(index);
    return JS_UNDEFINED;
}

static JSValue js_vertexAttribDivisor(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    uint32_t index, divisor;
    JS_ToUint32(ctx, &index, argv[0]); JS_ToUint32(ctx, &divisor, argv[1]);
    gl->vertexAttribDivisor(index, divisor);
    return JS_UNDEFINED;
}

// ===========================================================================
// Exported function list
// ===========================================================================

const JSCFunctionListEntry webgl2_buffer_funcs[] = {
    JS_CFUNC_DEF("createBuffer", 0, js_createBuffer),
    JS_CFUNC_DEF("deleteBuffer", 1, js_deleteBuffer),
    JS_CFUNC_DEF("bindBuffer", 2, js_bindBuffer),
    JS_CFUNC_DEF("bufferData", 3, js_bufferData),
    JS_CFUNC_DEF("bufferSubData", 3, js_bufferSubData),
    JS_CFUNC_DEF("copyBufferSubData", 5, js_copyBufferSubData),
    JS_CFUNC_DEF("getBufferSubData", 3, js_getBufferSubData),
    JS_CFUNC_DEF("bindBufferBase", 3, js_bindBufferBase),
    JS_CFUNC_DEF("bindBufferRange", 5, js_bindBufferRange),
    JS_CFUNC_DEF("createVertexArray", 0, js_createVertexArray),
    JS_CFUNC_DEF("deleteVertexArray", 1, js_deleteVertexArray),
    JS_CFUNC_DEF("bindVertexArray", 1, js_bindVertexArray),
    JS_CFUNC_DEF("vertexAttribPointer", 6, js_vertexAttribPointer),
    JS_CFUNC_DEF("vertexAttribIPointer", 5, js_vertexAttribIPointer),
    JS_CFUNC_DEF("enableVertexAttribArray", 1, js_enableVertexAttribArray),
    JS_CFUNC_DEF("disableVertexAttribArray", 1, js_disableVertexAttribArray),
    JS_CFUNC_DEF("vertexAttribDivisor", 2, js_vertexAttribDivisor),
};
const int webgl2_buffer_funcs_count = sizeof(webgl2_buffer_funcs) / sizeof(webgl2_buffer_funcs[0]);

} // namespace bro::js::webgl2
