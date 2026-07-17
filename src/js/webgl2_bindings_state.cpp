#include "js/webgl2_bindings_util.h"

namespace bro::js::webgl2 {

// ===========================================================================
// State management + draw calls
// ===========================================================================

static JSValue js_viewport(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_UNDEFINED;
    int x, y, w, h;
    JS_ToInt32(ctx, &x, argv[0]); JS_ToInt32(ctx, &y, argv[1]);
    JS_ToInt32(ctx, &w, argv[2]); JS_ToInt32(ctx, &h, argv[3]);
    gl->viewport(x, y, w, h);
    return JS_UNDEFINED;
}

static JSValue js_scissor(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_UNDEFINED;
    int x, y, w, h;
    JS_ToInt32(ctx, &x, argv[0]); JS_ToInt32(ctx, &y, argv[1]);
    JS_ToInt32(ctx, &w, argv[2]); JS_ToInt32(ctx, &h, argv[3]);
    gl->scissor(x, y, w, h);
    return JS_UNDEFINED;
}

static JSValue js_clearColor(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_UNDEFINED;
    double r, g, b, a;
    JS_ToFloat64(ctx, &r, argv[0]); JS_ToFloat64(ctx, &g, argv[1]);
    JS_ToFloat64(ctx, &b, argv[2]); JS_ToFloat64(ctx, &a, argv[3]);
    gl->clearColor((float)r, (float)g, (float)b, (float)a);
    return JS_UNDEFINED;
}

static JSValue js_clearDepth(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    double d; JS_ToFloat64(ctx, &d, argv[0]);
    gl->clearDepth((float)d);
    return JS_UNDEFINED;
}

static JSValue js_clearStencil(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    int s; JS_ToInt32(ctx, &s, argv[0]);
    gl->clearStencil(s);
    return JS_UNDEFINED;
}

static JSValue js_clear(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t mask; JS_ToUint32(ctx, &mask, argv[0]);
    gl->clear(mask);
    return JS_UNDEFINED;
}

static JSValue js_enable(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t cap; JS_ToUint32(ctx, &cap, argv[0]);
    gl->enable(cap);
    return JS_UNDEFINED;
}

static JSValue js_disable(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t cap; JS_ToUint32(ctx, &cap, argv[0]);
    gl->disable(cap);
    return JS_UNDEFINED;
}

static JSValue js_isEnabled(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t cap; JS_ToUint32(ctx, &cap, argv[0]);
    return JS_NewBool(ctx, gl->isEnabled(cap));
}

static JSValue js_depthFunc(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t f; JS_ToUint32(ctx, &f, argv[0]);
    gl->depthFunc(f);
    return JS_UNDEFINED;
}

static JSValue js_depthMask(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    gl->depthMask(JS_ToBool(ctx, argv[0]) ? GL_TRUE : GL_FALSE);
    return JS_UNDEFINED;
}

static JSValue js_depthRange(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    double n, f; JS_ToFloat64(ctx, &n, argv[0]); JS_ToFloat64(ctx, &f, argv[1]);
    gl->depthRange((float)n, (float)f);
    return JS_UNDEFINED;
}

static JSValue js_blendFunc(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    uint32_t s, d; JS_ToUint32(ctx, &s, argv[0]); JS_ToUint32(ctx, &d, argv[1]);
    gl->blendFunc(s, d);
    return JS_UNDEFINED;
}

static JSValue js_blendFuncSeparate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_UNDEFINED;
    uint32_t sr, dr, sa, da;
    JS_ToUint32(ctx, &sr, argv[0]); JS_ToUint32(ctx, &dr, argv[1]);
    JS_ToUint32(ctx, &sa, argv[2]); JS_ToUint32(ctx, &da, argv[3]);
    gl->blendFuncSeparate(sr, dr, sa, da);
    return JS_UNDEFINED;
}

static JSValue js_blendEquation(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t m; JS_ToUint32(ctx, &m, argv[0]);
    gl->blendEquation(m);
    return JS_UNDEFINED;
}

static JSValue js_blendEquationSeparate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    uint32_t mr, ma; JS_ToUint32(ctx, &mr, argv[0]); JS_ToUint32(ctx, &ma, argv[1]);
    gl->blendEquationSeparate(mr, ma);
    return JS_UNDEFINED;
}

static JSValue js_blendColor(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_UNDEFINED;
    double r, g, b, a;
    JS_ToFloat64(ctx, &r, argv[0]); JS_ToFloat64(ctx, &g, argv[1]);
    JS_ToFloat64(ctx, &b, argv[2]); JS_ToFloat64(ctx, &a, argv[3]);
    gl->blendColor((float)r, (float)g, (float)b, (float)a);
    return JS_UNDEFINED;
}

static JSValue js_colorMask(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_UNDEFINED;
    gl->colorMask(JS_ToBool(ctx, argv[0]), JS_ToBool(ctx, argv[1]),
                  JS_ToBool(ctx, argv[2]), JS_ToBool(ctx, argv[3]));
    return JS_UNDEFINED;
}

