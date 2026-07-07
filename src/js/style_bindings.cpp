#include "js/dom_bindings_internal.h"
#include "css/properties.h"
#include "css/color.h"
#include "layout/formatting_context.h"
#include "layout/skia_text_metrics.h"
#include "engine/engine.h"

#include <qjsbind/qjsbind.h>

#include <algorithm>
#include <sstream>
#include <cstdio>
#include <cctype>

namespace bro::js {

// ===========================================================================
// CSSStyleDeclaration wrapper (wraps bro::dom::StyleProxy)
// ===========================================================================

static int js_cssstyle_get_own_property(JSContext* ctx,
                                        JSPropertyDescriptor* desc,
                                        JSValueConst obj, JSAtom prop)
{
    auto* style = static_cast<bro::dom::StyleProxy*>(
        JS_GetOpaque(obj, js_cssstyle_class_id));
    if (!style) return 0;

    const char* name = JS_AtomToCString(ctx, prop);
    if (!name) return 0;

    std::string nameStr(name);
    JS_FreeCString(ctx, name);

    // Skip exotic lookup for properties handled by the prototype function list.
    if (nameStr == "cssText" || nameStr == "setProperty" ||
        nameStr == "getPropertyValue" || nameStr == "removeProperty" ||
        nameStr == "length" || nameStr == "item") return 0;

    std::string cssName = camelToKebab(nameStr);
    std::string val = style->getProperty(cssName);

    if (desc) {
        desc->flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE;
        desc->value = JS_NewString(ctx, val.c_str());
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    }
    return 1;
}

static int js_cssstyle_set_property(JSContext* ctx, JSValueConst obj,
                                    JSAtom prop, JSValueConst val,
                                    JSValueConst /*receiver*/, int /*flags*/)
{
    auto* style = static_cast<bro::dom::StyleProxy*>(
        JS_GetOpaque(obj, js_cssstyle_class_id));
    if (!style) return -1;

    const char* name = JS_AtomToCString(ctx, prop);
    if (!name) return -1;

    std::string nameStr(name);
    JS_FreeCString(ctx, name);

    if (nameStr == "cssText") {
        std::string text = jsToStdString(ctx, val);
        style->setCssText(text);
        return 1;
    }

    std::string cssName = camelToKebab(nameStr);
    std::string value   = jsToStdString(ctx, val);
    if (value.empty()) {
        style->removeProperty(cssName);
    } else {
        style->setProperty(cssName, value);
    }
    return 1;
}

static JSClassExoticMethods js_cssstyle_exotic = {
    js_cssstyle_get_own_property,
    nullptr, nullptr, nullptr, nullptr, nullptr,
    js_cssstyle_set_property,
};

// JSClassDef replaced by qjsbind::Class in installStyleBindings()

static JSValue js_cssstyle_get_cssText(JSContext* ctx, JSValueConst this_val)
{
    auto* style = static_cast<bro::dom::StyleProxy*>(
        JS_GetOpaque(this_val, js_cssstyle_class_id));
    if (!style) return JS_UNDEFINED;
    return JS_NewString(ctx, style->cssText().c_str());
}

static JSValue js_cssstyle_set_cssText(JSContext* ctx, JSValueConst this_val,
                                       JSValueConst val)
{
    auto* style = static_cast<bro::dom::StyleProxy*>(
        JS_GetOpaque(this_val, js_cssstyle_class_id));
    if (!style) return JS_UNDEFINED;
    style->setCssText(jsToStdString(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_cssstyle_getPropertyValue(JSContext* ctx,
                                            JSValueConst this_val,
                                            int argc, JSValueConst* argv)
{
    auto* style = static_cast<bro::dom::StyleProxy*>(
        JS_GetOpaque(this_val, js_cssstyle_class_id));
    if (!style || argc < 1) return JS_UNDEFINED;
    std::string name = jsToStdString(ctx, argv[0]);
    return JS_NewString(ctx, style->getProperty(name).c_str());
}

static JSValue js_cssstyle_setPropertyValue(JSContext* ctx,
                                            JSValueConst this_val,
                                            int argc, JSValueConst* argv)
{
    auto* style = static_cast<bro::dom::StyleProxy*>(
        JS_GetOpaque(this_val, js_cssstyle_class_id));
    if (!style || argc < 2) return JS_UNDEFINED;
    std::string name  = jsToStdString(ctx, argv[0]);
    std::string value = jsToStdString(ctx, argv[1]);
    style->setProperty(name, value);
    return JS_UNDEFINED;
}

static JSValue js_cssstyle_removeProperty(JSContext* ctx,
                                          JSValueConst this_val,
                                          int argc, JSValueConst* argv)
{
    auto* style = static_cast<bro::dom::StyleProxy*>(
        JS_GetOpaque(this_val, js_cssstyle_class_id));
    if (!style || argc < 1) return JS_NewString(ctx, "");
    std::string name = jsToStdString(ctx, argv[0]);
    std::string prev = style->getProperty(name);
    style->removeProperty(name);
    return JS_NewString(ctx, prev.c_str());
}

static JSValue js_cssstyle_get_length(JSContext* ctx, JSValueConst this_val)
{
    auto* style = static_cast<bro::dom::StyleProxy*>(
        JS_GetOpaque(this_val, js_cssstyle_class_id));
    if (!style) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, static_cast<int32_t>(style->size()));
}

static JSValue js_cssstyle_item(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv)
{
    auto* style = static_cast<bro::dom::StyleProxy*>(
        JS_GetOpaque(this_val, js_cssstyle_class_id));
    if (!style || argc < 1) return JS_NewString(ctx, "");
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    if (idx < 0 || static_cast<size_t>(idx) >= style->size())
        return JS_NewString(ctx, "");
    // unordered_map iteration order is implementation-defined but stable
    // between mutations — good enough for per-spec item() callers that
    // typically loop 0..length-1.
    auto it = style->properties().begin();
    std::advance(it, idx);
    return JS_NewString(ctx, it->first.c_str());
}

static const JSCFunctionListEntry js_cssstyle_proto_funcs[] = {
    JS_CGETSET_DEF("cssText", js_cssstyle_get_cssText, js_cssstyle_set_cssText),
    JS_CGETSET_DEF("length",  js_cssstyle_get_length,  nullptr),
    JS_CFUNC_DEF("getPropertyValue", 1, js_cssstyle_getPropertyValue),
    JS_CFUNC_DEF("setProperty",      2, js_cssstyle_setPropertyValue),
    JS_CFUNC_DEF("removeProperty",   1, js_cssstyle_removeProperty),
    JS_CFUNC_DEF("item",             1, js_cssstyle_item),
};

JSValue wrapStyleProxy(JSContext* ctx, bro::dom::StyleProxy* style)
{
    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_cssstyle_class_id));
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, style);
    return obj;
}

