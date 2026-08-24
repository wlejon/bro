#if BRO_WITH_TEXT_SHAPING

#include "js/text_bindings.h"
#include "engine/engine.h"
#include "render/bidi.h"
#include "render/renderer.h"
#include "render/shaped_run.h"
#include <qjsbind/qjsbind.h>
#include <string>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

namespace {

render::Renderer* rendererOf(engine::Engine* eng) {
    return eng ? eng->renderer() : nullptr;
}

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

JSValue caretToJs(JSContext* ctx, const render::ShapedRun::Caret& c) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, c.x));
    JS_SetPropertyStr(ctx, o, "isLeadingEdge", JS_NewBool(ctx, c.isLeadingEdge));
    return o;
}

} // namespace

// ---------------------------------------------------------------------------
// Engine pointer stash (no pinned JSValues, no finalizer-order hazard).
// ---------------------------------------------------------------------------

static const char* kTextEngineKey = "__bro_text_engine_ptr";

static engine::Engine* getEngine(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kTextEngineKey);
    engine::Engine* e = nullptr;
    if (JS_IsNumber(val)) {
        int64_t ptr = 0;
        JS_ToInt64(ctx, &ptr, val);
        e = reinterpret_cast<engine::Engine*>(static_cast<intptr_t>(ptr));
    }
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, global);
    return e;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

static JSValue js_text_get_bidi_available(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* eng = getEngine(ctx);
    return JS_NewBool(ctx, render::bidi::available());
}

static JSValue js_text_shape(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* eng = getEngine(ctx);
    std::string text;
    Opts opts;
    const render::ShapedRun* run = shapeArgs(ctx, eng, argc, argv, text, opts);
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

static JSValue js_text_byte_offset_to_x(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* eng = getEngine(ctx);
    std::string text;
    Opts opts;
    const render::ShapedRun* run = shapeArgs(ctx, eng, argc, argv, text, opts);
    if (!run) return JS_NULL;
    int32_t off = 0;
    if (argc > 2) JS_ToInt32(ctx, &off, argv[2]);
    auto pos = run->byteOffsetToX(static_cast<std::size_t>(off < 0 ? 0 : off), opts.spacing);
    JSValue out = caretToJs(ctx, pos.primary);
    if (pos.hasSecondary) {
        JS_SetPropertyStr(ctx, out, "secondary", caretToJs(ctx, pos.secondary));
    }
    return out;
}

static JSValue js_text_x_to_byte_offset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* eng = getEngine(ctx);
    std::string text;
    Opts opts;
    const render::ShapedRun* run = shapeArgs(ctx, eng, argc, argv, text, opts);
    if (!run) return JS_NULL;
    double x = 0;
    if (argc > 2) JS_ToFloat64(ctx, &x, argv[2]);
    return JS_NewInt32(ctx, static_cast<int>(
        run->xToByteOffset(static_cast<float>(x), opts.spacing)));
}

static JSValue js_text_cluster_range(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* eng = getEngine(ctx);
    std::string text;
    Opts opts;
    const render::ShapedRun* run = shapeArgs(ctx, eng, argc, argv, text, opts);
    if (!run) return JS_NULL;
    int32_t off = 0;
    if (argc > 2) JS_ToInt32(ctx, &off, argv[2]);
    auto span = run->clusterRange(static_cast<std::size_t>(off < 0 ? 0 : off));
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "start", JS_NewInt32(ctx, static_cast<int>(span.byteStart)));
    JS_SetPropertyStr(ctx, out, "end", JS_NewInt32(ctx, static_cast<int>(span.byteEnd)));
    return out;
}

static JSValue js_text_cache_stats(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* eng = getEngine(ctx);
    render::Renderer* r = rendererOf(eng);
    render::TextShapingEngine* te = r ? r->textEngine() : nullptr;
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "hits",
                      JS_NewInt64(ctx, te ? static_cast<int64_t>(te->hits()) : 0));
    JS_SetPropertyStr(ctx, out, "misses",
                      JS_NewInt64(ctx, te ? static_cast<int64_t>(te->misses()) : 0));
    return out;
}

