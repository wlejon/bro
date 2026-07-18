#include "js/webgl2_bindings_util.h"
#include <glad/gl.h>

namespace bro::js::webgl2 {

// ===========================================================================
// Sampler objects
// ===========================================================================

static JSValue js_createSampler(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* gl = getCtx(this_val); if (!gl) return JS_NULL;
    return wrapSampler(ctx, gl->createSampler());
}

static JSValue js_deleteSampler(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    gl->deleteSampler(unwrapSampler(argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_bindSampler(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    uint32_t unit; JS_ToUint32(ctx, &unit, argv[0]);
    gl->bindSampler(unit, unwrapSampler(argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_samplerParameteri(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t pname; int param;
    JS_ToUint32(ctx, &pname, argv[1]); JS_ToInt32(ctx, &param, argv[2]);
    gl->samplerParameteri(unwrapSampler(argv[0]), pname, param);
    return JS_UNDEFINED;
}

static JSValue js_samplerParameterf(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t pname; double param;
    JS_ToUint32(ctx, &pname, argv[1]); JS_ToFloat64(ctx, &param, argv[2]);
    gl->samplerParameterf(unwrapSampler(argv[0]), pname, (float)param);
    return JS_UNDEFINED;
}

static JSValue js_getSamplerParameter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_NULL;
    uint32_t pname; JS_ToUint32(ctx, &pname, argv[1]);
    auto s = unwrapSampler(argv[0]);
    switch (pname) {
        case 0x813A: // TEXTURE_MIN_LOD
        case 0x813B: // TEXTURE_MAX_LOD
            return JS_NewFloat64(ctx, gl->getSamplerParameterf(s, pname));
        default:     // filter / wrap / compare enums
            return JS_NewInt32(ctx, gl->getSamplerParameteri(s, pname));
    }
}

static JSValue js_isSampler(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_FALSE;
    return JS_NewBool(ctx, gl->isSampler(unwrapSampler(argv[0])));
}

// ===========================================================================
// Sync objects
// ===========================================================================

static JSValue js_fenceSync(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_NULL;
    uint32_t condition, flags;
    JS_ToUint32(ctx, &condition, argv[0]); JS_ToUint32(ctx, &flags, argv[1]);
    auto s = gl->fenceSync(condition, flags);
    if (!s.sync) return JS_NULL;
    return wrapSync(ctx, s);
}

static JSValue js_deleteSync(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    gl->deleteSync(unwrapSync(argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_clientWaitSync(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t flags; double timeout;
    JS_ToUint32(ctx, &flags, argv[1]); JS_ToFloat64(ctx, &timeout, argv[2]);
    return JS_NewUint32(ctx, gl->clientWaitSync(unwrapSync(argv[0]), flags, timeout));
}

static JSValue js_waitSync(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t flags; double timeout;
    JS_ToUint32(ctx, &flags, argv[1]); JS_ToFloat64(ctx, &timeout, argv[2]);
    gl->waitSync(unwrapSync(argv[0]), flags, timeout);
    return JS_UNDEFINED;
}

static JSValue js_getSyncParameter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_NULL;
    uint32_t pname; JS_ToUint32(ctx, &pname, argv[1]);
    return JS_NewInt32(ctx, gl->getSyncParameter(unwrapSync(argv[0]), pname));
}

static JSValue js_isSync(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_FALSE;
    return JS_NewBool(ctx, gl->isSync(unwrapSync(argv[0])));
}

// ===========================================================================
// Query objects
//
// getQuery(target, CURRENT_QUERY) must return the WebGLQuery object that is
// active on the target. The C++ context only knows GL ids, so the binding
// stashes the JS wrapper on the context object under a hidden per-target
// property while the query is active.
// ===========================================================================

static void activeQueryProp(char* buf, size_t n, uint32_t target) {
    snprintf(buf, n, "__activeQuery%x", target);
}

static JSValue js_createQuery(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* gl = getCtx(this_val); if (!gl) return JS_NULL;
    return wrapQuery(ctx, gl->createQuery());
}

static JSValue js_deleteQuery(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    gl->deleteQuery(unwrapQuery(argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_beginQuery(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    uint32_t target; JS_ToUint32(ctx, &target, argv[0]);
    gl->beginQuery(target, unwrapQuery(argv[1]));
    char prop[32]; activeQueryProp(prop, sizeof(prop), target);
    JS_SetPropertyStr(ctx, this_val, prop, JS_DupValue(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_endQuery(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t target; JS_ToUint32(ctx, &target, argv[0]);
    gl->endQuery(target);
    char prop[32]; activeQueryProp(prop, sizeof(prop), target);
    JS_SetPropertyStr(ctx, this_val, prop, JS_NULL);
    return JS_UNDEFINED;
}

static JSValue js_getQuery(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_NULL;
    uint32_t target, pname;
    JS_ToUint32(ctx, &target, argv[0]); JS_ToUint32(ctx, &pname, argv[1]);
    if (pname != 0x8865 /* CURRENT_QUERY */) return JS_NULL;
    char prop[32]; activeQueryProp(prop, sizeof(prop), target);
    JSValue v = JS_GetPropertyStr(ctx, this_val, prop);
    if (JS_IsUndefined(v)) return JS_NULL;
    return v;
}

static JSValue js_getQueryParameter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_NULL;
    uint32_t pname; JS_ToUint32(ctx, &pname, argv[1]);
    GLuint v = gl->getQueryParameteru(unwrapQuery(argv[0]), pname);
    if (pname == 0x8867 /* QUERY_RESULT_AVAILABLE */) return JS_NewBool(ctx, v != 0);
    return JS_NewUint32(ctx, v); // QUERY_RESULT
}

static JSValue js_isQuery(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_FALSE;
    return JS_NewBool(ctx, gl->isQuery(unwrapQuery(argv[0])));
}

// ===========================================================================
// Exported function list
// ===========================================================================

const JSCFunctionListEntry webgl2_object_funcs[] = {
    // Samplers
    JS_CFUNC_DEF("createSampler", 0, js_createSampler),
    JS_CFUNC_DEF("deleteSampler", 1, js_deleteSampler),
    JS_CFUNC_DEF("bindSampler", 2, js_bindSampler),
    JS_CFUNC_DEF("samplerParameteri", 3, js_samplerParameteri),
    JS_CFUNC_DEF("samplerParameterf", 3, js_samplerParameterf),
    JS_CFUNC_DEF("getSamplerParameter", 2, js_getSamplerParameter),
    JS_CFUNC_DEF("isSampler", 1, js_isSampler),
    // Sync
    JS_CFUNC_DEF("fenceSync", 2, js_fenceSync),
    JS_CFUNC_DEF("deleteSync", 1, js_deleteSync),
    JS_CFUNC_DEF("clientWaitSync", 3, js_clientWaitSync),
    JS_CFUNC_DEF("waitSync", 3, js_waitSync),
    JS_CFUNC_DEF("getSyncParameter", 2, js_getSyncParameter),
    JS_CFUNC_DEF("isSync", 1, js_isSync),
    // Queries
    JS_CFUNC_DEF("createQuery", 0, js_createQuery),
    JS_CFUNC_DEF("deleteQuery", 1, js_deleteQuery),
    JS_CFUNC_DEF("beginQuery", 2, js_beginQuery),
    JS_CFUNC_DEF("endQuery", 1, js_endQuery),
    JS_CFUNC_DEF("getQuery", 2, js_getQuery),
    JS_CFUNC_DEF("getQueryParameter", 2, js_getQueryParameter),
    JS_CFUNC_DEF("isQuery", 1, js_isQuery),
};
const int webgl2_object_funcs_count = sizeof(webgl2_object_funcs) / sizeof(webgl2_object_funcs[0]);

} // namespace bro::js::webgl2