// ===========================================================================
// ComputedStyleDeclaration (read-only, backed by element's computedStyle map)
// ===========================================================================

// Is this a CSS property whose computed value should be resolved to rgb()?
static bool isColorProperty(const std::string& prop) {
    return prop == "color" || prop == "background-color" ||
           prop == "border-top-color" || prop == "border-right-color" ||
           prop == "border-bottom-color" || prop == "border-left-color" ||
           prop == "outline-color";
}

// Resolve a CSS color value to rgb(r, g, b) or rgba(r, g, b, a) notation.
// Returns the original value if it's already in rgb()/rgba() form or can't be parsed.
static std::string resolveColorToRgb(const std::string& value) {
    if (value.empty() || value == "transparent")
        return "rgba(0, 0, 0, 0)";
    if (value == "currentcolor" || value == "currentColor" || value == "inherit")
        return value;

    auto c = htmlayout::css::parseColor(value);
    // parseColor returns {0,0,0,0} for unrecognized — if alpha is 0 and input
    // wasn't "transparent", it likely failed to parse
    if (c.a == 0 && c.r == 0 && c.g == 0 && c.b == 0 && value != "transparent")
        return value;

    if (c.a == 255) {
        char buf[32];
        snprintf(buf, sizeof(buf), "rgb(%d, %d, %d)", c.r, c.g, c.b);
        return buf;
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "rgba(%d, %d, %d, %.2g)",
                 c.r, c.g, c.b, c.a / 255.0);
        return buf;
    }
}

