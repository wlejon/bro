#pragma once

#include "webgl/webgl2_context.h"
#include "webgl/webgl_objects.h"

#include <quickjs.h>
#include <string>
#include <cstring>

namespace bro::js::webgl2 {

// --- Class IDs (allocated in webgl2_bindings.cpp) ---
extern JSClassID js_webgl2_ctx_class_id;
extern JSClassID js_webgl_buffer_class_id;
extern JSClassID js_webgl_texture_class_id;
extern JSClassID js_webgl_program_class_id;
extern JSClassID js_webgl_shader_class_id;
extern JSClassID js_webgl_framebuffer_class_id;
extern JSClassID js_webgl_renderbuffer_class_id;
extern JSClassID js_webgl_vao_class_id;
extern JSClassID js_webgl_uniform_loc_class_id;

// --- Extract C++ context from JS this value ---
inline webgl::WebGL2RenderingContext* getCtx(JSValueConst this_val) {
    return static_cast<webgl::WebGL2RenderingContext*>(
        JS_GetOpaque(this_val, js_webgl2_ctx_class_id));
}

// --- String extraction ---
inline std::string jsStr(JSContext* ctx, JSValueConst val) {
    const char* s = JS_ToCString(ctx, val);
    std::string result = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    return result;
}

// --- WebGL object wrapping/unwrapping ---

// Wrap a WebGLBuffer as a JS object
JSValue wrapBuffer(JSContext* ctx, webgl::WebGLBuffer buf);
JSValue wrapTexture(JSContext* ctx, webgl::WebGLTexture tex);
JSValue wrapProgram(JSContext* ctx, webgl::WebGLProgram prog);
JSValue wrapShader(JSContext* ctx, webgl::WebGLShader shader);
JSValue wrapFramebuffer(JSContext* ctx, webgl::WebGLFramebuffer fbo);
JSValue wrapRenderbuffer(JSContext* ctx, webgl::WebGLRenderbuffer rbo);
JSValue wrapVAO(JSContext* ctx, webgl::WebGLVertexArrayObject vao);
JSValue wrapUniformLocation(JSContext* ctx, webgl::WebGLUniformLocation loc);

// Unwrap WebGL objects from JS values (returns id=0 if null/undefined)
inline webgl::WebGLBuffer unwrapBuffer(JSValueConst val) {
    if (JS_IsNull(val) || JS_IsUndefined(val)) return {0};
    auto* p = static_cast<webgl::WebGLBuffer*>(JS_GetOpaque(val, js_webgl_buffer_class_id));
    return p ? *p : webgl::WebGLBuffer{0};
}

inline webgl::WebGLTexture unwrapTexture(JSValueConst val) {
    if (JS_IsNull(val) || JS_IsUndefined(val)) return {0};
    auto* p = static_cast<webgl::WebGLTexture*>(JS_GetOpaque(val, js_webgl_texture_class_id));
    return p ? *p : webgl::WebGLTexture{0};
}

inline webgl::WebGLProgram unwrapProgram(JSValueConst val) {
    if (JS_IsNull(val) || JS_IsUndefined(val)) return {0};
    auto* p = static_cast<webgl::WebGLProgram*>(JS_GetOpaque(val, js_webgl_program_class_id));
    return p ? *p : webgl::WebGLProgram{0};
}

inline webgl::WebGLShader unwrapShader(JSValueConst val) {
    if (JS_IsNull(val) || JS_IsUndefined(val)) return {0};
    auto* p = static_cast<webgl::WebGLShader*>(JS_GetOpaque(val, js_webgl_shader_class_id));
    return p ? *p : webgl::WebGLShader{0};
}

inline webgl::WebGLFramebuffer unwrapFramebuffer(JSValueConst val) {
    if (JS_IsNull(val) || JS_IsUndefined(val)) return {0};
    auto* p = static_cast<webgl::WebGLFramebuffer*>(JS_GetOpaque(val, js_webgl_framebuffer_class_id));
    return p ? *p : webgl::WebGLFramebuffer{0};
}

inline webgl::WebGLRenderbuffer unwrapRenderbuffer(JSValueConst val) {
    if (JS_IsNull(val) || JS_IsUndefined(val)) return {0};
    auto* p = static_cast<webgl::WebGLRenderbuffer*>(JS_GetOpaque(val, js_webgl_renderbuffer_class_id));
    return p ? *p : webgl::WebGLRenderbuffer{0};
}

inline webgl::WebGLVertexArrayObject unwrapVAO(JSValueConst val) {
    if (JS_IsNull(val) || JS_IsUndefined(val)) return {0};
    auto* p = static_cast<webgl::WebGLVertexArrayObject*>(JS_GetOpaque(val, js_webgl_vao_class_id));
    return p ? *p : webgl::WebGLVertexArrayObject{0};
}

inline webgl::WebGLUniformLocation unwrapUniformLocation(JSValueConst val) {
    if (JS_IsNull(val) || JS_IsUndefined(val)) return {-1, 0};
    auto* p = static_cast<webgl::WebGLUniformLocation*>(JS_GetOpaque(val, js_webgl_uniform_loc_class_id));
    return p ? *p : webgl::WebGLUniformLocation{-1, 0};
}

// --- TypedArray / ArrayBuffer data extraction ---
// Returns pointer and byte length. Returns false if not a valid buffer source.
inline bool getBufferData(JSContext* ctx, JSValueConst val,
                          const uint8_t** outData, size_t* outLen) {
    // Try ArrayBuffer first
    if (JS_IsArrayBuffer(val)) {
        *outData = JS_GetArrayBuffer(ctx, outLen, val);
        return *outData != nullptr;
    }
    // Try TypedArray
    size_t byteOffset = 0, byteLength = 0, bytesPerElement = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, val, &byteOffset, &byteLength, &bytesPerElement);
    if (!JS_IsException(abuf)) {
        size_t abufLen = 0;
        uint8_t* ptr = JS_GetArrayBuffer(ctx, &abufLen, abuf);
        JS_FreeValue(ctx, abuf);
        if (ptr) {
            *outData = ptr + byteOffset;
            *outLen = byteLength;
            return true;
        }
    }
    return false;
}