static JSValue js_stencilFunc(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t f, m; int r;
    JS_ToUint32(ctx, &f, argv[0]); JS_ToInt32(ctx, &r, argv[1]); JS_ToUint32(ctx, &m, argv[2]);
    gl->stencilFunc(f, r, m);
    return JS_UNDEFINED;
}

static JSValue js_stencilFuncSeparate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_UNDEFINED;
    uint32_t face, f, m; int r;
    JS_ToUint32(ctx, &face, argv[0]); JS_ToUint32(ctx, &f, argv[1]);
    JS_ToInt32(ctx, &r, argv[2]); JS_ToUint32(ctx, &m, argv[3]);
    gl->stencilFuncSeparate(face, f, r, m);
    return JS_UNDEFINED;
}

static JSValue js_stencilOp(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t f, zf, zp;
    JS_ToUint32(ctx, &f, argv[0]); JS_ToUint32(ctx, &zf, argv[1]); JS_ToUint32(ctx, &zp, argv[2]);
    gl->stencilOp(f, zf, zp);
    return JS_UNDEFINED;
}

static JSValue js_stencilOpSeparate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_UNDEFINED;
    uint32_t face, f, zf, zp;
    JS_ToUint32(ctx, &face, argv[0]); JS_ToUint32(ctx, &f, argv[1]);
    JS_ToUint32(ctx, &zf, argv[2]); JS_ToUint32(ctx, &zp, argv[3]);
    gl->stencilOpSeparate(face, f, zf, zp);
    return JS_UNDEFINED;
}

static JSValue js_stencilMask(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t m; JS_ToUint32(ctx, &m, argv[0]);
    gl->stencilMask(m);
    return JS_UNDEFINED;
}

static JSValue js_stencilMaskSeparate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    uint32_t face, m; JS_ToUint32(ctx, &face, argv[0]); JS_ToUint32(ctx, &m, argv[1]);
    gl->stencilMaskSeparate(face, m);
    return JS_UNDEFINED;
}

static JSValue js_cullFace(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t m; JS_ToUint32(ctx, &m, argv[0]);
    gl->cullFace(m);
    return JS_UNDEFINED;
}

static JSValue js_frontFace(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    uint32_t m; JS_ToUint32(ctx, &m, argv[0]);
    gl->frontFace(m);
    return JS_UNDEFINED;
}

static JSValue js_polygonOffset(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    double f, u; JS_ToFloat64(ctx, &f, argv[0]); JS_ToFloat64(ctx, &u, argv[1]);
    gl->polygonOffset((float)f, (float)u);
    return JS_UNDEFINED;
}

static JSValue js_lineWidth(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 1) return JS_UNDEFINED;
    double w; JS_ToFloat64(ctx, &w, argv[0]);
    gl->lineWidth((float)w);
    return JS_UNDEFINED;
}

static JSValue js_pixelStorei(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    uint32_t pname; int param;
    JS_ToUint32(ctx, &pname, argv[0]); JS_ToInt32(ctx, &param, argv[1]);
    gl->pixelStorei(pname, param);
    return JS_UNDEFINED;
}

static JSValue js_getError(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* gl = getCtx(this_val); if (!gl) return JS_UNDEFINED;
    return JS_NewUint32(ctx, gl->getError());
}

static JSValue js_flush(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    auto* gl = getCtx(this_val); if (gl) gl->flush();
    return JS_UNDEFINED;
}

static JSValue js_finish(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    auto* gl = getCtx(this_val); if (gl) gl->finish();
    return JS_UNDEFINED;
}

static JSValue js_hint(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 2) return JS_UNDEFINED;
    uint32_t t, m; JS_ToUint32(ctx, &t, argv[0]); JS_ToUint32(ctx, &m, argv[1]);
    gl->hint(t, m);
    return JS_UNDEFINED;
}

// --- Draw calls ---

static JSValue js_drawArrays(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 3) return JS_UNDEFINED;
    uint32_t mode; int first, count;
    JS_ToUint32(ctx, &mode, argv[0]); JS_ToInt32(ctx, &first, argv[1]); JS_ToInt32(ctx, &count, argv[2]);
    gl->drawArrays(mode, first, count);
    return JS_UNDEFINED;
}

static JSValue js_drawElements(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_UNDEFINED;
    uint32_t mode, type; int count; int64_t offset;
    JS_ToUint32(ctx, &mode, argv[0]); JS_ToInt32(ctx, &count, argv[1]);
    JS_ToUint32(ctx, &type, argv[2]); JS_ToInt64(ctx, &offset, argv[3]);
    gl->drawElements(mode, count, type, (GLintptr)offset);
    return JS_UNDEFINED;
}

