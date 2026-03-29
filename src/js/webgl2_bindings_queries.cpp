#include "js/webgl2_bindings_util.h"

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
        case 0x8CAB: // GL_RASTERIZER_DISCARD
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
    JS_CGETSET_DEF("drawingBufferWidth", js_get_drawingBufferWidth, NULL),
    JS_CGETSET_DEF("drawingBufferHeight", js_get_drawingBufferHeight, NULL),
};
const int webgl2_query_funcs_count = sizeof(webgl2_query_funcs) / sizeof(webgl2_query_funcs[0]);

} // namespace bro::js::webgl2