static bro::engine::Engine* getEngineFromCtx(JSContext* ctx) {
    if (!ctx) return nullptr;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, "__bro_engine_ptr");
    bro::engine::Engine* e = nullptr;
    if (JS_IsNumber(val)) {
        int64_t ptr = 0;
        JS_ToInt64(ctx, &ptr, val);
        e = reinterpret_cast<bro::engine::Engine*>(static_cast<intptr_t>(ptr));
    }
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, global);
    return e;
}

static std::string getComputedProperty(JSContext* ctx, bro::dom::Element* el, const std::string& prop) {
    if (!el) return "";
    auto& style = el->computedStyle();
    std::string value;
    auto it = style.find(prop);
    if (it != style.end()) {
        value = it->second;
    } else {
        // Fall back to CSS initial value for known properties
        value = htmlayout::css::initialValue(prop);
    }

    // Resolve gap shorthand from longhands if not directly present
    if (prop == "gap" && (value == "normal" || value.empty())) {
        auto rg = style.find("row-gap");
        auto cg = style.find("column-gap");
        if (rg != style.end() && cg != style.end()) {
            if (rg->second == cg->second)
                return rg->second;
            return rg->second + " " + cg->second;
        } else if (rg != style.end()) {
            return rg->second;
        } else if (cg != style.end()) {
            return cg->second;
        }
    }

    // Resolve width/height to used values from layout box (matches Chrome
    // getComputedStyle). Always pull from the layout box rather than the
    // specified value so:
    //   - `box-sizing: border-box` reports the border-box dimension
    //     (Chrome behavior; CSSOM spec is content-box but Chrome diverges).
    //   - `auto` resolves to the used pixel size.
    //   - non-replaced inline elements still report "auto" since they have no
    //     CSS-meaningful width/height.
    if (prop == "width" || prop == "height") {
        // SVG geometry presentation attributes: SVG2 makes width/height on
        // shapes (rect, image, use, foreignObject…) real CSS geometry
        // properties, and Chromium's getComputedStyle reports them as px
        // lengths sourced from the attribute. bro renders SVG subtrees via
        // SkSVGDOM — the children have no layout boxes — so resolve straight
        // from the attribute for any element inside an <svg>.
        {
            bool insideSvg = false;
            for (auto* p = el->parentElement(); p; p = p->parentElement()) {
                const std::string& t = p->tagName();
                if (t == "svg" || t == "SVG") { insideSvg = true; break; }
            }
            if (insideSvg) {
                // Author CSS beats the presentation attribute in the
                // cascade; only fall back to the attribute when the
                // cascade left the property at its initial value.
                if (!value.empty() && value != "auto") return value;
                const std::string& attr = el->getAttribute(prop);
                if (!attr.empty()) {
                    char* end = nullptr;
                    double n = std::strtod(attr.c_str(), &end);
                    if (end != attr.c_str() &&
                        (*end == '\0' || std::string_view(end) == "px")) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%.6gpx", n);
                        return buf;
                    }
                    // Non-px units (%, em…): report the attribute as
                    // specified — no SVG viewport resolution here.
                    return attr;
                }
                // No geometry attribute: the property's initial value.
                return value.empty() ? std::string("auto") : value;
            }
        }
        auto dIt = style.find("display");
        std::string disp = (dIt != style.end()) ? dIt->second : "inline";
        const std::string& tag = el->tagName();
        bool isReplaced =
            tag == "img" || tag == "IMG" ||
            tag == "video" || tag == "VIDEO" ||
            tag == "canvas" || tag == "CANVAS" ||
            tag == "iframe" || tag == "IFRAME" ||
            tag == "input" || tag == "INPUT" ||
            tag == "embed" || tag == "EMBED" ||
            tag == "object" || tag == "OBJECT" ||
            tag == "svg" || tag == "SVG";
        if (disp == "inline" && !isReplaced) {
            return "auto";
        }
        // display:none — Chrome returns the specified value, not the laid-out
        // (zero) used value.
        if (disp == "none") {
            return value.empty() ? std::string("auto") : value;
        }
        auto& box = el->layoutBox();
        float v = (prop == "width")
            ? box.contentRect.width
            : box.contentRect.height;
        // For border-box, include padding + border on the relevant axis so
        // getComputedStyle reflects the box-sizing-aware dimension Chrome
        // exposes (and that broparity expects).
        auto bsIt = style.find("box-sizing");
        if (bsIt != style.end() && bsIt->second == "border-box") {
            if (prop == "width")
                v += box.padding.left + box.padding.right
                   + box.border.left + box.border.right;
            else
                v += box.padding.top + box.padding.bottom
                   + box.border.top + box.border.bottom;
        }
        if (v >= 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.6gpx", v);
            return buf;
        }
    }

    // Resolve padding-* and margin-* to pixel values when the specified value
    // is `auto` or a percentage. Chrome reports these as resolved px while
    // length values stay as specified (so margin-collapse adjustments don't
    // leak into getComputedStyle output).
    if (prop == "padding-top" || prop == "padding-right" ||
        prop == "padding-bottom" || prop == "padding-left" ||
        prop == "margin-top" || prop == "margin-right" ||
        prop == "margin-bottom" || prop == "margin-left") {
        bool isPct = !value.empty() && value.back() == '%';
        bool isAuto = value == "auto";
        if (isPct || isAuto) {
            auto& box = el->layoutBox();
            float v = 0;
            if (prop == "padding-top")    v = box.padding.top;
            else if (prop == "padding-right")  v = box.padding.right;
            else if (prop == "padding-bottom") v = box.padding.bottom;
            else if (prop == "padding-left")   v = box.padding.left;
            else if (prop == "margin-top")     v = box.margin.top;
            else if (prop == "margin-right")   v = box.margin.right;
            else if (prop == "margin-bottom")  v = box.margin.bottom;
            else if (prop == "margin-left")    v = box.margin.left;
            char buf[32];
            snprintf(buf, sizeof(buf), "%.6gpx", v);
            return buf;
        }
        // Absolutize font-relative and absolute-unit lengths (em, rem, pt…)
        // to px: the computed value of a margin/padding <length> is always
        // absolute (Chromium reports e.g. `margin: 1em` as "16px").
        bool endsPx = value.size() > 2 &&
            value.compare(value.size() - 2, 2, "px") == 0;
        bool hasAlphaUnit = !value.empty() &&
            std::isalpha(static_cast<unsigned char>(value.back()));
        if (hasAlphaUnit && !endsPx) {
            std::string fs;
            auto fsIt = style.find("font-size");
            if (fsIt != style.end()) fs = fsIt->second;
            float fontSize = htmlayout::layout::resolveLength(fs, 16.0f, 16.0f);
            if (fontSize <= 0) fontSize = 16.0f;
            float v = htmlayout::layout::resolveLength(value, 0.0f, fontSize);
            char buf[32];
            snprintf(buf, sizeof(buf), "%.6gpx", v);
            return buf;
        }
    }

    // Resolve line-height to used px value (matches browser getComputedStyle behavior).
    // Per CSSOM, the resolved value of line-height is the used value in px,
    // except for "normal" which serializes as "normal".
    if (prop == "line-height" && !value.empty() && value != "normal") {
        // Get font-size from the same computed style (may itself need resolution)
        std::string fs;
        auto fsIt = style.find("font-size");
        if (fsIt != style.end()) fs = fsIt->second;
        float fontSize = htmlayout::layout::resolveLength(fs, 16.0f, 16.0f);
        if (fontSize <= 0) fontSize = 16.0f;

        bro::layout::SkiaTextMetrics* tm = nullptr;
        auto* eng = getEngineFromCtx(ctx);
        if (eng) tm = eng->textMetrics();

        std::string ff, fw;
        auto ffIt = style.find("font-family");
        if (ffIt != style.end()) ff = ffIt->second;
        auto fwIt = style.find("font-weight");
        if (fwIt != style.end()) fw = fwIt->second;

        float lh = tm
            ? htmlayout::layout::resolveLineHeight(value, fontSize, ff, fw, *tm)
            : htmlayout::layout::resolveLineHeight(value, fontSize);
        if (lh > 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.4gpx", lh);
            return buf;
        }
    }

    // Resolve colors to rgb() notation (matches browser getComputedStyle behavior)
    if (isColorProperty(prop))
        value = resolveColorToRgb(value);

    // Resolve font-weight keywords to numeric (matches Chrome)
    if (prop == "font-weight") {
        if (value == "normal") return "400";
        if (value == "bold") return "700";
    }

    // Resolve border-width to 0px when border-style is none
    if ((prop == "border-top-width" || prop == "border-right-width" ||
         prop == "border-bottom-width" || prop == "border-left-width") &&
        (value == "medium" || value == "thin" || value == "thick")) {
        // Check if the corresponding border-style is none
        std::string sideProp = prop.substr(0, prop.rfind("-width")) + "-style";
        auto sit = style.find(sideProp);
        std::string borderStyle = (sit != style.end()) ? sit->second : "none";
        if (borderStyle == "none" || borderStyle == "hidden")
            return "0px";
    }

    return value;
}

