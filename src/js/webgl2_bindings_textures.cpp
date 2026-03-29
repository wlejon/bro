#include "js/webgl2_bindings_util.h"

namespace bro::js::webgl2 {

// ===========================================================================
// Textures
// ===========================================================================

static JSValue js_createTexture(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* gl = getCtx(this_val); if (!gl) return JS_NULL;
    return wrapTexture(ctx, gl->createTexture());
}

static JSValue js_deleteTexture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    gl->deleteTexture(unwrapTexture(argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_bindTexture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    uint32_t target; JS_ToUint32(ctx, &target, argv[0]);
    gl->bindTexture(target, unwrapTexture(argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_activeTexture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t texture; JS_ToUint32(ctx, &texture, argv[0]);
    gl->activeTexture(texture);
    return JS_UNDEFINED;
}

static JSValue js_texParameteri(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t target, pname; int param;
    JS_ToUint32(ctx, &target, argv[0]); JS_ToUint32(ctx, &pname, argv[1]);
    JS_ToInt32(ctx, &param, argv[2]);
    gl->texParameteri(target, pname, param);
    return JS_UNDEFINED;
}

static JSValue js_texParameterf(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t target, pname; double param;
    JS_ToUint32(ctx, &target, argv[0]); JS_ToUint32(ctx, &pname, argv[1]);
    JS_ToFloat64(ctx, &param, argv[2]);
    gl->texParameterf(target, pname, (float)param);
    return JS_UNDEFINED;
}

static JSValue js_texImage2D(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 9) return JS_UNDEFINED;
    uint32_t target, format, type;
    int level, internalformat, width, height, border;
    JS_ToUint32(ctx, &target, argv[0]); JS_ToInt32(ctx, &level, argv[1]);
    JS_ToInt32(ctx, &internalformat, argv[2]);
    JS_ToInt32(ctx, &width, argv[3]); JS_ToInt32(ctx, &height, argv[4]);
    JS_ToInt32(ctx, &border, argv[5]);
    JS_ToUint32(ctx, &format, argv[6]); JS_ToUint32(ctx, &type, argv[7]);

    if (JS_IsNull(argv[8]) || JS_IsUndefined(argv[8])) {
        gl->texImage2D(target, level, internalformat, width, height, border, format, type, nullptr);
    } else {
        const uint8_t* data = nullptr;
        size_t len = 0;
        if (getBufferData(ctx, argv[8], &data, &len)) {
            gl->texImage2D(target, level, internalformat, width, height, border, format, type, data);
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_texSubImage2D(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 9) return JS_UNDEFINED;
    uint32_t target, format, type;
    int level, xoffset, yoffset, width, height;
    JS_ToUint32(ctx, &target, argv[0]); JS_ToInt32(ctx, &level, argv[1]);
    JS_ToInt32(ctx, &xoffset, argv[2]); JS_ToInt32(ctx, &yoffset, argv[3]);
    JS_ToInt32(ctx, &width, argv[4]); JS_ToInt32(ctx, &height, argv[5]);
    JS_ToUint32(ctx, &format, argv[6]); JS_ToUint32(ctx, &type, argv[7]);

    const uint8_t* data = nullptr;
    size_t len = 0;
    if (getBufferData(ctx, argv[8], &data, &len)) {
        gl->texSubImage2D(target, level, xoffset, yoffset, width, height, format, type, data);
    }
    return JS_UNDEFINED;
}

static JSValue js_texImage3D(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 10) return JS_UNDEFINED;
    uint32_t target, format, type;
    int level, internalformat, width, height, depth, border;
    JS_ToUint32(ctx, &target, argv[0]); JS_ToInt32(ctx, &level, argv[1]);
    JS_ToInt32(ctx, &internalformat, argv[2]);
    JS_ToInt32(ctx, &width, argv[3]); JS_ToInt32(ctx, &height, argv[4]);
    JS_ToInt32(ctx, &depth, argv[5]); JS_ToInt32(ctx, &border, argv[6]);
    JS_ToUint32(ctx, &format, argv[7]); JS_ToUint32(ctx, &type, argv[8]);

    if (JS_IsNull(argv[9]) || JS_IsUndefined(argv[9])) {
        gl->texImage3D(target, level, internalformat, width, height, depth, border, format, type, nullptr);
    } else {
        const uint8_t* data = nullptr;
        size_t len = 0;
        if (getBufferData(ctx, argv[9], &data, &len)) {
            gl->texImage3D(target, level, internalformat, width, height, depth, border, format, type, data);
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_texSubImage3D(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 11) return JS_UNDEFINED;
    uint32_t target, format, type;
    int level, xoffset, yoffset, zoffset, width, height, depth;
    JS_ToUint32(ctx, &target, argv[0]); JS_ToInt32(ctx, &level, argv[1]);
    JS_ToInt32(ctx, &xoffset, argv[2]); JS_ToInt32(ctx, &yoffset, argv[3]);
    JS_ToInt32(ctx, &zoffset, argv[4]);
    JS_ToInt32(ctx, &width, argv[5]); JS_ToInt32(ctx, &height, argv[6]);
    JS_ToInt32(ctx, &depth, argv[7]);
    JS_ToUint32(ctx, &format, argv[8]); JS_ToUint32(ctx, &type, argv[9]);

    const uint8_t* data = nullptr;
    size_t len = 0;
    if (getBufferData(ctx, argv[10], &data, &len)) {
        gl->texSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, data);
    }
    return JS_UNDEFINED;
}

static JSValue js_generateMipmap(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t target; JS_ToUint32(ctx, &target, argv[0]);
    gl->generateMipmap(target);
    return JS_UNDEFINED;
}

static JSValue js_texStorage2D(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 5) return JS_UNDEFINED;
    uint32_t target, internalformat; int levels, width, height;
    JS_ToUint32(ctx, &target, argv[0]); JS_ToInt32(ctx, &levels, argv[1]);
    JS_ToUint32(ctx, &internalformat, argv[2]);
    JS_ToInt32(ctx, &width, argv[3]); JS_ToInt32(ctx, &height, argv[4]);
    gl->texStorage2D(target, levels, internalformat, width, height);
    return JS_UNDEFINED;
}

static JSValue js_texStorage3D(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 6) return JS_UNDEFINED;
    uint32_t target, internalformat; int levels, width, height, depth;
    JS_ToUint32(ctx, &target, argv[0]); JS_ToInt32(ctx, &levels, argv[1]);
    JS_ToUint32(ctx, &internalformat, argv[2]);
    JS_ToInt32(ctx, &width, argv[3]); JS_ToInt32(ctx, &height, argv[4]);
    JS_ToInt32(ctx, &depth, argv[5]);
    gl->texStorage3D(target, levels, internalformat, width, height, depth);
    return JS_UNDEFINED;
}

// ===========================================================================
// Exported function list
// ===========================================================================

const JSCFunctionListEntry webgl2_texture_funcs[] = {
    JS_CFUNC_DEF("createTexture", 0, js_createTexture),
    JS_CFUNC_DEF("deleteTexture", 1, js_deleteTexture),
    JS_CFUNC_DEF("bindTexture", 2, js_bindTexture),
    JS_CFUNC_DEF("activeTexture", 1, js_activeTexture),
    JS_CFUNC_DEF("texParameteri", 3, js_texParameteri),
    JS_CFUNC_DEF("texParameterf", 3, js_texParameterf),
    JS_CFUNC_DEF("texImage2D", 9, js_texImage2D),
    JS_CFUNC_DEF("texSubImage2D", 9, js_texSubImage2D),
    JS_CFUNC_DEF("texImage3D", 10, js_texImage3D),
    JS_CFUNC_DEF("texSubImage3D", 11, js_texSubImage3D),
    JS_CFUNC_DEF("generateMipmap", 1, js_generateMipmap),
    JS_CFUNC_DEF("texStorage2D", 5, js_texStorage2D),
    JS_CFUNC_DEF("texStorage3D", 6, js_texStorage3D),
};
const int webgl2_texture_funcs_count = sizeof(webgl2_texture_funcs) / sizeof(webgl2_texture_funcs[0]);

} // namespace bro::js::webgl2
