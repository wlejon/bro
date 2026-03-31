#include "js/custom_elements.h"
#include "js/dom_bindings.h"
#include "dom/element.h"
#include "dom/document.h"
#include "util/log.h"

#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// ---------------------------------------------------------------------------
// Per-context state
// ---------------------------------------------------------------------------

struct CustomElementDef {
    JSValue constructor;  // ref-counted
    std::vector<std::string> observedAttributes;
};

struct CERegistry {
    std::unordered_map<std::string, CustomElementDef> defs;
    JSClassID elementClassId = 0;
    bro::dom::Document* document = nullptr;
};

static std::unordered_map<JSContext*, CERegistry*> s_registries;

static CERegistry* getReg(JSContext* ctx) {
    auto it = s_registries.find(ctx);
    return (it != s_registries.end()) ? it->second : nullptr;
}

// Tag currently being constructed by createElement (for HTMLElement super() call)
static thread_local std::string s_constructingTag;
// C++ element being constructed by createElement
static thread_local bro::dom::Element* s_constructingElem = nullptr;

static std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

static bool isValidName(const std::string& name) {
    if (name.empty() || name.find('-') == std::string::npos)
        return false;
    char c0 = name[0];
    if (!std::isalpha(static_cast<unsigned char>(c0)))
        return false;
    return true;
}

// ---------------------------------------------------------------------------
// HTMLElement constructor — called via super() from user class constructors
// ---------------------------------------------------------------------------

static JSValue js_htmlelement_ctor(JSContext* ctx, JSValueConst new_target,
                                   int /*argc*/, JSValueConst* /*argv*/)
{
    auto* reg = getReg(ctx);
    if (!reg) return JS_ThrowTypeError(ctx, "No custom element registry");

    std::string tag = s_constructingTag;
    bro::dom::Element* elem = s_constructingElem;

    // If not called from createElement, reverse-lookup new_target
    if (tag.empty()) {
        for (auto& [name, def] : reg->defs) {
            if (JS_VALUE_GET_PTR(def.constructor) == JS_VALUE_GET_PTR(new_target)) {
                tag = name;
                break;
            }
        }
        if (tag.empty())
            return JS_ThrowTypeError(ctx, "Illegal constructor");
    }

    // Get prototype from new_target (correct for class inheritance)
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) return proto;

    JSValue obj = JS_NewObjectProtoClass(ctx, proto, reg->elementClassId);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;

    if (!elem) {
        // `new MyElement()` path — create C++ element
        if (!reg->document) {
            JS_FreeValue(ctx, obj);
            return JS_ThrowInternalError(ctx, "No document");
        }
        elem = reg->document->createElement(tag);
        if (!elem) {
            JS_FreeValue(ctx, obj);
            return JS_ThrowInternalError(ctx, "createElement failed");
        }
    }

    JS_SetOpaque(obj, elem);

    // Cache in __bro_elem_map
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (JS_IsUndefined(elemMap)) {
        elemMap = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__bro_elem_map", JS_DupValue(ctx, elemMap));
    }
    std::string key = std::to_string(elem->nodeId());
    JS_SetPropertyStr(ctx, elemMap, key.c_str(), JS_DupValue(ctx, obj));
    JS_FreeValue(ctx, elemMap);
    JS_FreeValue(ctx, global);

    return obj;
}

// Forward declarations for lifecycle helpers (used by define's upgrade step)
static void fireCallback(JSContext* ctx, JSValue wrapper, const char* name);

// ---------------------------------------------------------------------------
// customElements.define / get / whenDefined
// ---------------------------------------------------------------------------

