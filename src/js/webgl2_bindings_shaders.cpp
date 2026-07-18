#include "js/webgl2_bindings_util.h"

namespace bro::js::webgl2 {

// ===========================================================================
// Shaders
// ===========================================================================

static JSValue js_createShader(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_NULL;
    uint32_t type; JS_ToUint32(ctx, &type, argv[0]);
    return wrapShader(ctx, gl->createShader(type));
}

static JSValue js_deleteShader(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    gl->deleteShader(unwrapShader(argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_shaderSource(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    gl->shaderSource(unwrapShader(argv[0]), jsStr(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_compileShader(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    gl->compileShader(unwrapShader(argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_getShaderParameter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    auto shader = unwrapShader(argv[0]);
    uint32_t pname; JS_ToUint32(ctx, &pname, argv[1]);
    switch (pname) {
        case 0x8B81: // GL_COMPILE_STATUS
            return JS_NewBool(ctx, gl->getShaderParameter_compileStatus(shader));
        case 0x8B80: { // GL_DELETE_STATUS
            GLint val; glGetShaderiv(shader.id, pname, &val);
            return JS_NewBool(ctx, val);
        }
        case 0x8B4F: { // GL_SHADER_TYPE
            GLint val; glGetShaderiv(shader.id, pname, &val);
            return JS_NewUint32(ctx, (uint32_t)val);
        }
        default: return JS_UNDEFINED;
    }
}

static JSValue js_getShaderInfoLog(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    std::string log = gl->getShaderInfoLog(unwrapShader(argv[0]));
    return JS_NewString(ctx, log.c_str());
}

// ===========================================================================
// Programs
// ===========================================================================

static JSValue js_createProgram(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* gl = getCtx(this_val); if (!gl) return JS_NULL;
    return wrapProgram(ctx, gl->createProgram());
}

static JSValue js_deleteProgram(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    gl->deleteProgram(unwrapProgram(argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_attachShader(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    gl->attachShader(unwrapProgram(argv[0]), unwrapShader(argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_detachShader(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    gl->detachShader(unwrapProgram(argv[0]), unwrapShader(argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_linkProgram(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    gl->linkProgram(unwrapProgram(argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_useProgram(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    auto prog = unwrapProgram(argv[0]); // returns {0} for null/undefined
    gl->useProgram(prog);
    return JS_UNDEFINED;
}

static JSValue js_getProgramParameter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    auto prog = unwrapProgram(argv[0]);
    uint32_t pname; JS_ToUint32(ctx, &pname, argv[1]);
    switch (pname) {
        case 0x8B82: // GL_LINK_STATUS
            return JS_NewBool(ctx, gl->getProgramParameter_linkStatus(prog));
        case 0x8B80: { // GL_DELETE_STATUS
            GLint val; glGetProgramiv(prog.id, pname, &val);
            return JS_NewBool(ctx, val);
        }
        case 0x8B89: // GL_ACTIVE_ATTRIBUTES
        case 0x8B86: // GL_ACTIVE_UNIFORMS
        case 0x8A36: // GL_ACTIVE_UNIFORM_BLOCKS
        case 0x8B85: // GL_ATTACHED_SHADERS
        case 0x8C83: // GL_TRANSFORM_FEEDBACK_VARYINGS
        case 0x8C7F: // GL_TRANSFORM_FEEDBACK_BUFFER_MODE
            return JS_NewInt32(ctx, gl->getProgramParameter_int(prog, pname));
        default: return JS_UNDEFINED;
    }
}

static JSValue js_getProgramInfoLog(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    std::string log = gl->getProgramInfoLog(unwrapProgram(argv[0]));
    return JS_NewString(ctx, log.c_str());
}

static JSValue js_bindAttribLocation(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t index; JS_ToUint32(ctx, &index, argv[1]);
    gl->bindAttribLocation(unwrapProgram(argv[0]), index, jsStr(ctx, argv[2]));
    return JS_UNDEFINED;
}

static JSValue js_getAttribLocation(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    GLint loc = gl->getAttribLocation(unwrapProgram(argv[0]), jsStr(ctx, argv[1]));
    return JS_NewInt32(ctx, loc);
}

static JSValue js_getFragDataLocation(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    GLint loc = gl->getFragDataLocation(unwrapProgram(argv[0]), jsStr(ctx, argv[1]));
    return JS_NewInt32(ctx, loc);
}

static JSValue js_getUniformLocation(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_NULL;
    auto loc = gl->getUniformLocation(unwrapProgram(argv[0]), jsStr(ctx, argv[1]));
    if (loc.location == -1) return JS_NULL;
    return wrapUniformLocation(ctx, loc);
}

static JSValue js_getActiveAttrib(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_NULL;
    uint32_t index; JS_ToUint32(ctx, &index, argv[1]);
    auto info = gl->getActiveAttrib(unwrapProgram(argv[0]), index);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, info.name.c_str()));
    JS_SetPropertyStr(ctx, obj, "type", JS_NewUint32(ctx, info.type));
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt32(ctx, info.size));
    return obj;
}

static JSValue js_getActiveUniform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_NULL;
    uint32_t index; JS_ToUint32(ctx, &index, argv[1]);
    auto info = gl->getActiveUniform(unwrapProgram(argv[0]), index);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, info.name.c_str()));
    JS_SetPropertyStr(ctx, obj, "type", JS_NewUint32(ctx, info.type));
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt32(ctx, info.size));
    return obj;
}

static JSValue js_getUniformBlockIndex(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    GLuint idx = gl->getUniformBlockIndex(unwrapProgram(argv[0]), jsStr(ctx, argv[1]));
    return JS_NewUint32(ctx, idx);
}

static JSValue js_uniformBlockBinding(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t blockIndex, blockBinding;
    JS_ToUint32(ctx, &blockIndex, argv[1]); JS_ToUint32(ctx, &blockBinding, argv[2]);
    gl->uniformBlockBinding(unwrapProgram(argv[0]), blockIndex, blockBinding);
    return JS_UNDEFINED;
}

// ===========================================================================
// Uniforms — scalar
// ===========================================================================

static JSValue js_uniform1f(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    double x; JS_ToFloat64(ctx, &x, argv[1]);
    gl->uniform1f(unwrapUniformLocation(argv[0]), (float)x);
    return JS_UNDEFINED;
}

static JSValue js_uniform2f(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    double x, y; JS_ToFloat64(ctx, &x, argv[1]); JS_ToFloat64(ctx, &y, argv[2]);
    gl->uniform2f(unwrapUniformLocation(argv[0]), (float)x, (float)y);
    return JS_UNDEFINED;
}

static JSValue js_uniform3f(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_UNDEFINED;
    double x, y, z;
    JS_ToFloat64(ctx, &x, argv[1]); JS_ToFloat64(ctx, &y, argv[2]); JS_ToFloat64(ctx, &z, argv[3]);
    gl->uniform3f(unwrapUniformLocation(argv[0]), (float)x, (float)y, (float)z);
    return JS_UNDEFINED;
}

static JSValue js_uniform4f(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 5) return JS_UNDEFINED;
    double x, y, z, w;
    JS_ToFloat64(ctx, &x, argv[1]); JS_ToFloat64(ctx, &y, argv[2]);
    JS_ToFloat64(ctx, &z, argv[3]); JS_ToFloat64(ctx, &w, argv[4]);
    gl->uniform4f(unwrapUniformLocation(argv[0]), (float)x, (float)y, (float)z, (float)w);
    return JS_UNDEFINED;
}

static JSValue js_uniform1i(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    int x; JS_ToInt32(ctx, &x, argv[1]);
    gl->uniform1i(unwrapUniformLocation(argv[0]), x);
    return JS_UNDEFINED;
}

static JSValue js_uniform2i(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    int x, y; JS_ToInt32(ctx, &x, argv[1]); JS_ToInt32(ctx, &y, argv[2]);
    gl->uniform2i(unwrapUniformLocation(argv[0]), x, y);
    return JS_UNDEFINED;
}

static JSValue js_uniform3i(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_UNDEFINED;
    int x, y, z;
    JS_ToInt32(ctx, &x, argv[1]); JS_ToInt32(ctx, &y, argv[2]); JS_ToInt32(ctx, &z, argv[3]);
    gl->uniform3i(unwrapUniformLocation(argv[0]), x, y, z);
    return JS_UNDEFINED;
}

static JSValue js_uniform4i(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 5) return JS_UNDEFINED;
    int x, y, z, w;
    JS_ToInt32(ctx, &x, argv[1]); JS_ToInt32(ctx, &y, argv[2]);
    JS_ToInt32(ctx, &z, argv[3]); JS_ToInt32(ctx, &w, argv[4]);
    gl->uniform4i(unwrapUniformLocation(argv[0]), x, y, z, w);
    return JS_UNDEFINED;
}

// ===========================================================================
// Uniforms — scalar unsigned int (WebGL2)
// ===========================================================================

static JSValue js_uniform1ui(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    uint32_t x; JS_ToUint32(ctx, &x, argv[1]);
    gl->uniform1ui(unwrapUniformLocation(argv[0]), x);
    return JS_UNDEFINED;
}

static JSValue js_uniform2ui(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t x, y; JS_ToUint32(ctx, &x, argv[1]); JS_ToUint32(ctx, &y, argv[2]);
    gl->uniform2ui(unwrapUniformLocation(argv[0]), x, y);
    return JS_UNDEFINED;
}

static JSValue js_uniform3ui(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_UNDEFINED;
    uint32_t x, y, z;
    JS_ToUint32(ctx, &x, argv[1]); JS_ToUint32(ctx, &y, argv[2]); JS_ToUint32(ctx, &z, argv[3]);
    gl->uniform3ui(unwrapUniformLocation(argv[0]), x, y, z);
    return JS_UNDEFINED;
}

static JSValue js_uniform4ui(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 5) return JS_UNDEFINED;
    uint32_t x, y, z, w;
    JS_ToUint32(ctx, &x, argv[1]); JS_ToUint32(ctx, &y, argv[2]);
    JS_ToUint32(ctx, &z, argv[3]); JS_ToUint32(ctx, &w, argv[4]);
    gl->uniform4ui(unwrapUniformLocation(argv[0]), x, y, z, w);
    return JS_UNDEFINED;
}

// ===========================================================================
// Uniforms — unsigned int vector (WebGL2)
// ===========================================================================

static JSValue js_uniform1uiv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    std::vector<uint32_t> storage; const uint32_t* data = nullptr; size_t count = 0;
    if (getUint32Array(ctx, argv[1], storage, &data, &count))
        gl->uniform1uiv(unwrapUniformLocation(argv[0]), (GLsizei)(count / 1), data);
    return JS_UNDEFINED;
}

static JSValue js_uniform2uiv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    std::vector<uint32_t> storage; const uint32_t* data = nullptr; size_t count = 0;
    if (getUint32Array(ctx, argv[1], storage, &data, &count))
        gl->uniform2uiv(unwrapUniformLocation(argv[0]), (GLsizei)(count / 2), data);
    return JS_UNDEFINED;
}

static JSValue js_uniform3uiv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    std::vector<uint32_t> storage; const uint32_t* data = nullptr; size_t count = 0;
    if (getUint32Array(ctx, argv[1], storage, &data, &count))
        gl->uniform3uiv(unwrapUniformLocation(argv[0]), (GLsizei)(count / 3), data);
    return JS_UNDEFINED;
}

static JSValue js_uniform4uiv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    std::vector<uint32_t> storage; const uint32_t* data = nullptr; size_t count = 0;
    if (getUint32Array(ctx, argv[1], storage, &data, &count))
        gl->uniform4uiv(unwrapUniformLocation(argv[0]), (GLsizei)(count / 4), data);
    return JS_UNDEFINED;
}

// ===========================================================================
// Uniform / block introspection (WebGL2)
// ===========================================================================

static JSValue js_getUniformIndices(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_NULL;
    std::vector<std::string> names;
    JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, lenVal);
    JS_FreeValue(ctx, lenVal);
    names.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        JSValue elem = JS_GetPropertyUint32(ctx, argv[1], i);
        names.push_back(jsStr(ctx, elem));
        JS_FreeValue(ctx, elem);
    }
    auto indices = gl->getUniformIndices(unwrapProgram(argv[0]), names);
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < indices.size(); i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewUint32(ctx, indices[i]));
    return arr;
}

static JSValue js_getActiveUniforms(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_NULL;
    uint32_t pname; JS_ToUint32(ctx, &pname, argv[2]);
    std::vector<uint32_t> storage; const uint32_t* data = nullptr; size_t count = 0;
    if (!getUint32Array(ctx, argv[1], storage, &data, &count)) return JS_NULL;
    auto params = gl->getActiveUniforms(unwrapProgram(argv[0]),
                                        std::vector<GLuint>(data, data + count), pname);
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < params.size(); i++) {
        // UNIFORM_IS_ROW_MAJOR answers as booleans per the WebGL2 spec.
        JSValue v = (pname == 0x8A3E) ? JS_NewBool(ctx, params[i] != 0)
                                      : JS_NewInt32(ctx, params[i]);
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, v);
    }
    return arr;
}

static JSValue js_getActiveUniformBlockParameter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_NULL;
    uint32_t blockIndex, pname;
    JS_ToUint32(ctx, &blockIndex, argv[1]); JS_ToUint32(ctx, &pname, argv[2]);
    auto prog = unwrapProgram(argv[0]);
    switch (pname) {
        case 0x8A43: { // UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES
            auto indices = gl->getActiveUniformBlockIndices(prog, blockIndex);
            JSValue arr = JS_NewArray(ctx);
            for (size_t i = 0; i < indices.size(); i++)
                JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewUint32(ctx, (uint32_t)indices[i]));
            return arr;
        }
        case 0x8A44: // UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER
        case 0x8A46: // UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER
            return JS_NewBool(ctx, gl->getActiveUniformBlockParameteri(prog, blockIndex, pname) != 0);
        default:     // BINDING / DATA_SIZE / ACTIVE_UNIFORMS
            return JS_NewInt32(ctx, gl->getActiveUniformBlockParameteri(prog, blockIndex, pname));
    }
}