static int js_computed_get_own_property(JSContext* ctx,
                                        JSPropertyDescriptor* desc,
                                        JSValueConst obj, JSAtom prop)
{
    auto* el = static_cast<bro::dom::Element*>(
        JS_GetOpaque(obj, js_computed_class_id));
    if (!el) return 0;

    const char* name = JS_AtomToCString(ctx, prop);
    if (!name) return 0;
    std::string nameStr(name);
    JS_FreeCString(ctx, name);

    if (nameStr.empty()) return 0;

    static const char* skip[] = {
        "getPropertyValue", "setProperty", "length", "cssText",
        "toString", "valueOf", "constructor", "toJSON", "then",
        "toLocaleString", "hasOwnProperty", "isPrototypeOf",
        "propertyIsEnumerable", "__proto__", "__defineGetter__",
        "__defineSetter__", "__lookupGetter__", "__lookupSetter__",
        nullptr
    };
    for (const char** s = skip; *s; ++s) {
        if (nameStr == *s) return 0;
    }

    char first = nameStr[0];
    if (!(first >= 'a' && first <= 'z')) return 0;

    std::string cssName = camelToKebab(nameStr);
    std::string val = getComputedProperty(ctx, el, cssName);

    if (desc) {
        desc->flags = JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE;
        desc->value = JS_NewString(ctx, val.c_str());
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    }
    return 1;
}

