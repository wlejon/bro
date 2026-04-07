#include "js/dom_bindings_internal.h"

#include <algorithm>
#include <sstream>

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
        nameStr == "length") return 0;

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

static JSClassDef js_cssstyle_class = {
    "CSSStyleDeclaration",
    nullptr,
    nullptr, nullptr,
    &js_cssstyle_exotic,
};

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

static const JSCFunctionListEntry js_cssstyle_proto_funcs[] = {
    JS_CGETSET_DEF("cssText", js_cssstyle_get_cssText, js_cssstyle_set_cssText),
    JS_CFUNC_DEF("getPropertyValue", 1, js_cssstyle_getPropertyValue),
    JS_CFUNC_DEF("setProperty",      2, js_cssstyle_setPropertyValue),
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

static std::string getComputedProperty(bro::dom::Element* el, const std::string& prop) {
    if (!el) return "";
    auto& style = el->computedStyle();
    auto it = style.find(prop);
    if (it != style.end()) return it->second;
    return "";
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
    std::string val = getComputedProperty(el, cssName);

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

static JSClassDef js_computed_class = {
    "CSSStyleDeclaration",
    nullptr, nullptr, nullptr,
    &js_computed_exotic,
};

static JSValue js_computed_getPropertyValue(JSContext* ctx,
                                            JSValueConst this_val,
                                            int argc, JSValueConst* argv)
{
    auto* el = static_cast<bro::dom::Element*>(
        JS_GetOpaque(this_val, js_computed_class_id));
    if (!el || argc < 1) return JS_NewString(ctx, "");
    std::string name = jsToStdString(ctx, argv[0]);
    return JS_NewString(ctx, getComputedProperty(el, name).c_str());
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

static JSClassDef js_tokenlist_class = {
    "DOMTokenList",
    nullptr, nullptr, nullptr, nullptr
};

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
// Registration
// ===========================================================================

void registerStyleClasses(JSRuntime* rt) {
    JS_NewClass(rt, js_cssstyle_class_id, &js_cssstyle_class);
    JS_NewClass(rt, js_computed_class_id, &js_computed_class);
    JS_NewClass(rt, js_tokenlist_class_id, &js_tokenlist_class);
}

void installStylePrototypes(JSContext* ctx) {
    JSValue css_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, css_proto, js_cssstyle_proto_funcs,
                               sizeof(js_cssstyle_proto_funcs) / sizeof(js_cssstyle_proto_funcs[0]));
    JS_SetClassProto(ctx, js_cssstyle_class_id, css_proto);

    JSValue comp_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, comp_proto, js_computed_proto_funcs,
                               sizeof(js_computed_proto_funcs) / sizeof(js_computed_proto_funcs[0]));
    JS_SetClassProto(ctx, js_computed_class_id, comp_proto);

    JSValue tl_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, tl_proto, js_tokenlist_proto_funcs,
                               sizeof(js_tokenlist_proto_funcs) / sizeof(js_tokenlist_proto_funcs[0]));
    JS_SetClassProto(ctx, js_tokenlist_class_id, tl_proto);
}

} // namespace bro::js