static JSValue js_drawArraysInstanced(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 4) return JS_UNDEFINED;
    uint32_t mode; int first, count, instances;
    JS_ToUint32(ctx, &mode, argv[0]); JS_ToInt32(ctx, &first, argv[1]);
    JS_ToInt32(ctx, &count, argv[2]); JS_ToInt32(ctx, &instances, argv[3]);
    gl->drawArraysInstanced(mode, first, count, instances);
    return JS_UNDEFINED;
}

static JSValue js_drawElementsInstanced(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 5) return JS_UNDEFINED;
    uint32_t mode, type; int count, instances; int64_t offset;
    JS_ToUint32(ctx, &mode, argv[0]); JS_ToInt32(ctx, &count, argv[1]);
    JS_ToUint32(ctx, &type, argv[2]); JS_ToInt64(ctx, &offset, argv[3]);
    JS_ToInt32(ctx, &instances, argv[4]);
    gl->drawElementsInstanced(mode, count, type, (GLintptr)offset, instances);
    return JS_UNDEFINED;
}

static JSValue js_drawRangeElements(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* gl = getCtx(this_val); if (!gl || argc < 6) return JS_UNDEFINED;
    uint32_t mode, start, end, type; int count; int64_t offset;
    JS_ToUint32(ctx, &mode, argv[0]);
    JS_ToUint32(ctx, &start, argv[1]); JS_ToUint32(ctx, &end, argv[2]);
    JS_ToInt32(ctx, &count, argv[3]); JS_ToUint32(ctx, &type, argv[4]);
    JS_ToInt64(ctx, &offset, argv[5]);
    gl->drawRangeElements(mode, start, end, count, type, (GLintptr)offset);
    return JS_UNDEFINED;
}

// ===========================================================================
// Exported function list
// ===========================================================================

const JSCFunctionListEntry webgl2_state_funcs[] = {
    JS_CFUNC_DEF("viewport", 4, js_viewport),
    JS_CFUNC_DEF("scissor", 4, js_scissor),
    JS_CFUNC_DEF("clearColor", 4, js_clearColor),
    JS_CFUNC_DEF("clearDepth", 1, js_clearDepth),
    JS_CFUNC_DEF("clearStencil", 1, js_clearStencil),
    JS_CFUNC_DEF("clear", 1, js_clear),
    JS_CFUNC_DEF("enable", 1, js_enable),
    JS_CFUNC_DEF("disable", 1, js_disable),
    JS_CFUNC_DEF("isEnabled", 1, js_isEnabled),
    JS_CFUNC_DEF("depthFunc", 1, js_depthFunc),
    JS_CFUNC_DEF("depthMask", 1, js_depthMask),
    JS_CFUNC_DEF("depthRange", 2, js_depthRange),
    JS_CFUNC_DEF("blendFunc", 2, js_blendFunc),
    JS_CFUNC_DEF("blendFuncSeparate", 4, js_blendFuncSeparate),
    JS_CFUNC_DEF("blendEquation", 1, js_blendEquation),
    JS_CFUNC_DEF("blendEquationSeparate", 2, js_blendEquationSeparate),
    JS_CFUNC_DEF("blendColor", 4, js_blendColor),
    JS_CFUNC_DEF("colorMask", 4, js_colorMask),
    JS_CFUNC_DEF("stencilFunc", 3, js_stencilFunc),
    JS_CFUNC_DEF("stencilFuncSeparate", 4, js_stencilFuncSeparate),
    JS_CFUNC_DEF("stencilOp", 3, js_stencilOp),
    JS_CFUNC_DEF("stencilOpSeparate", 4, js_stencilOpSeparate),
    JS_CFUNC_DEF("stencilMask", 1, js_stencilMask),
    JS_CFUNC_DEF("stencilMaskSeparate", 2, js_stencilMaskSeparate),
    JS_CFUNC_DEF("cullFace", 1, js_cullFace),
    JS_CFUNC_DEF("frontFace", 1, js_frontFace),
    JS_CFUNC_DEF("polygonOffset", 2, js_polygonOffset),
    JS_CFUNC_DEF("lineWidth", 1, js_lineWidth),
    JS_CFUNC_DEF("pixelStorei", 2, js_pixelStorei),
    JS_CFUNC_DEF("getError", 0, js_getError),
    JS_CFUNC_DEF("flush", 0, js_flush),
    JS_CFUNC_DEF("finish", 0, js_finish),
    JS_CFUNC_DEF("hint", 2, js_hint),
    JS_CFUNC_DEF("drawArrays", 3, js_drawArrays),
    JS_CFUNC_DEF("drawElements", 4, js_drawElements),
    JS_CFUNC_DEF("drawArraysInstanced", 4, js_drawArraysInstanced),
    JS_CFUNC_DEF("drawElementsInstanced", 5, js_drawElementsInstanced),
    JS_CFUNC_DEF("drawRangeElements", 6, js_drawRangeElements),
};
const int webgl2_state_funcs_count = sizeof(webgl2_state_funcs) / sizeof(webgl2_state_funcs[0]);

} // namespace bro::js::webgl2