static JSValue js_getActiveUniformBlockName(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_NULL;
    uint32_t blockIndex; JS_ToUint32(ctx, &blockIndex, argv[1]);
    std::string name = gl->getActiveUniformBlockName(unwrapProgram(argv[0]), blockIndex);
    if (name.empty()) return JS_NULL;
    return JS_NewString(ctx, name.c_str());
}

static JSValue js_isProgram(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_FALSE;
    return JS_NewBool(ctx, gl->isProgram(unwrapProgram(argv[0])));
}

static JSValue js_isShader(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_FALSE;
    return JS_NewBool(ctx, gl->isShader(unwrapShader(argv[0])));
}

// ===========================================================================
// Uniforms — float vector
// ===========================================================================

static JSValue js_uniform1fv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    std::vector<float> storage; const float* data = nullptr; size_t count = 0;
    if (getFloatArray(ctx, argv[1], storage, &data, &count))
        gl->uniform1fv(unwrapUniformLocation(argv[0]), (GLsizei)(count / 1), data);
    return JS_UNDEFINED;
}

static JSValue js_uniform2fv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    std::vector<float> storage; const float* data = nullptr; size_t count = 0;
    if (getFloatArray(ctx, argv[1], storage, &data, &count))
        gl->uniform2fv(unwrapUniformLocation(argv[0]), (GLsizei)(count / 2), data);
    return JS_UNDEFINED;
}

