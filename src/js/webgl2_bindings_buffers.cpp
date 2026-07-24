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

    // Signatures: bufferData(target, size, usage), bufferData(target, data, usage),
    // and the WebGL2 form bufferData(target, srcData, usage, srcOffset[, length])
    // where srcOffset/length are in ELEMENT units of the source typed array.
    const uint8_t* data = nullptr;
    size_t len = 0, elemSize = 1;
    if (getBufferDataEx(ctx, argv[1], &data, &len, &elemSize)) {
        size_t elemCount = len / elemSize;
        size_t srcOffset = 0, count = elemCount;
        if (argc >= 4) { uint32_t so; JS_ToUint32(ctx, &so, argv[3]); srcOffset = so; }
        if (srcOffset > elemCount) srcOffset = elemCount;
        count = elemCount - srcOffset;
        if (argc >= 5 && !JS_IsUndefined(argv[4])) {
            uint32_t l; JS_ToUint32(ctx, &l, argv[4]);
            if ((size_t)l < count) count = l;
        }
        gl->bufferData(target, (GLsizeiptr)(count * elemSize),
                       data + srcOffset * elemSize, usage);
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
    size_t len = 0, elemSize = 1;
    if (getBufferDataEx(ctx, argv[2], &data, &len, &elemSize)) {
        // Optional srcOffset and length parameters (WebGL2) — both are in
        // ELEMENT units of the source typed array, not bytes.
        size_t elemCount = len / elemSize;
        size_t srcOffset = 0;
        if (argc >= 4) { uint32_t so; JS_ToUint32(ctx, &so, argv[3]); srcOffset = so; }
        if (srcOffset > elemCount) srcOffset = elemCount;
        size_t count = elemCount - srcOffset;
        if (argc >= 5 && !JS_IsUndefined(argv[4])) {
            uint32_t l; JS_ToUint32(ctx, &l, argv[4]);
            if ((size_t)l < count) count = l;
        }
        gl->bufferSubData(target, (GLintptr)offset, (GLsizeiptr)(count * elemSize),
                          data + srcOffset * elemSize);
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
    size_t len = 0, elemSize = 1;
    // argv[2] is the destination TypedArray — we write into it. Optional
    // dstOffset/length (WebGL2) are in ELEMENT units of the destination.
    if (getBufferDataEx(ctx, argv[2], &data, &len, &elemSize)) {
        size_t elemCount = len / elemSize;
        size_t dstOffset = 0;
        if (argc >= 4) { uint32_t o; JS_ToUint32(ctx, &o, argv[3]); dstOffset = o; }
        if (dstOffset > elemCount) dstOffset = elemCount;
        size_t count = elemCount - dstOffset;
        if (argc >= 5 && !JS_IsUndefined(argv[4])) {
            uint32_t l; JS_ToUint32(ctx, &l, argv[4]);
            if ((size_t)l < count) count = l;
        }
        gl->getBufferSubData(target, (GLintptr)srcOffset,
                             (void*)(data + dstOffset * elemSize),
                             (GLsizeiptr)(count * elemSize));
    }
    return JS_UNDEFINED;
}

// ===========================================================================
// Buffer mapping (BRO_buffer_map)
// ===========================================================================
// mapBufferRange hands back an ArrayBuffer whose storage IS the driver's
// mapped range — no copy in either direction. That makes unmapping a lifetime
// problem: the moment glUnmapBuffer runs, any surviving ArrayBuffer aliases
// memory the driver has taken back. So the ArrayBuffer is detached first and
// unmapped second, and it is parked on the context (in `__mappedBuffers`,
// keyed by GL buffer id) in between. Parking it there is what makes the
// ordering enforceable: the registry is a strong reference, so the buffer
// cannot be collected while mapped, and unmapBuffer always has the exact
// JSValue it needs to detach.
//
// GL 3.3 has no persistent mapping, so this is a per-update handle, not
// something to hold across frames.

// The mapped range is driver memory; the ArrayBuffer only borrows it.
static void freeMappedRange(JSRuntime*, void*, void*) {}

static JSValue mappedRegistry(JSContext* ctx, JSValueConst this_val) {
    JSValue reg = JS_GetPropertyStr(ctx, this_val, "__mappedBuffers");
    if (JS_IsUndefined(reg)) {
        reg = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, this_val, "__mappedBuffers", JS_DupValue(ctx, reg));
    }
    return reg;
}

static JSValue js_mapBufferRange(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_NULL;
    uint32_t target, access; int64_t offset, length;
    JS_ToUint32(ctx, &target, argv[0]);
    JS_ToInt64(ctx, &offset, argv[1]);
    JS_ToInt64(ctx, &length, argv[2]);
    JS_ToUint32(ctx, &access, argv[3]);

    void* ptr = gl->mapBufferRange(target, (GLintptr)offset, (GLsizeiptr)length, access);
    if (!ptr) return JS_NULL; // context already recorded the GL error

    JSValue ab = JS_NewArrayBuffer(ctx, (uint8_t*)ptr, (size_t)length,
                                   freeMappedRange, nullptr, /*is_shared*/ false);
    if (JS_IsException(ab)) { gl->unmapBuffer(target); return ab; } // OOM; keep the exception

    char key[16];
    snprintf(key, sizeof(key), "b%u", gl->boundBuffer(target));
    JSValue reg = mappedRegistry(ctx, this_val);
    JS_SetPropertyStr(ctx, reg, key, JS_DupValue(ctx, ab));
    JS_FreeValue(ctx, reg);
    return ab;
}

