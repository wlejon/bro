#include "js/crosshair_bindings.h"
#include "engine/engine.h"
#include "util/log.h"

extern "C" {
#include "quickjs.h"
}

#include <cstdlib>
#include <cstring>
#include <string>

namespace bro::js {

// ---------------------------------------------------------------------------
// Engine pointer stash
// ---------------------------------------------------------------------------

static const char* kCrosshairEngineKey = "__bro_crosshair_engine_ptr";

static engine::Engine* getEngine(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kCrosshairEngineKey);
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
// Color parsing helper — supports #RGB, #RRGGBB, #RRGGBBAA
// ---------------------------------------------------------------------------

static bool parseHexColor(const char* str, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    if (!str || str[0] != '#') return false;
    str++; // skip '#'
    size_t len = strlen(str);
    unsigned long val = strtoul(str, nullptr, 16);
    if (len == 3) {
        r = static_cast<uint8_t>(((val >> 8) & 0xF) * 17);
        g = static_cast<uint8_t>(((val >> 4) & 0xF) * 17);
        b = static_cast<uint8_t>((val & 0xF) * 17);
        a = 255;
        return true;
    } else if (len == 6) {
        r = static_cast<uint8_t>((val >> 16) & 0xFF);
        g = static_cast<uint8_t>((val >> 8) & 0xFF);
        b = static_cast<uint8_t>(val & 0xFF);
        a = 255;
        return true;
    } else if (len == 8) {
        r = static_cast<uint8_t>((val >> 24) & 0xFF);
        g = static_cast<uint8_t>((val >> 16) & 0xFF);
        b = static_cast<uint8_t>((val >> 8) & 0xFF);
        a = static_cast<uint8_t>(val & 0xFF);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// JS functions
// ---------------------------------------------------------------------------

static JSValue js_crosshair_show(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* eng = getEngine(ctx);
    if (!eng) return JS_UNDEFINED;
    eng->crosshair().visible = true;
    return JS_UNDEFINED;
}

static JSValue js_crosshair_hide(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* eng = getEngine(ctx);
    if (!eng) return JS_UNDEFINED;
    eng->crosshair().visible = false;
    return JS_UNDEFINED;
}

static JSValue js_crosshair_get_visible(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* eng = getEngine(ctx);
    if (!eng) return JS_FALSE;
    return JS_NewBool(ctx, eng->crosshair().visible);
}

static JSValue js_crosshair_configure(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "crosshair.configure() requires an options object");

    auto* eng = getEngine(ctx);
    if (!eng) return JS_UNDEFINED;
    auto& ch = eng->crosshair();
    JSValue opts = argv[0];

    // style: 'cross' | 'dot' | 'circle' | 'crossdot'
    JSValue styleVal = JS_GetPropertyStr(ctx, opts, "style");
    if (JS_IsString(styleVal)) {
        const char* s = JS_ToCString(ctx, styleVal);
        if (s) {
            if (strcmp(s, "cross") == 0)    ch.style = engine::CrosshairConfig::Cross;
            else if (strcmp(s, "dot") == 0) ch.style = engine::CrosshairConfig::Dot;
            else if (strcmp(s, "circle") == 0) ch.style = engine::CrosshairConfig::Circle;
            else if (strcmp(s, "crossdot") == 0) ch.style = engine::CrosshairConfig::CrossDot;
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, styleVal);

    // Numeric properties
    auto readFloat = [&](const char* name, float& out) {
        JSValue v = JS_GetPropertyStr(ctx, opts, name);
        if (JS_IsNumber(v)) {
            double d;
            JS_ToFloat64(ctx, &d, v);
            out = static_cast<float>(d);
        }
        JS_FreeValue(ctx, v);
    };

    readFloat("size", ch.size);
    readFloat("thickness", ch.thickness);
    readFloat("gap", ch.gap);
    readFloat("dotSize", ch.dotSize);
    readFloat("outlineThickness", ch.outlineThickness);

    // outline: bool
    JSValue outlineVal = JS_GetPropertyStr(ctx, opts, "outline");
    if (JS_IsBool(outlineVal)) {
        ch.outline = JS_ToBool(ctx, outlineVal);
    }
    JS_FreeValue(ctx, outlineVal);

    // color: '#RRGGBB' or '#RRGGBBAA'
    JSValue colorVal = JS_GetPropertyStr(ctx, opts, "color");
    if (JS_IsString(colorVal)) {
        const char* s = JS_ToCString(ctx, colorVal);
        if (s) {
            parseHexColor(s, ch.r, ch.g, ch.b, ch.a);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, colorVal);

    // opacity: 0-1 (applied to alpha)
    JSValue opacityVal = JS_GetPropertyStr(ctx, opts, "opacity");
    if (JS_IsNumber(opacityVal)) {
        double op;
        JS_ToFloat64(ctx, &op, opacityVal);
        if (op < 0.0) op = 0.0;
        if (op > 1.0) op = 1.0;
        ch.a = static_cast<uint8_t>(op * 255.0);
    }
    JS_FreeValue(ctx, opacityVal);

    // outlineColor: '#RRGGBB'
    JSValue outColorVal = JS_GetPropertyStr(ctx, opts, "outlineColor");
    if (JS_IsString(outColorVal)) {
        const char* s = JS_ToCString(ctx, outColorVal);
        if (s) {
            parseHexColor(s, ch.outR, ch.outG, ch.outB, ch.outA);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, outColorVal);

    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_crosshair_funcs[] = {
    JS_CFUNC_DEF("show", 0, js_crosshair_show),
    JS_CFUNC_DEF("hide", 0, js_crosshair_hide),
    JS_CFUNC_DEF("configure", 1, js_crosshair_configure),
};

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void CrosshairBindings::install(JSContext* ctx, engine::Engine* engine) {
    // Stash engine pointer
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, kCrosshairEngineKey,
                      JS_NewInt64(ctx, static_cast<int64_t>(
                          reinterpret_cast<intptr_t>(engine))));

    // Get or create bro object
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue chObj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, chObj, js_crosshair_funcs,
                               sizeof(js_crosshair_funcs) / sizeof(js_crosshair_funcs[0]));

    // visible getter
    JSAtom visibleAtom = JS_NewAtom(ctx, "visible");
    JS_DefinePropertyGetSet(ctx, chObj, visibleAtom,
        JS_NewCFunction(ctx, js_crosshair_get_visible, "get visible", 0),
        JS_UNDEFINED,
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, visibleAtom);

    JS_SetPropertyStr(ctx, broObj, "crosshair", chObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