static JSValue js_uniform3fv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    std::vector<float> storage; const float* data = nullptr; size_t count = 0;
    if (getFloatArray(ctx, argv[1], storage, &data, &count))
        gl->uniform3fv(unwrapUniformLocation(argv[0]), (GLsizei)(count / 3), data);
    return JS_UNDEFINED;
}

static JSValue js_uniform4fv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    std::vector<float> storage; const float* data = nullptr; size_t count = 0;
    if (getFloatArray(ctx, argv[1], storage, &data, &count))
        gl->uniform4fv(unwrapUniformLocation(argv[0]), (GLsizei)(count / 4), data);
    return JS_UNDEFINED;
}

// ===========================================================================
// Uniforms — int vector
// ===========================================================================

static JSValue js_uniform1iv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    std::vector<int32_t> storage; const int32_t* data = nullptr; size_t count = 0;
    if (getInt32Array(ctx, argv[1], storage, &data, &count))
        gl->uniform1iv(unwrapUniformLocation(argv[0]), (GLsizei)(count / 1), data);
    return JS_UNDEFINED;
}

static JSValue js_uniform2iv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    std::vector<int32_t> storage; const int32_t* data = nullptr; size_t count = 0;
    if (getInt32Array(ctx, argv[1], storage, &data, &count))
        gl->uniform2iv(unwrapUniformLocation(argv[0]), (GLsizei)(count / 2), data);
    return JS_UNDEFINED;
}