static JSValue js_unmapBuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_FALSE;
    uint32_t target; JS_ToUint32(ctx, &target, argv[0]);

    char key[16];
    snprintf(key, sizeof(key), "b%u", gl->boundBuffer(target));
    JSValue reg = mappedRegistry(ctx, this_val);
    JSValue ab = JS_GetPropertyStr(ctx, reg, key);
    if (!JS_IsUndefined(ab)) {
        // Detach before unmapping: after this the ArrayBuffer reads as
        // zero-length rather than aliasing memory the driver has reclaimed.
        JS_DetachArrayBuffer(ctx, ab);
        JS_FreeValue(ctx, ab);
        JS_SetPropertyStr(ctx, reg, key, JS_UNDEFINED);
    }
    JS_FreeValue(ctx, reg);
    return JS_NewBool(ctx, gl->unmapBuffer(target));
}

static JSValue js_flushMappedBufferRange(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t target; int64_t offset, length;
    JS_ToUint32(ctx, &target, argv[0]);
    JS_ToInt64(ctx, &offset, argv[1]);
    JS_ToInt64(ctx, &length, argv[2]);
    gl->flushMappedBufferRange(target, (GLintptr)offset, (GLsizeiptr)length);
    return JS_UNDEFINED;
}

static JSValue js_bindBufferBase(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t target, index;
    JS_ToUint32(ctx, &target, argv[0]); JS_ToUint32(ctx, &index, argv[1]);
    gl->bindBufferBase(target, index, unwrapBuffer(argv[2]));
    stashIndexedBinding(ctx, this_val, target, index, argv[2]);
    return JS_UNDEFINED;
}

static JSValue js_bindBufferRange(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 5) return JS_UNDEFINED;
    uint32_t target, index; int64_t offset, size;
    JS_ToUint32(ctx, &target, argv[0]); JS_ToUint32(ctx, &index, argv[1]);
    JS_ToInt64(ctx, &offset, argv[3]); JS_ToInt64(ctx, &size, argv[4]);
    gl->bindBufferRange(target, index, unwrapBuffer(argv[2]), (GLintptr)offset, (GLsizeiptr)size);
    stashIndexedBinding(ctx, this_val, target, index, argv[2]);
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

// --- Constant integer vertex attributes (WebGL2) ---

static JSValue js_vertexAttribI4i(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 5) return JS_UNDEFINED;
    uint32_t index; int x, y, z, w;
    JS_ToUint32(ctx, &index, argv[0]);
    JS_ToInt32(ctx, &x, argv[1]); JS_ToInt32(ctx, &y, argv[2]);
    JS_ToInt32(ctx, &z, argv[3]); JS_ToInt32(ctx, &w, argv[4]);
    gl->vertexAttribI4i(index, x, y, z, w);
    return JS_UNDEFINED;
}

static JSValue js_vertexAttribI4ui(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 5) return JS_UNDEFINED;
    uint32_t index, x, y, z, w;
    JS_ToUint32(ctx, &index, argv[0]);
    JS_ToUint32(ctx, &x, argv[1]); JS_ToUint32(ctx, &y, argv[2]);
    JS_ToUint32(ctx, &z, argv[3]); JS_ToUint32(ctx, &w, argv[4]);
    gl->vertexAttribI4ui(index, x, y, z, w);
    return JS_UNDEFINED;
}

static JSValue js_vertexAttribI4iv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    uint32_t index; JS_ToUint32(ctx, &index, argv[0]);
    std::vector<int32_t> storage; const int32_t* data = nullptr; size_t count = 0;
    if (getInt32Array(ctx, argv[1], storage, &data, &count) && count >= 4)
        gl->vertexAttribI4iv(index, data);
    return JS_UNDEFINED;
}

static JSValue js_vertexAttribI4uiv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    uint32_t index; JS_ToUint32(ctx, &index, argv[0]);
    std::vector<uint32_t> storage; const uint32_t* data = nullptr; size_t count = 0;
    if (getUint32Array(ctx, argv[1], storage, &data, &count) && count >= 4)
        gl->vertexAttribI4uiv(index, data);
    return JS_UNDEFINED;
}

// --- Object predicates ---

static JSValue js_isBuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_FALSE;
    return JS_NewBool(ctx, gl->isBuffer(unwrapBuffer(argv[0])));
}

static JSValue js_isVertexArray(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_FALSE;
    return JS_NewBool(ctx, gl->isVertexArray(unwrapVAO(argv[0])));
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
    JS_CFUNC_DEF("mapBufferRange", 4, js_mapBufferRange),
    JS_CFUNC_DEF("unmapBuffer", 1, js_unmapBuffer),
    JS_CFUNC_DEF("flushMappedBufferRange", 3, js_flushMappedBufferRange),
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
    JS_CFUNC_DEF("vertexAttribI4i", 5, js_vertexAttribI4i),
    JS_CFUNC_DEF("vertexAttribI4ui", 5, js_vertexAttribI4ui),
    JS_CFUNC_DEF("vertexAttribI4iv", 2, js_vertexAttribI4iv),
    JS_CFUNC_DEF("vertexAttribI4uiv", 2, js_vertexAttribI4uiv),
    JS_CFUNC_DEF("isBuffer", 1, js_isBuffer),
    JS_CFUNC_DEF("isVertexArray", 1, js_isVertexArray),
};
const int webgl2_buffer_funcs_count = sizeof(webgl2_buffer_funcs) / sizeof(webgl2_buffer_funcs[0]);

} // namespace bro::js::webgl2