static JSClassExoticMethods js_computed_exotic = {
    js_computed_get_own_property,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

// JSClassDef replaced by qjsbind::Class in installStyleBindings()

static JSValue js_computed_getPropertyValue(JSContext* ctx,
                                            JSValueConst this_val,
                                            int argc, JSValueConst* argv)
{
    auto* el = static_cast<bro::dom::Element*>(
        JS_GetOpaque(this_val, js_computed_class_id));
    if (!el || argc < 1) return JS_NewString(ctx, "");
    std::string name = jsToStdString(ctx, argv[0]);
    return JS_NewString(ctx, getComputedProperty(ctx, el, name).c_str());
}

static JSValue js_computed_setProperty(JSContext* ctx,
                                       JSValueConst /*this_val*/,
                                       int /*argc*/, JSValueConst* /*argv*/)
{
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_computed_proto_funcs[] = {
    JS_CFUNC_DEF("getPropertyValue", 1, js_computed_getPropertyValue),
    JS_CFUNC_DEF("setProperty",      2, js_computed_setProperty),
};

JSValue js_window_getComputedStyle(JSContext* ctx,
                                   JSValueConst /*this_val*/,
                                   int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_NULL;

    auto* el = static_cast<bro::dom::Element*>(
        JS_GetOpaque(argv[0], js_element_class_id));

    if (!el) {
        JSValue obj = JS_NewObject(ctx);
        JSValue fn = JS_NewCFunction(ctx, [](JSContext* c, JSValueConst, int, JSValueConst*) -> JSValue {
            return JS_NewString(c, "");
        }, "getPropertyValue", 1);
        JS_SetPropertyStr(ctx, obj, "getPropertyValue", fn);
        return obj;
    }

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_computed_class_id));
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, el);
    return obj;
}