static JSValue js_uniform3iv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    std::vector<int32_t> storage; const int32_t* data = nullptr; size_t count = 0;
    if (getInt32Array(ctx, argv[1], storage, &data, &count))
        gl->uniform3iv(unwrapUniformLocation(argv[0]), (GLsizei)(count / 3), data);
    return JS_UNDEFINED;
}

static JSValue js_uniform4iv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    std::vector<int32_t> storage; const int32_t* data = nullptr; size_t count = 0;
    if (getInt32Array(ctx, argv[1], storage, &data, &count))
        gl->uniform4iv(unwrapUniformLocation(argv[0]), (GLsizei)(count / 4), data);
    return JS_UNDEFINED;
}

// ===========================================================================
// Uniforms — matrix (square)
// ===========================================================================

static JSValue js_uniformMatrix2fv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    GLboolean transpose = JS_ToBool(ctx, argv[1]) ? GL_TRUE : GL_FALSE;
    std::vector<float> storage; const float* data = nullptr; size_t count = 0;
    if (getFloatArray(ctx, argv[2], storage, &data, &count))
        gl->uniformMatrix2fv(unwrapUniformLocation(argv[0]), (GLsizei)(count / (2 * 2)), transpose, data);
    return JS_UNDEFINED;
}

static JSValue js_uniformMatrix3fv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    GLboolean transpose = JS_ToBool(ctx, argv[1]) ? GL_TRUE : GL_FALSE;
    std::vector<float> storage; const float* data = nullptr; size_t count = 0;
    if (getFloatArray(ctx, argv[2], storage, &data, &count))
        gl->uniformMatrix3fv(unwrapUniformLocation(argv[0]), (GLsizei)(count / (3 * 3)), transpose, data);
    return JS_UNDEFINED;
}