static JSValue js_ce_define(JSContext* ctx, JSValueConst /*this_val*/,
                            int argc, JSValueConst* argv)
{
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "define requires name and constructor");

    const char* nameStr = JS_ToCString(ctx, argv[0]);
    if (!nameStr) return JS_EXCEPTION;
    std::string name = toLower(nameStr);
    JS_FreeCString(ctx, nameStr);

    if (!isValidName(name))
        return JS_ThrowSyntaxError(ctx, "'%s' is not a valid custom element name", name.c_str());

    if (!JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "Constructor must be a function");

    auto* reg = getReg(ctx);
    if (!reg) return JS_UNDEFINED;

    if (reg->defs.count(name))
        return JS_ThrowTypeError(ctx, "'%s' already defined", name.c_str());

    CustomElementDef def;
    def.constructor = JS_DupValue(ctx, argv[1]);

    // Read static observedAttributes
    JSValue observed = JS_GetPropertyStr(ctx, argv[1], "observedAttributes");
    if (JS_IsArray(observed)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, observed, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int32_t i = 0; i < len; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, observed, i);
            const char* s = JS_ToCString(ctx, item);
            if (s) {
                def.observedAttributes.push_back(s);
                JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, item);
        }
    }
    JS_FreeValue(ctx, observed);

    reg->defs[name] = def;

    // Upgrade existing elements in the DOM that match this tag name.
    // Per the spec, when define() is called, any existing elements with
    // that tag get upgraded: constructor runs, then connectedCallback fires.
    if (reg->document) {
        auto existing = reg->document->querySelectorAll(name);
        for (auto* elem : existing) {
            // Run the full constructor via createCustomElement.
            // This sets the prototype, creates a cached wrapper, and runs
            // user constructor code (e.g. this._count = 0).
            JSValue upgraded = createCustomElement(ctx, elem, name);
            if (!JS_IsException(upgraded) && !JS_IsUndefined(upgraded)) {
                // Fire connectedCallback since element is already in the tree
                if (elem->parentNode()) {
                    fireCallback(ctx, upgraded, "connectedCallback");
                }
                JS_FreeValue(ctx, upgraded);
            } else {
                JS_FreeValue(ctx, upgraded);
            }
        }
    }

    return JS_UNDEFINED;
}

static JSValue js_ce_get(JSContext* ctx, JSValueConst /*this_val*/,
                         int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_UNDEFINED;
    std::string name = toLower(s);
    JS_FreeCString(ctx, s);

    auto* reg = getReg(ctx);
    if (!reg) return JS_UNDEFINED;
    auto it = reg->defs.find(name);
    if (it == reg->defs.end()) return JS_UNDEFINED;
    return JS_DupValue(ctx, it->second.constructor);
}

static JSValue js_ce_whenDefined(JSContext* ctx, JSValueConst /*this_val*/,
                                 int argc, JSValueConst* argv)
{
    // Simplified: always returns a resolved promise
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue promiseCtor = JS_GetPropertyStr(ctx, global, "Promise");
    JSValue resolve = JS_GetPropertyStr(ctx, promiseCtor, "resolve");
    JSValue undef = JS_UNDEFINED;
    JSValue result = JS_Call(ctx, resolve, promiseCtor, 1, &undef);
    JS_FreeValue(ctx, resolve);
    JS_FreeValue(ctx, promiseCtor);
    JS_FreeValue(ctx, global);
    return result;
}

// ---------------------------------------------------------------------------
// Lifecycle helpers
// ---------------------------------------------------------------------------

static void fireCallback(JSContext* ctx, JSValue wrapper, const char* name) {
    JSValue method = JS_GetPropertyStr(ctx, wrapper, name);
    if (JS_IsFunction(ctx, method)) {
        JSValue ret = JS_Call(ctx, method, wrapper, 0, nullptr);
        if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(ctx);
            const char* msg = JS_ToCString(ctx, exc);
            LOG_ERROR("Custom element %s error: %s", name, msg ? msg : "unknown");
            if (msg) JS_FreeCString(ctx, msg);
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, method);
}

