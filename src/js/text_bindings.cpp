#include "js/text_bindings.h"

#include "engine/engine.h"
#include "render/renderer.h"
#include "render/shaped_run.h"

#include <qjsbind/qjsbind.h>

#include <string>

namespace bro::js {

namespace {

render::Renderer* rendererOf(engine::Engine* eng) {
    return eng ? eng->renderer() : nullptr;
}

// Read the { family, size, weight, italic, letterSpacing, wordSpacing } options
// bag. `family` must be kept alive by the caller: FontRef holds a view.
struct Opts {
    std::string    family = "Arial";
    render::FontRef ref{};
    render::Spacing spacing{};
};

Opts readOpts(JSContext* ctx, JSValueConst v) {
    Opts o;
    if (JS_IsObject(v)) {
        std::string fam = qjsbind::get_prop_string(ctx, v, "family", "Arial");
        if (!fam.empty()) o.family = std::move(fam);
        o.ref.size   = static_cast<float>(qjsbind::get_prop_number(ctx, v, "size", 16.0));
        o.ref.weight = qjsbind::get_prop_int(ctx, v, "weight", 400);
        o.ref.italic = qjsbind::get_prop_bool(ctx, v, "italic", false);
        o.spacing.letter =
            static_cast<float>(qjsbind::get_prop_number(ctx, v, "letterSpacing", 0.0));
        o.spacing.word =
            static_cast<float>(qjsbind::get_prop_number(ctx, v, "wordSpacing", 0.0));
    } else {
        o.ref.size = 16.0f;
    }
    o.ref.family = o.family;
    return o;
}

// Shape argv[0] with argv[1]'s options. Returns null (and leaves *out unset)
// when there is no renderer or nothing to shape.
const render::ShapedRun* shapeArgs(JSContext* ctx, engine::Engine* eng,
                                   int argc, JSValueConst* argv,
                                   std::string& textOut, Opts& optsOut) {
    render::Renderer* r = rendererOf(eng);
    if (!r || argc < 1) return nullptr;
    textOut = qjsbind::Convert<std::string>::from_js(ctx, argv[0]);
    optsOut = readOpts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    optsOut.ref.family = optsOut.family;
    return r->shapeText(textOut, optsOut.ref, optsOut.spacing.letter != 0.0f);
}

JSValue js_shape(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue js_byteOffsetToX(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_xToByteOffset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_clusterRange(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_cacheStats(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);

engine::Engine* g_engine = nullptr;

JSValue js_shape(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string text;
    Opts opts;
    const render::ShapedRun* run = shapeArgs(ctx, g_engine, argc, argv, text, opts);
    if (!run) return JS_NULL;

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "text", JS_NewString(ctx, text.c_str()));
    JS_SetPropertyStr(ctx, out, "glyphCount",
                      JS_NewInt32(ctx, static_cast<int>(run->glyphCount())));
    JS_SetPropertyStr(ctx, out, "width", JS_NewFloat64(ctx, run->width(opts.spacing)));

    auto list = run->clusterList(opts.spacing);
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const auto& c : list) {
        JSValue e = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, e, "start", JS_NewInt32(ctx, static_cast<int>(c.byteStart)));
        JS_SetPropertyStr(ctx, e, "end", JS_NewInt32(ctx, static_cast<int>(c.byteEnd)));
        JS_SetPropertyStr(ctx, e, "x", JS_NewFloat64(ctx, c.x));
        JS_SetPropertyStr(ctx, e, "advance", JS_NewFloat64(ctx, c.advance));
        JS_SetPropertyStr(ctx, e, "glyphs", JS_NewInt32(ctx, static_cast<int>(c.glyphCount)));
        JS_SetPropertyStr(ctx, e, "rtl", JS_NewBool(ctx, c.rtl));
        JS_SetPropertyUint32(ctx, arr, i++, e);
    }
    JS_SetPropertyStr(ctx, out, "clusters", arr);
    return out;
}

JSValue caretToJs(JSContext* ctx, const render::ShapedRun::Caret& c) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, c.x));
    JS_SetPropertyStr(ctx, o, "isLeadingEdge", JS_NewBool(ctx, c.isLeadingEdge));
    return o;
}

JSValue js_byteOffsetToX(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string text;
    Opts opts;
    const render::ShapedRun* run = shapeArgs(ctx, g_engine, argc, argv, text, opts);
    if (!run) return JS_NULL;
    int32_t off = 0;
    if (argc > 2) JS_ToInt32(ctx, &off, argv[2]);
    auto pos = run->byteOffsetToX(static_cast<std::size_t>(off < 0 ? 0 : off), opts.spacing);
    JSValue out = caretToJs(ctx, pos.primary);
    // Present from day one so a bidi-aware caller written today keeps working
    // when chunk 4 starts filling it in.
    if (pos.hasSecondary) {
        JS_SetPropertyStr(ctx, out, "secondary", caretToJs(ctx, pos.secondary));
    }
    return out;
}

JSValue js_xToByteOffset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string text;
    Opts opts;
    const render::ShapedRun* run = shapeArgs(ctx, g_engine, argc, argv, text, opts);
    if (!run) return JS_NULL;
    double x = 0;
    if (argc > 2) JS_ToFloat64(ctx, &x, argv[2]);
    return JS_NewInt32(ctx, static_cast<int>(
        run->xToByteOffset(static_cast<float>(x), opts.spacing)));
}

JSValue js_clusterRange(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string text;
    Opts opts;
    const render::ShapedRun* run = shapeArgs(ctx, g_engine, argc, argv, text, opts);
    if (!run) return JS_NULL;
    int32_t off = 0;
    if (argc > 2) JS_ToInt32(ctx, &off, argv[2]);
    auto span = run->clusterRange(static_cast<std::size_t>(off < 0 ? 0 : off));
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "start", JS_NewInt32(ctx, static_cast<int>(span.byteStart)));
    JS_SetPropertyStr(ctx, out, "end", JS_NewInt32(ctx, static_cast<int>(span.byteEnd)));
    return out;
}

JSValue js_cacheStats(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    render::Renderer* r = rendererOf(g_engine);
    render::TextShapingEngine* te = r ? r->textEngine() : nullptr;
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "hits",
                      JS_NewInt64(ctx, te ? static_cast<int64_t>(te->hits()) : 0));
    JS_SetPropertyStr(ctx, out, "misses",
                      JS_NewInt64(ctx, te ? static_cast<int64_t>(te->misses()) : 0));
    return out;
}

}  // namespace

void installTextBindings(JSContext* ctx, engine::Engine* engine) {
    g_engine = engine;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (!JS_IsObject(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue text = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, text, "shape", JS_NewCFunction(ctx, js_shape, "shape", 2));
    JS_SetPropertyStr(ctx, text, "byteOffsetToX",
                      JS_NewCFunction(ctx, js_byteOffsetToX, "byteOffsetToX", 3));
    JS_SetPropertyStr(ctx, text, "xToByteOffset",
                      JS_NewCFunction(ctx, js_xToByteOffset, "xToByteOffset", 3));
    JS_SetPropertyStr(ctx, text, "clusterRange",
                      JS_NewCFunction(ctx, js_clusterRange, "clusterRange", 3));
    JS_SetPropertyStr(ctx, text, "cacheStats",
                      JS_NewCFunction(ctx, js_cacheStats, "cacheStats", 0));
    JS_SetPropertyStr(ctx, broObj, "text", text);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