static JSValue js_uniformMatrix4fv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    GLboolean transpose = JS_ToBool(ctx, argv[1]) ? GL_TRUE : GL_FALSE;
    std::vector<float> storage; const float* data = nullptr; size_t count = 0;
    if (getFloatArray(ctx, argv[2], storage, &data, &count))
        gl->uniformMatrix4fv(unwrapUniformLocation(argv[0]), (GLsizei)(count / (4 * 4)), transpose, data);
    return JS_UNDEFINED;
}

// ===========================================================================
// Uniforms — matrix (non-square, WebGL2)
// ===========================================================================

static JSValue js_uniformMatrix2x3fv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    GLboolean transpose = JS_ToBool(ctx, argv[1]) ? GL_TRUE : GL_FALSE;
    std::vector<float> storage; const float* data = nullptr; size_t count = 0;
    if (getFloatArray(ctx, argv[2], storage, &data, &count))
        gl->uniformMatrix2x3fv(unwrapUniformLocation(argv[0]), (GLsizei)(count / (2 * 3)), transpose, data);
    return JS_UNDEFINED;
}

static JSValue js_uniformMatrix3x2fv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    GLboolean transpose = JS_ToBool(ctx, argv[1]) ? GL_TRUE : GL_FALSE;
    std::vector<float> storage; const float* data = nullptr; size_t count = 0;
    if (getFloatArray(ctx, argv[2], storage, &data, &count))
        gl->uniformMatrix3x2fv(unwrapUniformLocation(argv[0]), (GLsizei)(count / (3 * 2)), transpose, data);
    return JS_UNDEFINED;
}

