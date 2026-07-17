#include "js/webgl2_bindings_util.h"
#include <glad/gl.h>

namespace bro::js::webgl2 {

// ===========================================================================
// Queries: getParameter, getExtension, getSupportedExtensions, etc.
// ===========================================================================

static JSValue js_getParameter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t pname; JS_ToUint32(ctx, &pname, argv[0]);

    switch (pname) {
        // String parameters
        case 0x1F02: // GL_VERSION
            return JS_NewString(ctx, "WebGL 2.0");
        case 0x8B8C: // GL_SHADING_LANGUAGE_VERSION
            return JS_NewString(ctx, "WebGL GLSL ES 3.00");
        case 0x1F01: // GL_RENDERER
        case 0x1F00: { // GL_VENDOR
            std::string s = gl->getParameterString(pname);
            return JS_NewString(ctx, s.c_str());
        }

        // Float parameters
        case 0x0B73: // GL_DEPTH_CLEAR_VALUE
        case 0x0B21: // GL_LINE_WIDTH (0x0B11 is POINT_SIZE, not a WebGL pname)
        case 0x80AA: // GL_SAMPLE_COVERAGE_VALUE (0x8005 is BLEND_COLOR)
        case 0x8066: // GL_POLYGON_OFFSET_FACTOR
        case 0x2A00: { // GL_POLYGON_OFFSET_UNITS
            return JS_NewFloat64(ctx, gl->getParameterFloat(pname));
        }

        // Int32Array[4] parameters (viewport, scissor box)
        case 0x0BA2: // GL_VIEWPORT
        case 0x0C10: { // GL_SCISSOR_BOX
            GLint v[4]; glGetIntegerv(pname, v);
            JSValue arr = JS_NewArray(ctx);  // WebGL returns Int32Array but plain array works
            for (int i = 0; i < 4; i++)
                JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, v[i]));
            return arr;
        }

        // Int32Array[2] parameters — these write TWO ints; routing them
        // through the single-value default path would smash the stack.
        case 0x0D3A: { // GL_MAX_VIEWPORT_DIMS
            GLint v[2] = {0, 0}; glGetIntegerv(pname, v);
            JSValue arr = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, v[0]));
            JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, v[1]));
            return arr;
        }

        // Float32Array[2] parameters
        case 0x846D: // GL_ALIASED_POINT_SIZE_RANGE
        case 0x846E: { // GL_ALIASED_LINE_WIDTH_RANGE
            GLfloat v[2] = {0, 0}; glGetFloatv(pname, v);
            JSValue arr = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, v[0]));
            JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, v[1]));
            return arr;
        }

        // WebGL-only pixel-store state — not real GL enums; answered from
        // the context's shadow state (the default path would raise
        // GL_INVALID_ENUM from glGetIntegerv).
        case 0x9240: // UNPACK_FLIP_Y_WEBGL
            return JS_NewBool(ctx, gl->unpackFlipY());
        case 0x9241: // UNPACK_PREMULTIPLY_ALPHA_WEBGL
            return JS_NewBool(ctx, gl->unpackPremultiplyAlpha());
        case 0x9243: // UNPACK_COLORSPACE_CONVERSION_WEBGL
            return JS_NewInt32(ctx, gl->unpackColorspaceConversion());

        // Float32Array[4] parameters
        case 0x0C22: // GL_COLOR_CLEAR_VALUE
        case 0x8005: { // GL_BLEND_COLOR (0x0BE0 is BLEND_DST, a scalar)
            GLfloat v[4]; glGetFloatv(pname, v);
            JSValue arr = JS_NewArray(ctx);
            for (int i = 0; i < 4; i++)
                JS_SetPropertyUint32(ctx, arr, i, JS_NewFloat64(ctx, v[i]));
            return arr;
        }

        // Float32Array[2] parameters
        case 0x0B70: { // GL_DEPTH_RANGE
            GLfloat v[2]; glGetFloatv(pname, v);
            JSValue arr = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, v[0]));
            JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, v[1]));
            return arr;
        }

        // Boolean[4] parameters
        case 0x0C23: { // GL_COLOR_WRITEMASK
            GLboolean v[4]; glGetBooleanv(pname, v);
            JSValue arr = JS_NewArray(ctx);
            for (int i = 0; i < 4; i++)
                JS_SetPropertyUint32(ctx, arr, i, JS_NewBool(ctx, v[i]));
            return arr;
        }

        // Null parameters (binding queries that return WebGL objects)
        case 0x8894: // GL_ARRAY_BUFFER_BINDING
        case 0x8895: // GL_ELEMENT_ARRAY_BUFFER_BINDING
        case 0x8B8D: // GL_CURRENT_PROGRAM
        case 0x8CA6: // GL_FRAMEBUFFER_BINDING
        case 0x8CA7: // GL_RENDERBUFFER_BINDING
        case 0x8069: // GL_TEXTURE_BINDING_2D
        case 0x8514: // GL_TEXTURE_BINDING_CUBE_MAP
        case 0x85B5: // GL_VERTEX_ARRAY_BINDING
            // three.js probes these but we can't return wrapped objects from here.
            // Return null (unbound) — three.js handles this gracefully.
            return JS_NULL;

        // Compressed texture formats — return empty array
        case 0x86A3: { // GL_COMPRESSED_TEXTURE_FORMATS
            return JS_NewArray(ctx);
        }

        // Boolean parameters
        case 0x0BE2: // GL_BLEND
        case 0x0B71: // GL_DEPTH_TEST
        case 0x0B44: // GL_CULL_FACE
        case 0x0C11: // GL_SCISSOR_TEST
        case 0x0B90: // GL_STENCIL_TEST
        case 0x0BD0: // GL_DITHER
        case 0x8037: // GL_POLYGON_OFFSET_FILL
        case 0x809E: // GL_SAMPLE_ALPHA_TO_COVERAGE
        case 0x80A0: // GL_SAMPLE_COVERAGE
        case 0x8C89: // GL_RASTERIZER_DISCARD
        case 0x0B72: // GL_DEPTH_WRITEMASK
            return JS_NewBool(ctx, gl->getParameterBool(pname));

        // Default: integer parameter
        default:
            return JS_NewInt32(ctx, gl->getParameterInt(pname));
    }
}