static JSValue js_text_bidi(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    const std::string text = qjsbind::Convert<std::string>::from_js(ctx, argv[0]);

    render::bidi::BaseDirection base = render::bidi::BaseDirection::Auto;
    if (argc > 1 && JS_IsString(argv[1])) {
        const std::string b = qjsbind::Convert<std::string>::from_js(ctx, argv[1]);
        if (b == "ltr") base = render::bidi::BaseDirection::LTR;
        else if (b == "rtl") base = render::bidi::BaseDirection::RTL;
    }
    render::bidi::Override ov = render::bidi::Override::Normal;
    if (argc > 2 && JS_ToBool(ctx, argv[2])) ov = render::bidi::Override::Override;

    const render::bidi::Paragraph para =
        render::bidi::resolveParagraph(text, base, ov);

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "paragraphLevel",
                      JS_NewInt32(ctx, para.paragraphLevel));
    JS_SetPropertyStr(ctx, out, "uniform", JS_NewBool(ctx, para.uniform));

    JSValue levels = JS_NewArray(ctx);
    uint32_t li = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
        JS_SetPropertyUint32(ctx, levels, li++,
                             JS_NewInt32(ctx, para.levels[i]));
    }
    JS_SetPropertyStr(ctx, out, "levels", levels);

    JSValue runs = JS_NewArray(ctx);
    uint32_t ri = 0;
    for (const auto& r : para.runs()) {
        JSValue e = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, e, "start", JS_NewInt32(ctx, static_cast<int>(r.start)));
        JS_SetPropertyStr(ctx, e, "end", JS_NewInt32(ctx, static_cast<int>(r.end)));
        JS_SetPropertyStr(ctx, e, "level", JS_NewInt32(ctx, r.level));
        JS_SetPropertyUint32(ctx, runs, ri++, e);
    }
    JS_SetPropertyStr(ctx, out, "runs", runs);
    return out;
}

static JSValue js_text_bidi_reorder(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsArray(argv[0])) return JS_NULL;
    uint32_t n = static_cast<uint32_t>(
        qjsbind::get_prop_int(ctx, argv[0], "length", 0));
    std::vector<render::bidi::Level> levels;
    levels.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        JSValue v = JS_GetPropertyUint32(ctx, argv[0], i);
        int32_t lv = 0;
        JS_ToInt32(ctx, &lv, v);
        JS_FreeValue(ctx, v);
        levels.push_back(static_cast<render::bidi::Level>(lv < 0 ? 0 : lv));
    }
    const std::vector<int32_t> order = render::bidi::reorderVisual(levels);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < order.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, order[i]));
    }
    return arr;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installTextBindings(JSContext* ctx, engine::Engine* engine) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, kTextEngineKey,
                      JS_NewInt64(ctx, static_cast<int64_t>(
                          reinterpret_cast<intptr_t>(engine))));

    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue textObj = JS_NewObject(ctx);

    auto defineGetSet = [&](const char* name, JSCFunction* getter,
                            JSCFunction* setter) {
        JSAtom atom = JS_NewAtom(ctx, name);
        JS_DefinePropertyGetSet(ctx, textObj, atom,
            JS_NewCFunction(ctx, getter, name, 0),
            setter ? JS_NewCFunction(ctx, setter, name, 1) : JS_UNDEFINED,
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
    };
    defineGetSet("bidiAvailable",  js_text_get_bidi_available,  nullptr);
    JS_SetPropertyStr(ctx, textObj, "shape",
        JS_NewCFunction(ctx, js_text_shape, "shape", 2));
    JS_SetPropertyStr(ctx, textObj, "byteOffsetToX",
        JS_NewCFunction(ctx, js_text_byte_offset_to_x, "byteOffsetToX", 3));
    JS_SetPropertyStr(ctx, textObj, "xToByteOffset",
        JS_NewCFunction(ctx, js_text_x_to_byte_offset, "xToByteOffset", 3));
    JS_SetPropertyStr(ctx, textObj, "clusterRange",
        JS_NewCFunction(ctx, js_text_cluster_range, "clusterRange", 3));
    JS_SetPropertyStr(ctx, textObj, "cacheStats",
        JS_NewCFunction(ctx, js_text_cache_stats, "cacheStats", 0));
    JS_SetPropertyStr(ctx, textObj, "bidi",
        JS_NewCFunction(ctx, js_text_bidi, "bidi", 3));
    JS_SetPropertyStr(ctx, textObj, "bidiReorder",
        JS_NewCFunction(ctx, js_text_bidi_reorder, "bidiReorder", 1));

    JS_SetPropertyStr(ctx, broObj, "text", textObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}


} // namespace bro::js

#endif // BRO_WITH_TEXT_SHAPING