// ===========================================================================
// DOMTokenList wrapper (classList)
// ===========================================================================

// JSClassDef replaced by qjsbind::Class in installStyleBindings()

static inline bro::dom::Element* getTokenListElement(JSValueConst val) {
    return static_cast<bro::dom::Element*>(
        JS_GetOpaque(val, js_tokenlist_class_id));
}

static std::vector<std::string> splitClasses(const std::string& cls) {
    std::vector<std::string> tokens;
    std::istringstream iss(cls);
    std::string tok;
    while (iss >> tok) tokens.push_back(tok);
    return tokens;
}

static std::string joinClasses(const std::vector<std::string>& tokens) {
    std::string result;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) result += ' ';
        result += tokens[i];
    }
    return result;
}

static JSValue js_tokenlist_get_length(JSContext* ctx, JSValueConst this_val) {
    auto* el = getTokenListElement(this_val);
    if (!el) return JS_NewInt32(ctx, 0);
    auto tokens = splitClasses(el->className());
    return JS_NewInt32(ctx, static_cast<int32_t>(tokens.size()));
}

static JSValue js_tokenlist_get_value(JSContext* ctx, JSValueConst this_val) {
    auto* el = getTokenListElement(this_val);
    if (!el) return JS_NewString(ctx, "");
    return JS_NewString(ctx, el->className().c_str());
}

static JSValue js_tokenlist_set_value(JSContext* ctx, JSValueConst this_val,
                                      JSValueConst val) {
    auto* el = getTokenListElement(this_val);
    if (!el) return JS_UNDEFINED;
    el->setClassName(jsToStdString(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_tokenlist_item(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* el = getTokenListElement(this_val);
    if (!el || argc < 1) return JS_NULL;
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    auto tokens = splitClasses(el->className());
    if (idx < 0 || static_cast<size_t>(idx) >= tokens.size()) return JS_NULL;
    return JS_NewString(ctx, tokens[static_cast<size_t>(idx)].c_str());
}

static JSValue js_tokenlist_contains(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* el = getTokenListElement(this_val);
    if (!el || argc < 1) return JS_FALSE;
    std::string token = jsToStdString(ctx, argv[0]);
    auto tokens = splitClasses(el->className());
    for (auto& t : tokens) {
        if (t == token) return JS_TRUE;
    }
    return JS_FALSE;
}

static JSValue js_tokenlist_add(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    auto* el = getTokenListElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto tokens = splitClasses(el->className());
    bool changed = false;
    for (int i = 0; i < argc; ++i) {
        std::string token = jsToStdString(ctx, argv[i]);
        if (token.empty()) continue;
        bool found = false;
        for (auto& t : tokens) { if (t == token) { found = true; break; } }
        if (!found) { tokens.push_back(token); changed = true; }
    }
    if (changed) el->setClassName(joinClasses(tokens));
    return JS_UNDEFINED;
}

static JSValue js_tokenlist_remove(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* el = getTokenListElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto tokens = splitClasses(el->className());
    size_t origSize = tokens.size();
    for (int i = 0; i < argc; ++i) {
        std::string token = jsToStdString(ctx, argv[i]);
        tokens.erase(std::remove(tokens.begin(), tokens.end(), token), tokens.end());
    }
    if (tokens.size() != origSize) el->setClassName(joinClasses(tokens));
    return JS_UNDEFINED;
}

static JSValue js_tokenlist_toggle(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* el = getTokenListElement(this_val);
    if (!el || argc < 1) return JS_FALSE;
    std::string token = jsToStdString(ctx, argv[0]);
    auto tokens = splitClasses(el->className());

    auto it = std::find(tokens.begin(), tokens.end(), token);
    bool hasForce = (argc >= 2 && !JS_IsUndefined(argv[1]));

    if (it != tokens.end()) {
        // Token present — force=true means keep it (no change)
        if (hasForce && JS_ToBool(ctx, argv[1])) {
            return JS_TRUE;
        }
        // Remove token
        tokens.erase(it);
        el->setClassName(joinClasses(tokens));
        return JS_FALSE;
    } else {
        // Token absent — force=false means keep it absent (no change)
        if (hasForce && !JS_ToBool(ctx, argv[1])) {
            return JS_FALSE;
        }
        // Add token
        tokens.push_back(token);
        el->setClassName(joinClasses(tokens));
        return JS_TRUE;
    }
}

static JSValue js_tokenlist_replace(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    auto* el = getTokenListElement(this_val);
    if (!el || argc < 2) return JS_FALSE;
    std::string oldToken = jsToStdString(ctx, argv[0]);
    std::string newToken = jsToStdString(ctx, argv[1]);
    auto tokens = splitClasses(el->className());
    auto it = std::find(tokens.begin(), tokens.end(), oldToken);
    if (it == tokens.end()) return JS_FALSE;
    *it = newToken;
    el->setClassName(joinClasses(tokens));
    return JS_TRUE;
}

static JSValue js_tokenlist_toString(JSContext* ctx, JSValueConst this_val,
                                     int /*argc*/, JSValueConst* /*argv*/) {
    return js_tokenlist_get_value(ctx, this_val);
}

static const JSCFunctionListEntry js_tokenlist_proto_funcs[] = {
    JS_CGETSET_DEF("length", js_tokenlist_get_length, nullptr),
    JS_CGETSET_DEF("value",  js_tokenlist_get_value,  js_tokenlist_set_value),
    JS_CFUNC_DEF("item",     1, js_tokenlist_item),
    JS_CFUNC_DEF("contains", 1, js_tokenlist_contains),
    JS_CFUNC_DEF("add",      1, js_tokenlist_add),
    JS_CFUNC_DEF("remove",   1, js_tokenlist_remove),
    JS_CFUNC_DEF("toggle",   1, js_tokenlist_toggle),
    JS_CFUNC_DEF("replace",  2, js_tokenlist_replace),
    JS_CFUNC_DEF("toString", 0, js_tokenlist_toString),
};

JSValue wrapTokenList(JSContext* ctx, bro::dom::Element* elem) {
    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_tokenlist_class_id));
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, elem);
    return obj;
}