static JSValue js_getExtension(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_NULL;
    std::string name = jsStr(ctx, argv[0]);
    if (gl->getExtension(name)) {
        // Return truthy empty object (WebGL convention)
        return JS_NewObject(ctx);
    }
    return JS_NULL;
}

static JSValue js_getSupportedExtensions(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* gl = getCtx(this_val); if (!gl) return JS_NULL;
    auto exts = gl->getSupportedExtensions();
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < exts.size(); i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, exts[i].c_str()));
    }
    return arr;
}

static JSValue js_getShaderPrecisionFormat(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val; (void)argc; (void)argv;
    // Return hardcoded highp float precision (covers all practical cases)
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "rangeMin", JS_NewInt32(ctx, 127));
    JS_SetPropertyStr(ctx, obj, "rangeMax", JS_NewInt32(ctx, 127));
    JS_SetPropertyStr(ctx, obj, "precision", JS_NewInt32(ctx, 23));
    return obj;
}

static JSValue js_isContextLost(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, 0);
}

static JSValue js_getContextAttributes(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "alpha", JS_TRUE);
    JS_SetPropertyStr(ctx, obj, "depth", JS_TRUE);
    JS_SetPropertyStr(ctx, obj, "stencil", JS_TRUE);
    JS_SetPropertyStr(ctx, obj, "antialias", JS_FALSE);
    JS_SetPropertyStr(ctx, obj, "premultipliedAlpha", JS_TRUE);
    JS_SetPropertyStr(ctx, obj, "preserveDrawingBuffer", JS_FALSE);
    JS_SetPropertyStr(ctx, obj, "powerPreference", JS_NewString(ctx, "default"));
    JS_SetPropertyStr(ctx, obj, "failIfMajorPerformanceCaveat", JS_FALSE);
    JS_SetPropertyStr(ctx, obj, "desynchronized", JS_FALSE);
    return obj;
}

// ===========================================================================
// Property getters: drawingBufferWidth, drawingBufferHeight
// ===========================================================================

static JSValue js_get_drawingBufferWidth(JSContext* ctx, JSValueConst this_val) {
    auto* gl = getCtx(this_val); if (!gl) return JS_UNDEFINED;
    return JS_NewInt32(ctx, gl->canvasWidth());
}

static JSValue js_get_drawingBufferHeight(JSContext* ctx, JSValueConst this_val) {
    auto* gl = getCtx(this_val); if (!gl) return JS_UNDEFINED;
    return JS_NewInt32(ctx, gl->canvasHeight());
}

// ===========================================================================
// Exported function list
// ===========================================================================

const JSCFunctionListEntry webgl2_query_funcs[] = {
    JS_CFUNC_DEF("getParameter", 1, js_getParameter),
    JS_CFUNC_DEF("getExtension", 1, js_getExtension),
    JS_CFUNC_DEF("getSupportedExtensions", 0, js_getSupportedExtensions),
    JS_CFUNC_DEF("getShaderPrecisionFormat", 2, js_getShaderPrecisionFormat),
    JS_CFUNC_DEF("isContextLost", 0, js_isContextLost),
    JS_CFUNC_DEF("getContextAttributes", 0, js_getContextAttributes),
    JS_CGETSET_DEF("drawingBufferWidth", js_get_drawingBufferWidth, NULL),
    JS_CGETSET_DEF("drawingBufferHeight", js_get_drawingBufferHeight, NULL),
};
const int webgl2_query_funcs_count = sizeof(webgl2_query_funcs) / sizeof(webgl2_query_funcs[0]);

} // namespace bro::js::webgl2