// Extract float array from TypedArray (Float32Array) or JS array
inline bool getFloatArray(JSContext* ctx, JSValueConst val,
                          const float** outData, size_t* outCount) {
    const uint8_t* data = nullptr;
    size_t len = 0;
    if (getBufferData(ctx, val, &data, &len)) {
        *outData = reinterpret_cast<const float*>(data);
        *outCount = len / sizeof(float);
        return true;
    }
    return false;
}

inline bool getInt32Array(JSContext* ctx, JSValueConst val,
                          const int32_t** outData, size_t* outCount) {
    const uint8_t* data = nullptr;
    size_t len = 0;
    if (getBufferData(ctx, val, &data, &len)) {
        *outData = reinterpret_cast<const int32_t*>(data);
        *outCount = len / sizeof(int32_t);
        return true;
    }
    return false;
}

// --- Function list arrays exported by each category file ---
extern const JSCFunctionListEntry webgl2_state_funcs[];
extern const int webgl2_state_funcs_count;

extern const JSCFunctionListEntry webgl2_buffer_funcs[];
extern const int webgl2_buffer_funcs_count;

extern const JSCFunctionListEntry webgl2_shader_funcs[];
extern const int webgl2_shader_funcs_count;

extern const JSCFunctionListEntry webgl2_texture_funcs[];
extern const int webgl2_texture_funcs_count;

extern const JSCFunctionListEntry webgl2_framebuffer_funcs[];
extern const int webgl2_framebuffer_funcs_count;

extern const JSCFunctionListEntry webgl2_query_funcs[];
extern const int webgl2_query_funcs_count;

} // namespace bro::js::webgl2