static JSValue js_uniformMatrix2x4fv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    GLboolean transpose = JS_ToBool(ctx, argv[1]) ? GL_TRUE : GL_FALSE;
    std::vector<float> storage; const float* data = nullptr; size_t count = 0;
    if (getFloatArray(ctx, argv[2], storage, &data, &count))
        gl->uniformMatrix2x4fv(unwrapUniformLocation(argv[0]), (GLsizei)(count / (2 * 4)), transpose, data);
    return JS_UNDEFINED;
}

static JSValue js_uniformMatrix4x2fv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    GLboolean transpose = JS_ToBool(ctx, argv[1]) ? GL_TRUE : GL_FALSE;
    std::vector<float> storage; const float* data = nullptr; size_t count = 0;
    if (getFloatArray(ctx, argv[2], storage, &data, &count))
        gl->uniformMatrix4x2fv(unwrapUniformLocation(argv[0]), (GLsizei)(count / (4 * 2)), transpose, data);
    return JS_UNDEFINED;
}

static JSValue js_uniformMatrix3x4fv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    GLboolean transpose = JS_ToBool(ctx, argv[1]) ? GL_TRUE : GL_FALSE;
    std::vector<float> storage; const float* data = nullptr; size_t count = 0;
    if (getFloatArray(ctx, argv[2], storage, &data, &count))
        gl->uniformMatrix3x4fv(unwrapUniformLocation(argv[0]), (GLsizei)(count / (3 * 4)), transpose, data);
    return JS_UNDEFINED;
}

static JSValue js_uniformMatrix4x3fv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    GLboolean transpose = JS_ToBool(ctx, argv[1]) ? GL_TRUE : GL_FALSE;
    std::vector<float> storage; const float* data = nullptr; size_t count = 0;
    if (getFloatArray(ctx, argv[2], storage, &data, &count))
        gl->uniformMatrix4x3fv(unwrapUniformLocation(argv[0]), (GLsizei)(count / (4 * 3)), transpose, data);
    return JS_UNDEFINED;
}

// ===========================================================================
// Exported function list
// ===========================================================================