// ===========================================================================
// Tag types for qjsbind (multiple classes wrap the same C++ pointer type)
// ===========================================================================

struct CSSStyleTag {};  // tag for CSSStyleDeclaration (wraps StyleProxy*)
struct ComputedTag {};  // tag for ComputedStyleDeclaration (wraps Element*)
struct TokenListTag {}; // tag for DOMTokenList (wraps Element*)

// ===========================================================================
// Registration
// ===========================================================================

void installStyleBindings(JSContext* ctx) {
    // CSSStyleDeclaration — exotic methods for dynamic camelCase property access
    qjsbind::Class<CSSStyleTag>(ctx, "CSSStyleDeclaration",
                                 qjsbind::NoGlobal | qjsbind::NoDestructor,
                                 nullptr, &js_cssstyle_exotic)
        .function_list(js_cssstyle_proto_funcs,
                       sizeof(js_cssstyle_proto_funcs) / sizeof(js_cssstyle_proto_funcs[0]));
    js_cssstyle_class_id = qjsbind::class_id<CSSStyleTag>();

    // ComputedStyleDeclaration — exotic methods for computed style lookup
    qjsbind::Class<ComputedTag>(ctx, "CSSStyleDeclaration",
                                 qjsbind::NoGlobal | qjsbind::NoDestructor,
                                 nullptr, &js_computed_exotic)
        .function_list(js_computed_proto_funcs,
                       sizeof(js_computed_proto_funcs) / sizeof(js_computed_proto_funcs[0]));
    js_computed_class_id = qjsbind::class_id<ComputedTag>();

    // DOMTokenList
    qjsbind::Class<TokenListTag>(ctx, "DOMTokenList",
                                  qjsbind::NoGlobal | qjsbind::NoDestructor)
        .function_list(js_tokenlist_proto_funcs,
                       sizeof(js_tokenlist_proto_funcs) / sizeof(js_tokenlist_proto_funcs[0]));
    js_tokenlist_class_id = qjsbind::class_id<TokenListTag>();
}

} // namespace bro::js