static void fireCallbackRecursive(JSContext* ctx, JSValue wrapper,
                                  const char* callbackName) {
    auto* reg = getReg(ctx);
    if (!reg) return;

    auto* elem = static_cast<bro::dom::Element*>(DomBindings::unwrapElement(ctx, wrapper));
    if (!elem) return;

    // Fire on this element if it's a custom element
    if (reg->defs.count(toLower(elem->tagName()))) {
        fireCallback(ctx, wrapper, callbackName);
    }

    // Recurse into child elements
    for (auto* child : elem->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element) {
            JSValue childW = DomBindings::wrapElement(ctx, child);
            fireCallbackRecursive(ctx, childW, callbackName);
            JS_FreeValue(ctx, childW);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void fireConnectedCallback(JSContext* ctx, JSValue elementWrapper) {
    fireCallbackRecursive(ctx, elementWrapper, "connectedCallback");
}

void fireDisconnectedCallback(JSContext* ctx, JSValue elementWrapper) {
    fireCallbackRecursive(ctx, elementWrapper, "disconnectedCallback");
}

void fireAttributeChangedCallback(JSContext* ctx, JSValue elementWrapper,
                                  const std::string& attrName,
                                  const std::string& oldVal,
                                  const std::string& newVal)
{
    auto* reg = getReg(ctx);
    if (!reg) return;

    auto* elem = static_cast<bro::dom::Element*>(DomBindings::unwrapElement(ctx, elementWrapper));
    if (!elem) return;

    std::string tag = toLower(elem->tagName());
    auto it = reg->defs.find(tag);
    if (it == reg->defs.end()) return;

    auto& observed = it->second.observedAttributes;
    if (std::find(observed.begin(), observed.end(), attrName) == observed.end())
        return;

    JSValue args[3];
    args[0] = JS_NewString(ctx, attrName.c_str());
    args[1] = oldVal.empty() ? JS_NULL : JS_NewString(ctx, oldVal.c_str());
    args[2] = JS_NewString(ctx, newVal.c_str());

    JSValue method = JS_GetPropertyStr(ctx, elementWrapper, "attributeChangedCallback");
    if (JS_IsFunction(ctx, method)) {
        JSValue ret = JS_Call(ctx, method, elementWrapper, 3, args);
        if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(ctx);
            const char* msg = JS_ToCString(ctx, exc);
            LOG_ERROR("attributeChangedCallback error: %s", msg ? msg : "unknown");
            if (msg) JS_FreeCString(ctx, msg);
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, method);
    for (int i = 0; i < 3; i++) JS_FreeValue(ctx, args[i]);
}

JSValue createCustomElement(JSContext* ctx, void* elemPtr, const std::string& tag) {
    auto* reg = getReg(ctx);
    if (!reg) return JS_UNDEFINED;

    std::string lower = toLower(tag);
    auto it = reg->defs.find(lower);
    if (it == reg->defs.end()) return JS_UNDEFINED;

    auto* elem = static_cast<bro::dom::Element*>(elemPtr);

    // Set thread-locals for the HTMLElement constructor
    s_constructingTag = lower;
    s_constructingElem = elem;

    JSValue result = JS_CallConstructor(ctx, it->second.constructor, 0, nullptr);

    s_constructingTag.clear();
    s_constructingElem = nullptr;

    return result;
}

bool upgradeCustomElementPrototype(JSContext* ctx, JSValue wrapper, const std::string& tagName) {
    auto* reg = getReg(ctx);
    if (!reg) return false;

    std::string lower = toLower(tagName);
    auto it = reg->defs.find(lower);
    if (it == reg->defs.end()) return false;

    JSValue customProto = JS_GetPropertyStr(ctx, it->second.constructor, "prototype");
    if (!JS_IsUndefined(customProto) && !JS_IsNull(customProto)) {
        JS_SetPrototype(ctx, wrapper, customProto);
    }
    JS_FreeValue(ctx, customProto);
    return true;
}

// ---------------------------------------------------------------------------
// Install / cleanup
// ---------------------------------------------------------------------------

void installCustomElements(JSContext* ctx, JSClassID elementClassId, void* documentPtr)
{
    auto* reg = new CERegistry();
    reg->elementClassId = elementClassId;
    reg->document = static_cast<bro::dom::Document*>(documentPtr);
    s_registries[ctx] = reg;

    JSValue global = JS_GetGlobalObject(ctx);

    // customElements object
    JSValue ce = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ce, "define",
        JS_NewCFunction(ctx, js_ce_define, "define", 2));
    JS_SetPropertyStr(ctx, ce, "get",
        JS_NewCFunction(ctx, js_ce_get, "get", 1));
    JS_SetPropertyStr(ctx, ce, "whenDefined",
        JS_NewCFunction(ctx, js_ce_whenDefined, "whenDefined", 1));
    JS_SetPropertyStr(ctx, global, "customElements", ce);

    // HTMLElement constructor — replaces the dummy stub class
    JSValue htmlCtor = JS_NewCFunction2(ctx, js_htmlelement_ctor,
                                        "HTMLElement", 0,
                                        JS_CFUNC_constructor, 0);

    // HTMLElement.prototype inherits from Element prototype
    JSValue elemProto = JS_GetClassProto(ctx, elementClassId);
    JSValue htmlProto = JS_NewObjectProto(ctx, elemProto);
    JS_FreeValue(ctx, elemProto);

    JS_SetPropertyStr(ctx, htmlCtor, "prototype", JS_DupValue(ctx, htmlProto));
    JS_SetPropertyStr(ctx, htmlProto, "constructor", JS_DupValue(ctx, htmlCtor));
    JS_FreeValue(ctx, htmlProto);

    JS_SetPropertyStr(ctx, global, "HTMLElement", htmlCtor);

    JS_FreeValue(ctx, global);
}

void cleanupCustomElements(JSContext* ctx)
{
    auto it = s_registries.find(ctx);
    if (it == s_registries.end()) return;

    auto* reg = it->second;
    for (auto& [name, def] : reg->defs) {
        JS_FreeValue(ctx, def.constructor);
    }
    delete reg;
    s_registries.erase(it);
}

} // namespace bro::js