const JSCFunctionListEntry webgl2_shader_funcs[] = {
    // Shaders
    JS_CFUNC_DEF("createShader", 1, js_createShader),
    JS_CFUNC_DEF("deleteShader", 1, js_deleteShader),
    JS_CFUNC_DEF("shaderSource", 2, js_shaderSource),
    JS_CFUNC_DEF("compileShader", 1, js_compileShader),
    JS_CFUNC_DEF("getShaderParameter", 2, js_getShaderParameter),
    JS_CFUNC_DEF("getShaderInfoLog", 1, js_getShaderInfoLog),
    // Programs
    JS_CFUNC_DEF("createProgram", 0, js_createProgram),
    JS_CFUNC_DEF("deleteProgram", 1, js_deleteProgram),
    JS_CFUNC_DEF("attachShader", 2, js_attachShader),
    JS_CFUNC_DEF("detachShader", 2, js_detachShader),
    JS_CFUNC_DEF("linkProgram", 1, js_linkProgram),
    JS_CFUNC_DEF("useProgram", 1, js_useProgram),
    JS_CFUNC_DEF("getProgramParameter", 2, js_getProgramParameter),
    JS_CFUNC_DEF("getProgramInfoLog", 1, js_getProgramInfoLog),
    JS_CFUNC_DEF("bindAttribLocation", 3, js_bindAttribLocation),
    JS_CFUNC_DEF("getAttribLocation", 2, js_getAttribLocation),
    JS_CFUNC_DEF("getFragDataLocation", 2, js_getFragDataLocation),
    JS_CFUNC_DEF("getUniformLocation", 2, js_getUniformLocation),
    JS_CFUNC_DEF("getActiveAttrib", 2, js_getActiveAttrib),
    JS_CFUNC_DEF("getActiveUniform", 2, js_getActiveUniform),
    JS_CFUNC_DEF("getUniformBlockIndex", 2, js_getUniformBlockIndex),
    JS_CFUNC_DEF("uniformBlockBinding", 3, js_uniformBlockBinding),
    JS_CFUNC_DEF("getUniformIndices", 2, js_getUniformIndices),
    JS_CFUNC_DEF("getActiveUniforms", 3, js_getActiveUniforms),
    JS_CFUNC_DEF("getActiveUniformBlockParameter", 3, js_getActiveUniformBlockParameter),
    JS_CFUNC_DEF("getActiveUniformBlockName", 2, js_getActiveUniformBlockName),
    JS_CFUNC_DEF("isProgram", 1, js_isProgram),
    JS_CFUNC_DEF("isShader", 1, js_isShader),
    // Uniforms — scalar float
    JS_CFUNC_DEF("uniform1f", 2, js_uniform1f),
    JS_CFUNC_DEF("uniform2f", 3, js_uniform2f),
    JS_CFUNC_DEF("uniform3f", 4, js_uniform3f),
    JS_CFUNC_DEF("uniform4f", 5, js_uniform4f),
    // Uniforms — scalar int
    JS_CFUNC_DEF("uniform1i", 2, js_uniform1i),
    JS_CFUNC_DEF("uniform2i", 3, js_uniform2i),
    JS_CFUNC_DEF("uniform3i", 4, js_uniform3i),
    JS_CFUNC_DEF("uniform4i", 5, js_uniform4i),
    // Uniforms — scalar unsigned int (WebGL2)
    JS_CFUNC_DEF("uniform1ui", 2, js_uniform1ui),
    JS_CFUNC_DEF("uniform2ui", 3, js_uniform2ui),
    JS_CFUNC_DEF("uniform3ui", 4, js_uniform3ui),
    JS_CFUNC_DEF("uniform4ui", 5, js_uniform4ui),
    // Uniforms — unsigned int vector (WebGL2)
    JS_CFUNC_DEF("uniform1uiv", 2, js_uniform1uiv),
    JS_CFUNC_DEF("uniform2uiv", 2, js_uniform2uiv),
    JS_CFUNC_DEF("uniform3uiv", 2, js_uniform3uiv),
    JS_CFUNC_DEF("uniform4uiv", 2, js_uniform4uiv),
    // Uniforms — float vector
    JS_CFUNC_DEF("uniform1fv", 2, js_uniform1fv),
    JS_CFUNC_DEF("uniform2fv", 2, js_uniform2fv),
    JS_CFUNC_DEF("uniform3fv", 2, js_uniform3fv),
    JS_CFUNC_DEF("uniform4fv", 2, js_uniform4fv),
    // Uniforms — int vector
    JS_CFUNC_DEF("uniform1iv", 2, js_uniform1iv),
    JS_CFUNC_DEF("uniform2iv", 2, js_uniform2iv),
    JS_CFUNC_DEF("uniform3iv", 2, js_uniform3iv),
    JS_CFUNC_DEF("uniform4iv", 2, js_uniform4iv),
    // Uniforms — matrix (square)
    JS_CFUNC_DEF("uniformMatrix2fv", 3, js_uniformMatrix2fv),
    JS_CFUNC_DEF("uniformMatrix3fv", 3, js_uniformMatrix3fv),
    JS_CFUNC_DEF("uniformMatrix4fv", 3, js_uniformMatrix4fv),
    // Uniforms — matrix (non-square)
    JS_CFUNC_DEF("uniformMatrix2x3fv", 3, js_uniformMatrix2x3fv),
    JS_CFUNC_DEF("uniformMatrix3x2fv", 3, js_uniformMatrix3x2fv),
    JS_CFUNC_DEF("uniformMatrix2x4fv", 3, js_uniformMatrix2x4fv),
    JS_CFUNC_DEF("uniformMatrix4x2fv", 3, js_uniformMatrix4x2fv),
    JS_CFUNC_DEF("uniformMatrix3x4fv", 3, js_uniformMatrix3x4fv),
    JS_CFUNC_DEF("uniformMatrix4x3fv", 3, js_uniformMatrix4x3fv),
};
const int webgl2_shader_funcs_count = sizeof(webgl2_shader_funcs) / sizeof(webgl2_shader_funcs[0]);

} // namespace bro::js::webgl2
