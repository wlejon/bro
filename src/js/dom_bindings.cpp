#include "js/dom_bindings.h"
#include "js/runtime.h"
#include "js/image_bindings.h"
#include "util/log.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/text_node.h"

#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <cctype>
#include <cstring>
#include <sstream>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// ===========================================================================
// Class IDs
// ===========================================================================

static JSClassID js_document_class_id = 0;
static JSClassID js_element_class_id  = 0;
static JSClassID js_event_class_id    = 0;
static JSClassID js_nodelist_class_id = 0;
static JSClassID js_cssstyle_class_id = 0;

// ===========================================================================
// Forward declarations of prototypes (set during install)
// ===========================================================================

static JSValue document_proto = JS_UNINITIALIZED;
static JSValue element_proto  = JS_UNINITIALIZED;
static JSValue event_proto    = JS_UNINITIALIZED;
static JSValue nodelist_proto = JS_UNINITIALIZED;
static JSValue cssstyle_proto = JS_UNINITIALIZED;

// Stashed Document pointer so appendChild can manage orphan ownership.
static bro::dom::Document* s_document = nullptr;

// Factory callback for element.getContext()
static DomBindings::GetContextFactory s_getContextFactory;

void DomBindings::setGetContextFactory(GetContextFactory factory) {
    s_getContextFactory = std::move(factory);
}

// ===========================================================================
// String conversion helpers
// ===========================================================================

/// Convert camelCase to kebab-case: "backgroundColor" -> "background-color"
static std::string camelToKebab(const std::string& name)
{
    std::string result;
    result.reserve(name.size() + 4);
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (std::isupper(static_cast<unsigned char>(c))) {
            if (i > 0) result += '-';
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            result += c;
        }
    }
    return result;
}

/// Convert kebab-case to camelCase: "background-color" -> "backgroundColor"
static std::string kebabToCamel(const std::string& name)
{
    std::string result;
    result.reserve(name.size());
    bool nextUpper = false;
    for (char c : name) {
        if (c == '-') {
            nextUpper = true;
        } else if (nextUpper) {
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            nextUpper = false;
        } else {
            result += c;
        }
    }
    return result;
}

// ===========================================================================
// Helper to get a C++ string from a JSValue argument
// ===========================================================================

static std::string jsToStdString(JSContext* ctx, JSValueConst val)
{
    const char* s = JS_ToCString(ctx, val);
    std::string result;
    if (s) {
        result = s;
        JS_FreeCString(ctx, s);
    }
    return result;
}

// ===========================================================================
// NodeList wrapper – wraps a vector<Element*>
// ===========================================================================

struct NodeListData {
    std::vector<bro::dom::Element*> elements;
};

static void js_nodelist_finalizer(JSRuntime* /*rt*/, JSValue val)
{
    auto* data = static_cast<NodeListData*>(
        JS_GetOpaque(val, js_nodelist_class_id));
    delete data;
}

static JSClassDef js_nodelist_class = {
    "NodeList",
    js_nodelist_finalizer,
    nullptr, nullptr, nullptr
};

static JSValue js_nodelist_length(JSContext* ctx, JSValueConst this_val)
{
    auto* data = static_cast<NodeListData*>(
        JS_GetOpaque(this_val, js_nodelist_class_id));
    if (!data) return JS_UNDEFINED;
    return JS_NewInt32(ctx, static_cast<int32_t>(data->elements.size()));
}

static JSValue js_nodelist_item(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv)
{
    auto* data = static_cast<NodeListData*>(
        JS_GetOpaque(this_val, js_nodelist_class_id));
    if (!data || argc < 1) return JS_UNDEFINED;

    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    if (idx < 0 || static_cast<size_t>(idx) >= data->elements.size())
        return JS_NULL;

    return DomBindings::wrapElement(ctx, data->elements[static_cast<size_t>(idx)]);
}

static const JSCFunctionListEntry js_nodelist_proto_funcs[] = {
    JS_CGETSET_DEF("length", js_nodelist_length, nullptr),
    JS_CFUNC_DEF("item", 1, js_nodelist_item),
};

static JSValue wrapNodeList(JSContext* ctx,
                            const std::vector<bro::dom::Element*>& elems)
{
    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_nodelist_class_id));
    if (JS_IsException(obj)) return obj;

    auto* data = new NodeListData{elems};
    JS_SetOpaque(obj, data);

    // Also set indexed properties so nodeList[0] works.
    for (size_t i = 0; i < elems.size(); ++i) {
        JS_SetPropertyUint32(ctx, obj, static_cast<uint32_t>(i),
                             DomBindings::wrapElement(ctx, elems[i]));
    }

    return obj;
}

// ===========================================================================
// Event wrapper
// ===========================================================================

struct EventData {
    std::string type;
    bro::dom::Element* target        = nullptr;
    bro::dom::Element* currentTarget = nullptr;
    bool bubbles          = false;
    bool cancelable       = false;
    bool defaultPrevented = false;
    double timeStamp      = 0;
    bool propagationStopped = false;
};

static void js_event_finalizer(JSRuntime* /*rt*/, JSValue val)
{
    auto* data = static_cast<EventData*>(
        JS_GetOpaque(val, js_event_class_id));
    delete data;
}

static JSClassDef js_event_class = {
    "Event",
    js_event_finalizer,
    nullptr, nullptr, nullptr
};

static JSValue js_event_get_type(JSContext* ctx, JSValueConst this_val)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (!e) return JS_UNDEFINED;
    return JS_NewString(ctx, e->type.c_str());
}

static JSValue js_event_get_target(JSContext* ctx, JSValueConst this_val)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (!e || !e->target) return JS_NULL;
    return DomBindings::wrapElement(ctx, e->target);
}

static JSValue js_event_get_currentTarget(JSContext* ctx, JSValueConst this_val)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (!e || !e->currentTarget) return JS_NULL;
    return DomBindings::wrapElement(ctx, e->currentTarget);
}

static JSValue js_event_get_bubbles(JSContext* ctx, JSValueConst this_val)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (!e) return JS_UNDEFINED;
    return JS_NewBool(ctx, e->bubbles);
}

static JSValue js_event_get_cancelable(JSContext* ctx, JSValueConst this_val)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (!e) return JS_UNDEFINED;
    return JS_NewBool(ctx, e->cancelable);
}

static JSValue js_event_get_defaultPrevented(JSContext* ctx, JSValueConst this_val)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (!e) return JS_UNDEFINED;
    return JS_NewBool(ctx, e->defaultPrevented);
}

static JSValue js_event_get_timeStamp(JSContext* ctx, JSValueConst this_val)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (!e) return JS_UNDEFINED;
    return JS_NewFloat64(ctx, e->timeStamp);
}

static JSValue js_event_preventDefault(JSContext* ctx, JSValueConst this_val,
                                       int /*argc*/, JSValueConst* /*argv*/)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (e && e->cancelable) e->defaultPrevented = true;
    (void)ctx;
    return JS_UNDEFINED;
}

static JSValue js_event_stopPropagation(JSContext* ctx, JSValueConst this_val,
                                        int /*argc*/, JSValueConst* /*argv*/)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (e) e->propagationStopped = true;
    (void)ctx;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_event_proto_funcs[] = {
    JS_CGETSET_DEF("type",             js_event_get_type,             nullptr),
    JS_CGETSET_DEF("target",           js_event_get_target,           nullptr),
    JS_CGETSET_DEF("currentTarget",    js_event_get_currentTarget,    nullptr),
    JS_CGETSET_DEF("bubbles",          js_event_get_bubbles,          nullptr),
    JS_CGETSET_DEF("cancelable",       js_event_get_cancelable,       nullptr),
    JS_CGETSET_DEF("defaultPrevented", js_event_get_defaultPrevented, nullptr),
    JS_CGETSET_DEF("timeStamp",        js_event_get_timeStamp,        nullptr),
    JS_CFUNC_DEF("preventDefault",  0, js_event_preventDefault),
    JS_CFUNC_DEF("stopPropagation", 0, js_event_stopPropagation),
};

static JSValue wrapEvent(JSContext* ctx, const std::string& type,
                         bro::dom::Element* target)
{
    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_event_class_id));
    if (JS_IsException(obj)) return obj;

    auto* data = new EventData();
    data->type          = type;
    data->target        = target;
    data->currentTarget = target;
    data->bubbles       = true;
    data->cancelable    = true;
    JS_SetOpaque(obj, data);
    return obj;
}

// ===========================================================================
// CSSStyleDeclaration wrapper (wraps bro::dom::StyleProxy)
// ===========================================================================

// The exotic methods let us intercept property gets/sets for arbitrary CSS
// property names (e.g., style.backgroundColor).

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

    // "cssText" is handled by the function list, skip exotic for it.
    if (nameStr == "cssText") return 0;

    // Convert camelCase JS property to kebab-case CSS property.
    std::string cssName = camelToKebab(nameStr);
    std::string val = style->getProperty(cssName);

    if (desc) {
        desc->flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE;
        desc->value = JS_NewString(ctx, val.c_str());
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    }
    return 1; // property found
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
    js_cssstyle_get_own_property,   // get_own_property
    nullptr,                        // get_own_property_names
    nullptr,                        // delete_property
    nullptr,                        // define_own_property
    nullptr,                        // has_property
    nullptr,                        // get_property
    js_cssstyle_set_property,       // set_property
};

static JSClassDef js_cssstyle_class = {
    "CSSStyleDeclaration",
    nullptr,    // finalizer – we don't own the StyleProxy, Element does
    nullptr,    // gc_mark
    nullptr,    // call
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

static JSValue wrapStyleProxy(JSContext* ctx, bro::dom::StyleProxy* style)
{
    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_cssstyle_class_id));
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, style);
    return obj;
}

// ===========================================================================
// DOMTokenList wrapper (classList)
// ===========================================================================

static JSClassID js_tokenlist_class_id = 0;

static JSClassDef js_tokenlist_class = {
    "DOMTokenList",
    nullptr, nullptr, nullptr, nullptr
};

// The DOMTokenList stores a pointer back to its owning Element.
// All operations read/write the element's "class" attribute directly.

static inline bro::dom::Element* getTokenListElement(JSValueConst val) {
    return static_cast<bro::dom::Element*>(
        JS_GetOpaque(val, js_tokenlist_class_id));
}

// Helper: split class attribute into tokens
static std::vector<std::string> splitClasses(const std::string& cls) {
    std::vector<std::string> tokens;
    std::istringstream iss(cls);
    std::string tok;
    while (iss >> tok) tokens.push_back(tok);
    return tokens;
}

// Helper: join tokens back to string
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
    for (int i = 0; i < argc; ++i) {
        std::string token = jsToStdString(ctx, argv[i]);
        if (token.empty()) continue;
        bool found = false;
        for (auto& t : tokens) { if (t == token) { found = true; break; } }
        if (!found) tokens.push_back(token);
    }
    el->setClassName(joinClasses(tokens));
    return JS_UNDEFINED;
}

static JSValue js_tokenlist_remove(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* el = getTokenListElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto tokens = splitClasses(el->className());
    for (int i = 0; i < argc; ++i) {
        std::string token = jsToStdString(ctx, argv[i]);
        tokens.erase(std::remove(tokens.begin(), tokens.end(), token), tokens.end());
    }
    el->setClassName(joinClasses(tokens));
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
        // Token present
        if (hasForce && JS_ToBool(ctx, argv[1])) {
            // force=true: keep it
            el->setClassName(joinClasses(tokens));
            return JS_TRUE;
        }
        tokens.erase(it);
        el->setClassName(joinClasses(tokens));
        return JS_FALSE;
    } else {
        // Token absent
        if (hasForce && !JS_ToBool(ctx, argv[1])) {
            // force=false: don't add
            return JS_FALSE;
        }
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

static JSValue tokenlist_proto = JS_UNINITIALIZED;

static JSValue wrapTokenList(JSContext* ctx, bro::dom::Element* elem) {
    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_tokenlist_class_id));
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, elem);
    return obj;
}

// ===========================================================================
// Element wrapper
// ===========================================================================

// We do NOT add a destructor – the C++ DOM owns the Element objects.
static JSClassDef js_element_class = {
    "Element",
    nullptr, nullptr, nullptr, nullptr
};

static inline bro::dom::Element* getElement(JSValueConst val)
{
    return static_cast<bro::dom::Element*>(
        JS_GetOpaque(val, js_element_class_id));
}

// ---- Properties -----------------------------------------------------------

static JSValue js_element_get_id(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    return JS_NewString(ctx, el->id().c_str());
}

static JSValue js_element_set_id(JSContext* ctx, JSValueConst this_val,
                                 JSValueConst val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    el->setAttribute("id", jsToStdString(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_element_get_tagName(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    return JS_NewString(ctx, el->tagName().c_str());
}

static JSValue js_element_get_className(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    return JS_NewString(ctx, el->getAttribute("class").c_str());
}

static JSValue js_element_set_className(JSContext* ctx, JSValueConst this_val,
                                        JSValueConst val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    el->setAttribute("class", jsToStdString(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_element_get_textContent(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    return JS_NewString(ctx, el->textContent().c_str());
}

static JSValue js_element_set_textContent(JSContext* ctx, JSValueConst this_val,
                                          JSValueConst val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    el->setTextContent(jsToStdString(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_element_get_innerHTML(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    return JS_NewString(ctx, el->innerHTML().c_str());
}

static JSValue js_element_set_innerHTML(JSContext* ctx, JSValueConst this_val,
                                        JSValueConst val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    el->setInnerHTML(jsToStdString(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_element_get_parentElement(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_NULL;
    auto* parent = el->parentElement();
    if (!parent) return JS_NULL;
    return DomBindings::wrapElement(ctx, parent);
}

static JSValue js_element_get_children(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_NewArray(ctx);
    auto kids = el->children();
    return wrapNodeList(ctx, kids);
}

static JSValue js_element_get_childNodes(JSContext* ctx, JSValueConst this_val)
{
    // For now childNodes == children (we don't have text nodes as separate objects)
    return js_element_get_children(ctx, this_val);
}

static JSValue js_element_get_classList(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    return wrapTokenList(ctx, el);
}

static JSValue js_element_get_parentNode(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_NULL;
    auto* parent = el->parentNode();
    if (!parent || parent->nodeType() != bro::dom::NodeType::Element) return JS_NULL;
    return DomBindings::wrapElement(ctx, static_cast<bro::dom::Element*>(parent));
}

// Helper: wrap any child Node as a JS value that jQuery can traverse.
// Elements get the full Element wrapper. Text nodes get a lightweight
// plain-object wrapper with nodeType + sibling getters backed by C++ closures.
//
// We use QuickJS exotic-object getters (defineProperty with get:) so that
// nextSibling/previousSibling are computed lazily, avoiding the exponential
// eager-expansion problem.

// Helper: build linked sibling chain of all children for firstChild/lastChild.
// jQuery's dir() helper walks firstChild → nextSibling, so text nodes need
// nextSibling/previousSibling to be set. We build the full chain in one pass.
static void buildChildChain(JSContext* ctx, bro::dom::Element* el,
                            std::vector<JSValue>& wrappers)
{
    auto& kids = el->childNodes();
    wrappers.reserve(kids.size());
    for (size_t i = 0; i < kids.size(); ++i) {
        auto* child = kids[i].get();
        if (child->nodeType() == bro::dom::NodeType::Element) {
            wrappers.push_back(DomBindings::wrapElement(ctx, static_cast<bro::dom::Element*>(child)));
        } else {
            JSValue obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "nodeType", JS_NewInt32(ctx, static_cast<int32_t>(child->nodeType())));
            JS_SetPropertyStr(ctx, obj, "nodeName", JS_NewString(ctx, child->nodeName().c_str()));
            if (child->nodeType() == bro::dom::NodeType::Text) {
                auto* text = static_cast<bro::dom::TextNode*>(child);
                JS_SetPropertyStr(ctx, obj, "textContent", JS_NewString(ctx, text->data().c_str()));
                JS_SetPropertyStr(ctx, obj, "nodeValue", JS_NewString(ctx, text->data().c_str()));
            }
            JS_SetPropertyStr(ctx, obj, "parentNode", DomBindings::wrapElement(ctx, el));
            wrappers.push_back(obj);
        }
    }
    for (size_t i = 0; i < wrappers.size(); ++i) {
        JS_SetPropertyStr(ctx, wrappers[i], "nextSibling",
            (i + 1 < wrappers.size()) ? JS_DupValue(ctx, wrappers[i + 1]) : JS_NULL);
        JS_SetPropertyStr(ctx, wrappers[i], "previousSibling",
            (i > 0) ? JS_DupValue(ctx, wrappers[i - 1]) : JS_NULL);
    }
}

static JSValue js_element_get_firstChild(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el || el->childNodes().empty()) return JS_NULL;
    std::vector<JSValue> chain;
    buildChildChain(ctx, el, chain);
    for (size_t i = 1; i < chain.size(); ++i) JS_FreeValue(ctx, chain[i]);
    return chain.empty() ? JS_NULL : chain[0];
}

static JSValue js_element_get_lastChild(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el || el->childNodes().empty()) return JS_NULL;
    std::vector<JSValue> chain;
    buildChildChain(ctx, el, chain);
    for (size_t i = 0; i + 1 < chain.size(); ++i) JS_FreeValue(ctx, chain[i]);
    return chain.empty() ? JS_NULL : chain.back();
}

// Forward declaration
static void buildChildChain(JSContext* ctx, bro::dom::Element* el,
                            std::vector<JSValue>& wrappers);

static JSValue js_element_get_nextSibling(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el || !el->parentNode() ||
        el->parentNode()->nodeType() != bro::dom::NodeType::Element) return JS_NULL;
    auto* parent = static_cast<bro::dom::Element*>(el->parentNode());
    std::vector<JSValue> chain;
    buildChildChain(ctx, parent, chain);
    // Find this element in the chain and return its nextSibling
    JSValue result = JS_NULL;
    auto& kids = parent->childNodes();
    for (size_t i = 0; i < kids.size(); ++i) {
        if (kids[i].get() == el && i + 1 < chain.size()) {
            result = JS_DupValue(ctx, chain[i + 1]);
            break;
        }
    }
    for (auto& w : chain) JS_FreeValue(ctx, w);
    return result;
}

static JSValue js_element_get_previousSibling(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el || !el->parentNode() ||
        el->parentNode()->nodeType() != bro::dom::NodeType::Element) return JS_NULL;
    auto* parent = static_cast<bro::dom::Element*>(el->parentNode());
    std::vector<JSValue> chain;
    buildChildChain(ctx, parent, chain);
    JSValue result = JS_NULL;
    auto& kids = parent->childNodes();
    for (size_t i = 0; i < kids.size(); ++i) {
        if (kids[i].get() == el && i > 0) {
            result = JS_DupValue(ctx, chain[i - 1]);
            break;
        }
    }
    for (auto& w : chain) JS_FreeValue(ctx, w);
    return result;
}

static JSValue js_element_get_nextElementSibling(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el || !el->parentNode()) return JS_NULL;
    auto& siblings = el->parentNode()->childNodes();
    bool found = false;
    for (size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i].get() == el) { found = true; continue; }
        if (found && siblings[i]->nodeType() == bro::dom::NodeType::Element)
            return DomBindings::wrapElement(ctx, static_cast<bro::dom::Element*>(siblings[i].get()));
    }
    return JS_NULL;
}

static JSValue js_element_get_previousElementSibling(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el || !el->parentNode()) return JS_NULL;
    auto& siblings = el->parentNode()->childNodes();
    bro::dom::Element* lastElem = nullptr;
    for (size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i].get() == el) return lastElem ? DomBindings::wrapElement(ctx, lastElem) : JS_NULL;
        if (siblings[i]->nodeType() == bro::dom::NodeType::Element)
            lastElem = static_cast<bro::dom::Element*>(siblings[i].get());
    }
    return JS_NULL;
}

static JSValue js_element_get_firstElementChild(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_NULL;
    for (auto& child : el->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element)
            return DomBindings::wrapElement(ctx, static_cast<bro::dom::Element*>(child.get()));
    }
    return JS_NULL;
}

static JSValue js_element_get_lastElementChild(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_NULL;
    auto& kids = el->childNodes();
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
        if ((*it)->nodeType() == bro::dom::NodeType::Element)
            return DomBindings::wrapElement(ctx, static_cast<bro::dom::Element*>((*it).get()));
    }
    return JS_NULL;
}

static JSValue js_element_get_childElementCount(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_NewInt32(ctx, 0);
    int32_t count = 0;
    for (auto& child : el->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element) ++count;
    }
    return JS_NewInt32(ctx, count);
}

static JSValue js_element_get_nodeType(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    return JS_NewInt32(ctx, static_cast<int32_t>(el->nodeType()));
}

static JSValue js_element_get_nodeName(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    return JS_NewString(ctx, el->nodeName().c_str());
}

static JSValue js_element_get_style(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    return wrapStyleProxy(ctx, &el->style());
}

// ---- Methods --------------------------------------------------------------

static JSValue js_element_getAttribute(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_UNDEFINED;
    std::string name = jsToStdString(ctx, argv[0]);
    std::string val  = el->getAttribute(name);
    if (val.empty()) return JS_NULL; // spec returns null if not present
    return JS_NewString(ctx, val.c_str());
}

static JSValue js_element_setAttribute(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 2) return JS_UNDEFINED;
    el->setAttribute(jsToStdString(ctx, argv[0]),
                     jsToStdString(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_element_removeAttribute(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_UNDEFINED;
    el->removeAttribute(jsToStdString(ctx, argv[0]));
    return JS_UNDEFINED;
}

static std::shared_ptr<bro::dom::Node> findSharedPtr(bro::dom::Element* child) {
    // If the child has a parent, it's in that parent's children_ vector.
    if (child->parentNode()) {
        for (auto& c : child->parentNode()->childNodes()) {
            if (c.get() == child) return c;
        }
    }
    // Otherwise check orphans in the document.
    if (s_document) {
        // Look through orphans_ -- Document::createElement stores them there.
        // We need to find the shared_ptr that owns this element.
        // adoptOrphan removes it, but we need to get it first.
        // For now, create a shared_ptr from orphans by scanning.
        for (auto& orphan : s_document->orphans()) {
            if (orphan.get() == child) return orphan;
        }
    }
    // Fallback: no-op deleter (shouldn't happen if createElement is used properly)
    return std::shared_ptr<bro::dom::Node>(child, [](bro::dom::Node*){});
}

static JSValue js_element_appendChild(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_UNDEFINED;
    auto* child = static_cast<bro::dom::Element*>(
        DomBindings::unwrapElement(ctx, argv[0]));
    if (child) {
        // DocumentFragment: move all children to parent instead
        if (child->tagName() == "#DOCUMENT-FRAGMENT") {
            // Copy children vector since we're modifying it during iteration
            auto kids = child->childNodes();
            for (auto& kid : kids) {
                el->appendChild(kid);
            }
            if (s_document) s_document->adoptOrphan(child);
        } else {
            auto childPtr = findSharedPtr(child);
            el->appendChild(childPtr);
            if (s_document) s_document->adoptOrphan(child);
        }
    }
    return argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
}

static JSValue js_element_removeChild(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_UNDEFINED;
    auto* child = static_cast<bro::dom::Element*>(
        DomBindings::unwrapElement(ctx, argv[0]));
    if (child) {
        // Unregister ID from document so getElementById won't find detached elements
        if (s_document && !child->id().empty()) {
            s_document->unregisterElementId(child->id());
        }
        el->removeChild(static_cast<bro::dom::Node*>(child));
    }
    return argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
}

static JSValue js_element_insertBefore(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 2) return JS_UNDEFINED;
    auto* newChild = static_cast<bro::dom::Element*>(
        DomBindings::unwrapElement(ctx, argv[0]));
    bro::dom::Element* refChild = nullptr;
    if (!JS_IsNull(argv[1])) {
        refChild = static_cast<bro::dom::Element*>(
            DomBindings::unwrapElement(ctx, argv[1]));
    }
    if (newChild) {
        auto childPtr = findSharedPtr(newChild);
        el->insertBefore(childPtr, static_cast<bro::dom::Node*>(refChild));
        if (s_document) s_document->adoptOrphan(newChild);
    }
    return argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
}

static JSValue js_element_replaceChild(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 2) return JS_UNDEFINED;
    auto* newChild = static_cast<bro::dom::Element*>(
        DomBindings::unwrapElement(ctx, argv[0]));
    auto* oldChild = static_cast<bro::dom::Element*>(
        DomBindings::unwrapElement(ctx, argv[1]));
    if (newChild && oldChild) {
        auto newPtr = findSharedPtr(newChild);
        // insertBefore newChild before oldChild, then remove oldChild
        el->insertBefore(newPtr, static_cast<bro::dom::Node*>(oldChild));
        if (s_document && !oldChild->id().empty()) {
            s_document->unregisterElementId(oldChild->id());
        }
        el->removeChild(static_cast<bro::dom::Node*>(oldChild));
        if (s_document) s_document->adoptOrphan(newChild);
    }
    return argc >= 2 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
}

static JSValue js_element_cloneNode(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || !s_document) return JS_NULL;

    bool deep = false;
    if (argc >= 1) deep = JS_ToBool(ctx, argv[0]);

    // Create a new element with the same tag
    auto clone = s_document->createElement(el->tagName());
    if (!clone) return JS_NULL;

    // Copy attributes (skip "id" — cloned nodes must not duplicate IDs)
    std::string cls = el->className();
    if (!cls.empty()) clone->setClassName(cls);

    static const char* attrs[] = {
        "style", "href", "src", "alt", "title", "name", "value", "type",
        "placeholder", "data-action", "data-setting", "data-control",
        "width", "height", "disabled", "checked", "selected", nullptr
    };
    for (int i = 0; attrs[i]; ++i) {
        std::string val = el->getAttribute(attrs[i]);
        if (!val.empty()) clone->setAttribute(attrs[i], val);
    }

    if (deep) {
        // Deep clone: recursively clone children
        for (auto& child : el->childNodes()) {
            if (child->nodeType() == bro::dom::NodeType::Element) {
                // Wrap child, call cloneNode(true), unwrap, appendChild
                JSValue childJs = DomBindings::wrapElement(ctx, static_cast<bro::dom::Element*>(child.get()));
                JSValue trueVal = JS_TRUE;
                JSValue clonedJs = js_element_cloneNode(ctx, childJs, 1, &trueVal);
                auto* clonedElem = static_cast<bro::dom::Element*>(DomBindings::unwrapElement(ctx, clonedJs));
                if (clonedElem) {
                    auto clonedPtr = findSharedPtr(clonedElem);
                    clone->appendChild(clonedPtr);
                    if (s_document) s_document->adoptOrphan(clonedElem);
                }
                JS_FreeValue(ctx, clonedJs);
                JS_FreeValue(ctx, childJs);
            } else if (child->nodeType() == bro::dom::NodeType::Text) {
                auto* textNode = static_cast<bro::dom::TextNode*>(child.get());
                auto clonedText = std::make_shared<bro::dom::TextNode>(textNode->data());
                clone->appendChild(clonedText);
            }
        }
    }

    return DomBindings::wrapElement(ctx, clone.get());
}

static JSValue js_element_remove(JSContext* ctx, JSValueConst this_val,
                                 int /*argc*/, JSValueConst* /*argv*/)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* parent = el->parentElement();
    if (parent) {
        if (s_document && !el->id().empty()) {
            s_document->unregisterElementId(el->id());
        }
        parent->removeChild(static_cast<bro::dom::Node*>(el));
    }
    (void)ctx;
    return JS_UNDEFINED;
}

// --- Event listeners -------------------------------------------------------

// We store callbacks in a per-context map: Element* -> type -> vector<JSValue>
// For simplicity we attach a hidden property on each JS Element object.

static const char* kListenersKey = "__bro_listeners";

struct ListenerEntry {
    std::string type;
    JSValue     callback; // DupValue'd
};

static JSValue js_element_addEventListener(JSContext* ctx,
                                           JSValueConst this_val,
                                           int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;

    std::string type = jsToStdString(ctx, argv[0]);

    // Store in a JS array attached to the element wrapper.
    JSAtom key = JS_NewAtom(ctx, kListenersKey);
    JSValue arr = JS_GetProperty(ctx, this_val, key);
    if (JS_IsUndefined(arr) || JS_IsException(arr)) {
        arr = JS_NewArray(ctx);
        JSValue mutable_this = this_val;
        JS_SetProperty(ctx, mutable_this, JS_NewAtom(ctx, kListenersKey), JS_DupValue(ctx, arr));
    }

    // Push {type, callback} as a small object
    JSValue entry = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entry, "type", JS_NewString(ctx, type.c_str()));
    JS_SetPropertyStr(ctx, entry, "cb", JS_DupValue(ctx, argv[1]));

    int64_t len = 0;
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    JS_ToInt64(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);
    JS_SetPropertyInt64(ctx, arr, len, entry);

    JS_FreeValue(ctx, arr);
    JS_FreeAtom(ctx, key);

    // Also register with the C++ Element so it knows to dispatch.
    // We use the JS callback pointer as a listener id.
    el->addEventListener(type, static_cast<int64_t>(len));

    return JS_UNDEFINED;
}

static JSValue js_element_removeEventListener(JSContext* ctx,
                                              JSValueConst this_val,
                                              int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;

    std::string type = jsToStdString(ctx, argv[0]);

    JSAtom key = JS_NewAtom(ctx, kListenersKey);
    JSValue arr = JS_GetProperty(ctx, this_val, key);
    JS_FreeAtom(ctx, key);

    if (JS_IsUndefined(arr) || !JS_IsArray(arr)) {
        JS_FreeValue(ctx, arr);
        return JS_UNDEFINED;
    }

    int64_t len = 0;
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    JS_ToInt64(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);

    for (int64_t i = 0; i < len; ++i) {
        JSValue entry = JS_GetPropertyInt64(ctx, arr, i);
        if (JS_IsObject(entry)) {
            JSValue tval = JS_GetPropertyStr(ctx, entry, "type");
            std::string t = jsToStdString(ctx, tval);
            JS_FreeValue(ctx, tval);

            if (t == type) {
                JSValue cb = JS_GetPropertyStr(ctx, entry, "cb");
                // Crude identity check – works for the same function reference.
                // A more robust approach would compare via an opaque id.
                // For now just remove the first matching type entry.
                JS_FreeValue(ctx, cb);
                // Remove by setting to undefined (sparse); good enough for now.
                JS_SetPropertyInt64(ctx, arr, i, JS_UNDEFINED);
                JS_FreeValue(ctx, entry);
                break;
            }
        }
        JS_FreeValue(ctx, entry);
    }

    JS_FreeValue(ctx, arr);
    return JS_UNDEFINED;
}

static JSValue js_element_querySelector(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_NULL;
    std::string sel = jsToStdString(ctx, argv[0]);
    auto* found = el->querySelector(sel);
    if (!found) return JS_NULL;
    return DomBindings::wrapElement(ctx, found);
}

static JSValue js_element_querySelectorAll(JSContext* ctx,
                                           JSValueConst this_val,
                                           int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_NewArray(ctx);
    std::string sel = jsToStdString(ctx, argv[0]);
    auto results = el->querySelectorAll(sel);
    return wrapNodeList(ctx, results);
}

static JSValue js_element_closest(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_NULL;
    std::string sel = jsToStdString(ctx, argv[0]);
    auto* found = el->closest(sel);
    if (!found) return JS_NULL;
    return DomBindings::wrapElement(ctx, found);
}

static JSValue js_element_matches(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_FALSE;
    std::string sel = jsToStdString(ctx, argv[0]);
    return JS_NewBool(ctx, el->matches(sel));
}

static JSValue js_element_hasAttribute(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_FALSE;
    std::string name = jsToStdString(ctx, argv[0]);
    return JS_NewBool(ctx, !el->getAttribute(name).empty());
}

static JSValue js_element_contains(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_FALSE;
    auto* other = static_cast<bro::dom::Element*>(
        DomBindings::unwrapElement(ctx, argv[0]));
    if (!other) return JS_FALSE;
    // Walk up from other's parents to see if we reach el
    auto* node = static_cast<bro::dom::Node*>(other);
    while (node) {
        if (node == el) return JS_TRUE;
        node = node->parentNode();
    }
    return JS_FALSE;
}

static JSValue js_element_compareDocumentPosition(JSContext* ctx,
                                                  JSValueConst this_val,
                                                  int argc, JSValueConst* argv)
{
    // Simplified: return DOCUMENT_POSITION_FOLLOWING (4) or PRECEDING (2)
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_NewInt32(ctx, 0);
    auto* other = static_cast<bro::dom::Element*>(
        DomBindings::unwrapElement(ctx, argv[0]));
    if (!other) return JS_NewInt32(ctx, 0);
    if (el == other) return JS_NewInt32(ctx, 0);

    // Check if other is contained by el
    auto* node = static_cast<bro::dom::Node*>(other);
    while (node) {
        if (node == el) return JS_NewInt32(ctx, 16 | 4); // CONTAINS | FOLLOWING
        node = node->parentNode();
    }
    // Check if el is contained by other
    node = static_cast<bro::dom::Node*>(el);
    while (node) {
        if (node == other) return JS_NewInt32(ctx, 8 | 2); // CONTAINED_BY | PRECEDING
        node = node->parentNode();
    }
    // Use node IDs for ordering
    return JS_NewInt32(ctx, el->nodeId() < other->nodeId() ? 4 : 2);
}

static JSValue js_element_get_ownerDocument(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el || !el->document()) return JS_NULL;
    // Return the global document object
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue doc = JS_GetPropertyStr(ctx, global, "document");
    JS_FreeValue(ctx, global);
    return doc;
}

static JSValue js_element_getElementsByTagName(JSContext* ctx,
                                               JSValueConst this_val,
                                               int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_NewArray(ctx);
    std::string tag = jsToStdString(ctx, argv[0]);
    auto results = el->querySelectorAll(tag);
    return wrapNodeList(ctx, results);
}

static JSValue js_element_getElementsByClassName(JSContext* ctx,
                                                 JSValueConst this_val,
                                                 int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_NewArray(ctx);
    std::string cls = jsToStdString(ctx, argv[0]);
    auto results = el->querySelectorAll("." + cls);
    return wrapNodeList(ctx, results);
}

static JSValue js_element_getContext(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_NULL;
    // Only canvas elements support getContext
    if (el->tagName() != "canvas" && el->tagName() != "CANVAS") return JS_NULL;
    const char* typeStr = JS_ToCString(ctx, argv[0]);
    std::string type = typeStr ? typeStr : "";
    if (typeStr) JS_FreeCString(ctx, typeStr);
    if (type != "2d" && type != "webgl" && type != "webgl2") return JS_NULL;
    if (s_getContextFactory) {
        return s_getContextFactory(ctx, el, type);
    }
    return JS_NULL;
}

// ---- Function list --------------------------------------------------------

// --- width / height (canvas element support, also used by three.js) ---

static JSValue js_element_get_width(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val); if (!el) return JS_UNDEFINED;
    std::string v = el->getAttribute("width");
    if (!v.empty()) return JS_NewInt32(ctx, std::atoi(v.c_str()));
    return JS_NewInt32(ctx, 300); // canvas default
}

static JSValue js_element_set_width(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* el = getElement(this_val); if (!el) return JS_UNDEFINED;
    int w; JS_ToInt32(ctx, &w, val);
    el->setAttribute("width", std::to_string(w));
    return JS_UNDEFINED;
}

static JSValue js_element_get_height(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val); if (!el) return JS_UNDEFINED;
    std::string v = el->getAttribute("height");
    if (!v.empty()) return JS_NewInt32(ctx, std::atoi(v.c_str()));
    return JS_NewInt32(ctx, 150); // canvas default
}

static JSValue js_element_set_height(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* el = getElement(this_val); if (!el) return JS_UNDEFINED;
    int h; JS_ToInt32(ctx, &h, val);
    el->setAttribute("height", std::to_string(h));
    return JS_UNDEFINED;
}

// --- clientWidth / clientHeight (read-only, same as width/height for now) ---

static JSValue js_element_get_clientWidth(JSContext* ctx, JSValueConst this_val) {
    return js_element_get_width(ctx, this_val);
}

static JSValue js_element_get_clientHeight(JSContext* ctx, JSValueConst this_val) {
    return js_element_get_height(ctx, this_val);
}

static const JSCFunctionListEntry js_element_proto_funcs[] = {
    // Properties
    JS_CGETSET_DEF("id",            js_element_get_id,          js_element_set_id),
    JS_CGETSET_DEF("tagName",       js_element_get_tagName,     nullptr),
    JS_CGETSET_DEF("className",     js_element_get_className,   js_element_set_className),
    JS_CGETSET_DEF("textContent",   js_element_get_textContent, js_element_set_textContent),
    JS_CGETSET_DEF("innerHTML",     js_element_get_innerHTML,   js_element_set_innerHTML),
    JS_CGETSET_DEF("parentElement",          js_element_get_parentElement,          nullptr),
    JS_CGETSET_DEF("parentNode",             js_element_get_parentNode,             nullptr),
    JS_CGETSET_DEF("children",               js_element_get_children,               nullptr),
    JS_CGETSET_DEF("childNodes",             js_element_get_childNodes,             nullptr),
    JS_CGETSET_DEF("firstChild",             js_element_get_firstChild,             nullptr),
    JS_CGETSET_DEF("lastChild",              js_element_get_lastChild,              nullptr),
    JS_CGETSET_DEF("nextSibling",            js_element_get_nextSibling,            nullptr),
    JS_CGETSET_DEF("previousSibling",        js_element_get_previousSibling,        nullptr),
    JS_CGETSET_DEF("nextElementSibling",     js_element_get_nextElementSibling,     nullptr),
    JS_CGETSET_DEF("previousElementSibling", js_element_get_previousElementSibling, nullptr),
    JS_CGETSET_DEF("firstElementChild",      js_element_get_firstElementChild,      nullptr),
    JS_CGETSET_DEF("lastElementChild",       js_element_get_lastElementChild,       nullptr),
    JS_CGETSET_DEF("childElementCount",      js_element_get_childElementCount,      nullptr),
    JS_CGETSET_DEF("nodeType",               js_element_get_nodeType,               nullptr),
    JS_CGETSET_DEF("nodeName",               js_element_get_nodeName,               nullptr),
    JS_CGETSET_DEF("classList",              js_element_get_classList,              nullptr),
    JS_CGETSET_DEF("style",                  js_element_get_style,                  nullptr),
    JS_CGETSET_DEF("width",         js_element_get_width,       js_element_set_width),
    JS_CGETSET_DEF("height",        js_element_get_height,      js_element_set_height),
    JS_CGETSET_DEF("clientWidth",   js_element_get_clientWidth, nullptr),
    JS_CGETSET_DEF("clientHeight",  js_element_get_clientHeight, nullptr),
    JS_CGETSET_DEF("ownerDocument", js_element_get_ownerDocument, nullptr),
    // Methods
    JS_CFUNC_DEF("getAttribute",        1, js_element_getAttribute),
    JS_CFUNC_DEF("hasAttribute",        1, js_element_hasAttribute),
    JS_CFUNC_DEF("setAttribute",        2, js_element_setAttribute),
    JS_CFUNC_DEF("removeAttribute",     1, js_element_removeAttribute),
    JS_CFUNC_DEF("appendChild",         1, js_element_appendChild),
    JS_CFUNC_DEF("removeChild",         1, js_element_removeChild),
    JS_CFUNC_DEF("insertBefore",        2, js_element_insertBefore),
    JS_CFUNC_DEF("addEventListener",    2, js_element_addEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, js_element_removeEventListener),
    JS_CFUNC_DEF("querySelector",       1, js_element_querySelector),
    JS_CFUNC_DEF("querySelectorAll",    1, js_element_querySelectorAll),
    JS_CFUNC_DEF("replaceChild",        2, js_element_replaceChild),
    JS_CFUNC_DEF("cloneNode",           1, js_element_cloneNode),
    JS_CFUNC_DEF("closest",             1, js_element_closest),
    JS_CFUNC_DEF("matches",                   1, js_element_matches),
    JS_CFUNC_DEF("contains",                  1, js_element_contains),
    JS_CFUNC_DEF("compareDocumentPosition",   1, js_element_compareDocumentPosition),
    JS_CFUNC_DEF("getElementsByTagName",      1, js_element_getElementsByTagName),
    JS_CFUNC_DEF("getElementsByClassName",    1, js_element_getElementsByClassName),
    JS_CFUNC_DEF("remove",                    0, js_element_remove),
    JS_CFUNC_DEF("getContext",                1, js_element_getContext),
};

// ===========================================================================
// Document wrapper
// ===========================================================================

static JSClassDef js_document_class = {
    "Document",
    nullptr, nullptr, nullptr, nullptr
};

static inline bro::dom::Document* getDocument(JSValueConst val)
{
    return static_cast<bro::dom::Document*>(
        JS_GetOpaque(val, js_document_class_id));
}

static JSValue js_document_get_title(JSContext* ctx, JSValueConst this_val)
{
    auto* doc = getDocument(this_val);
    if (!doc) return JS_UNDEFINED;
    return JS_NewString(ctx, doc->title().c_str());
}

static JSValue js_document_set_title(JSContext* ctx, JSValueConst this_val,
                                     JSValueConst val)
{
    auto* doc = getDocument(this_val);
    if (!doc) return JS_UNDEFINED;
    doc->setTitle(jsToStdString(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_document_get_body(JSContext* ctx, JSValueConst this_val)
{
    auto* doc = getDocument(this_val);
    if (!doc) return JS_NULL;
    auto* body = doc->body();
    if (!body) return JS_NULL;
    return DomBindings::wrapElement(ctx, body);
}

static JSValue js_document_get_documentElement(JSContext* ctx,
                                               JSValueConst this_val)
{
    auto* doc = getDocument(this_val);
    if (!doc) return JS_NULL;
    auto* root = doc->documentElement();
    if (!root) return JS_NULL;
    return DomBindings::wrapElement(ctx, root);
}

static JSValue js_document_getElementById(JSContext* ctx,
                                          JSValueConst this_val,
                                          int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NULL;
    std::string id = jsToStdString(ctx, argv[0]);
    auto* el = doc->getElementById(id);
    if (!el) return JS_NULL;
    return DomBindings::wrapElement(ctx, el);
}

static JSValue js_document_createElement(JSContext* ctx,
                                         JSValueConst this_val,
                                         int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NULL;
    std::string tag = jsToStdString(ctx, argv[0]);
    // Return an Image object for <img> elements (supports src loading + texImage2D)
    if (tag == "img" || tag == "IMG")
        return ImageBindings::createImage(ctx);
    auto el = doc->createElement(tag);
    if (!el) return JS_NULL;
    // Document::createElement stores the element in orphans_, keeping it alive
    // until it is appended to a parent.
    return DomBindings::wrapElement(ctx, el.get());
}

static JSValue js_document_createElementNS(JSContext* ctx,
                                           JSValueConst this_val,
                                           int argc, JSValueConst* argv)
{
    // createElementNS(namespaceURI, qualifiedName) — ignore namespace, just create element
    auto* doc = getDocument(this_val);
    if (!doc || argc < 2) return JS_NULL;
    std::string tag = jsToStdString(ctx, argv[1]);
    if (tag == "img" || tag == "IMG")
        return ImageBindings::createImage(ctx);
    auto el = doc->createElement(tag);
    if (!el) return JS_NULL;
    return DomBindings::wrapElement(ctx, el.get());
}

static JSValue js_document_createTextNode(JSContext* ctx,
                                          JSValueConst this_val,
                                          int argc, JSValueConst* argv)
{
    // We model text nodes as elements with tag "#text" and textContent set.
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NULL;
    std::string text = jsToStdString(ctx, argv[0]);
    auto el = doc->createElement("#text");
    if (!el) return JS_NULL;
    el->setTextContent(text);
    return DomBindings::wrapElement(ctx, el.get());
}

static JSValue js_document_createDocumentFragment(JSContext* ctx,
                                                  JSValueConst this_val,
                                                  int /*argc*/, JSValueConst* /*argv*/)
{
    // Model fragment as an element with tag "#document-fragment".
    // When appendChild receives a fragment, it should move all children.
    auto* doc = getDocument(this_val);
    if (!doc) return JS_NULL;
    auto el = doc->createElement("#document-fragment");
    if (!el) return JS_NULL;
    return DomBindings::wrapElement(ctx, el.get());
}

static JSValue js_document_querySelector(JSContext* ctx,
                                         JSValueConst this_val,
                                         int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NULL;
    std::string sel = jsToStdString(ctx, argv[0]);
    auto results = doc->querySelectorAll(sel);
    if (results.empty()) return JS_NULL;
    return DomBindings::wrapElement(ctx, results[0]);
}

static JSValue js_document_querySelectorAll(JSContext* ctx,
                                            JSValueConst this_val,
                                            int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NewArray(ctx);
    std::string sel = jsToStdString(ctx, argv[0]);
    auto results = doc->querySelectorAll(sel);
    return wrapNodeList(ctx, results);
}

static JSValue js_document_get_nodeType(JSContext* ctx, JSValueConst /*this_val*/)
{
    return JS_NewInt32(ctx, 9); // Node.DOCUMENT_NODE
}

static JSValue js_document_get_nodeName(JSContext* ctx, JSValueConst /*this_val*/)
{
    return JS_NewString(ctx, "#document");
}

static JSValue js_document_createComment(JSContext* ctx,
                                         JSValueConst this_val,
                                         int argc, JSValueConst* argv)
{
    // Model comments as elements with tag "#comment"
    auto* doc = getDocument(this_val);
    if (!doc) return JS_NULL;
    std::string text = (argc >= 1) ? jsToStdString(ctx, argv[0]) : "";
    auto el = doc->createElement("#comment");
    if (!el) return JS_NULL;
    el->setTextContent(text);
    return DomBindings::wrapElement(ctx, el.get());
}

static JSValue js_document_getElementsByTagName(JSContext* ctx,
                                                JSValueConst this_val,
                                                int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NewArray(ctx);
    std::string tag = jsToStdString(ctx, argv[0]);
    // Use querySelectorAll with the tag name
    auto results = doc->querySelectorAll(tag);
    return wrapNodeList(ctx, results);
}

static JSValue js_document_getElementsByClassName(JSContext* ctx,
                                                  JSValueConst this_val,
                                                  int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NewArray(ctx);
    std::string cls = jsToStdString(ctx, argv[0]);
    auto results = doc->querySelectorAll("." + cls);
    return wrapNodeList(ctx, results);
}

static JSValue js_document_getElementsByName(JSContext* ctx,
                                             JSValueConst /*this_val*/,
                                             int /*argc*/, JSValueConst* /*argv*/)
{
    // Stub — returns empty NodeList
    std::vector<bro::dom::Element*> empty;
    return wrapNodeList(ctx, empty);
}

static JSValue js_document_get_defaultView(JSContext* ctx, JSValueConst /*this_val*/)
{
    // Return window (globalThis)
    return JS_GetGlobalObject(ctx);
}

static const JSCFunctionListEntry js_document_proto_funcs[] = {
    // Properties
    JS_CGETSET_DEF("title",           js_document_get_title,           js_document_set_title),
    JS_CGETSET_DEF("body",            js_document_get_body,            nullptr),
    JS_CGETSET_DEF("documentElement", js_document_get_documentElement, nullptr),
    JS_CGETSET_DEF("nodeType",        js_document_get_nodeType,        nullptr),
    JS_CGETSET_DEF("nodeName",        js_document_get_nodeName,        nullptr),
    JS_CGETSET_DEF("defaultView",     js_document_get_defaultView,     nullptr),
    // Methods
    JS_CFUNC_DEF("getElementById",          1, js_document_getElementById),
    JS_CFUNC_DEF("createElement",           1, js_document_createElement),
    JS_CFUNC_DEF("createElementNS",         2, js_document_createElementNS),
    JS_CFUNC_DEF("createTextNode",          1, js_document_createTextNode),
    JS_CFUNC_DEF("createComment",           1, js_document_createComment),
    JS_CFUNC_DEF("createDocumentFragment",  0, js_document_createDocumentFragment),
    JS_CFUNC_DEF("querySelector",           1, js_document_querySelector),
    JS_CFUNC_DEF("querySelectorAll",        1, js_document_querySelectorAll),
    JS_CFUNC_DEF("getElementsByTagName",    1, js_document_getElementsByTagName),
    JS_CFUNC_DEF("getElementsByClassName",  1, js_document_getElementsByClassName),
    JS_CFUNC_DEF("getElementsByName",       1, js_document_getElementsByName),
};

// ===========================================================================
// Wrap / unwrap helpers (public API)
// ===========================================================================

JSValue DomBindings::wrapElement(JSContext* ctx, void* element_ptr)
{
    if (!element_ptr) return JS_NULL;

    auto* elem = static_cast<bro::dom::Element*>(element_ptr);

    // Check if we already have a wrapper in __bro_elem_map
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (JS_IsUndefined(elemMap)) {
        elemMap = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__bro_elem_map", JS_DupValue(ctx, elemMap));
    }

    std::string key = std::to_string(elem->nodeId());
    JSValue existing = JS_GetPropertyStr(ctx, elemMap, key.c_str());
    if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) {
        // Return existing wrapper (prevents duplicates)
        JS_FreeValue(ctx, elemMap);
        JS_FreeValue(ctx, global);
        return existing;
    }
    JS_FreeValue(ctx, existing);

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_element_class_id));
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, elemMap);
        JS_FreeValue(ctx, global);
        return obj;
    }
    JS_SetOpaque(obj, element_ptr);
    JS_SetPrototype(ctx, obj, JS_DupValue(ctx, element_proto));

    // Register in the element map for event dispatch lookup
    JS_SetPropertyStr(ctx, elemMap, key.c_str(), JS_DupValue(ctx, obj));

    JS_FreeValue(ctx, elemMap);
    JS_FreeValue(ctx, global);
    return obj;
}

void* DomBindings::unwrapElement(JSContext* /*ctx*/, JSValueConst val)
{
    return JS_GetOpaque(val, js_element_class_id);
}

JSValue DomBindings::wrapDocument(JSContext* ctx, void* document_ptr)
{
    if (!document_ptr) return JS_NULL;

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_document_class_id));
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, document_ptr);
    JS_SetPrototype(ctx, obj, JS_DupValue(ctx, document_proto));
    return obj;
}

// ===========================================================================
// install() – register everything
// ===========================================================================

void DomBindings::install(JSContext* ctx, void* document_ptr)
{
    JSRuntime* rt = JS_GetRuntime(ctx);

    // ----- Allocate class IDs -----
    JS_NewClassID(rt, &js_document_class_id);
    JS_NewClassID(rt, &js_element_class_id);
    JS_NewClassID(rt, &js_event_class_id);
    JS_NewClassID(rt, &js_nodelist_class_id);
    JS_NewClassID(rt, &js_cssstyle_class_id);
    JS_NewClassID(rt, &js_tokenlist_class_id);

    // ----- Register classes on the runtime -----
    JS_NewClass(rt, js_document_class_id, &js_document_class);
    JS_NewClass(rt, js_element_class_id,  &js_element_class);
    JS_NewClass(rt, js_event_class_id,    &js_event_class);
    JS_NewClass(rt, js_nodelist_class_id, &js_nodelist_class);
    JS_NewClass(rt, js_cssstyle_class_id, &js_cssstyle_class);
    JS_NewClass(rt, js_tokenlist_class_id, &js_tokenlist_class);

    // ----- Create prototypes -----

    // Document prototype
    document_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, document_proto, js_document_proto_funcs,
                               sizeof(js_document_proto_funcs) / sizeof(js_document_proto_funcs[0]));
    JS_SetClassProto(ctx, js_document_class_id, JS_DupValue(ctx, document_proto));

    // Element prototype
    element_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, element_proto, js_element_proto_funcs,
                               sizeof(js_element_proto_funcs) / sizeof(js_element_proto_funcs[0]));
    JS_SetClassProto(ctx, js_element_class_id, JS_DupValue(ctx, element_proto));

    // Event prototype
    event_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, event_proto, js_event_proto_funcs,
                               sizeof(js_event_proto_funcs) / sizeof(js_event_proto_funcs[0]));
    JS_SetClassProto(ctx, js_event_class_id, JS_DupValue(ctx, event_proto));

    // NodeList prototype
    nodelist_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, nodelist_proto, js_nodelist_proto_funcs,
                               sizeof(js_nodelist_proto_funcs) / sizeof(js_nodelist_proto_funcs[0]));
    JS_SetClassProto(ctx, js_nodelist_class_id, JS_DupValue(ctx, nodelist_proto));

    // CSSStyleDeclaration prototype
    cssstyle_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, cssstyle_proto, js_cssstyle_proto_funcs,
                               sizeof(js_cssstyle_proto_funcs) / sizeof(js_cssstyle_proto_funcs[0]));
    JS_SetClassProto(ctx, js_cssstyle_class_id, JS_DupValue(ctx, cssstyle_proto));

    // DOMTokenList prototype
    tokenlist_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, tokenlist_proto, js_tokenlist_proto_funcs,
                               sizeof(js_tokenlist_proto_funcs) / sizeof(js_tokenlist_proto_funcs[0]));
    JS_SetClassProto(ctx, js_tokenlist_class_id, JS_DupValue(ctx, tokenlist_proto));

    // ----- Stash Document pointer for orphan management -----
    s_document = static_cast<bro::dom::Document*>(document_ptr);

    // ----- Set global `document` -----
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue docObj = wrapDocument(ctx, document_ptr);
    JS_SetPropertyStr(ctx, global, "document", docObj);
    JS_FreeValue(ctx, global);

    // ----- Polyfills for jQuery/framework compatibility -----
    const char* polyfills = R"JS(
(function() {
    // document.implementation.createHTMLDocument
    document.implementation = {
        createHTMLDocument: function(title) {
            // Return a minimal document-like object
            var body = document.createElement('body');
            var html = document.createElement('html');
            html.appendChild(body);
            return {
                nodeType: 9,
                documentElement: html,
                body: body,
                createElement: function(tag) { return document.createElement(tag); },
                createElementNS: function(ns, tag) { return document.createElementNS(ns, tag); },
                createTextNode: function(text) { return document.createTextNode(text); },
                createDocumentFragment: function() { return document.createDocumentFragment(); }
            };
        }
    };

    // Array.from polyfill (QuickJS may not have it)
    if (!Array.from) {
        Array.from = function(obj, mapFn) {
            var arr = [];
            for (var i = 0; i < obj.length; i++) arr.push(mapFn ? mapFn(obj[i], i) : obj[i]);
            return arr;
        };
    }

    // NodeList.prototype.forEach
    if (typeof NodeList !== 'undefined' && !NodeList.prototype.forEach) {
        NodeList.prototype.forEach = Array.prototype.forEach;
    }

    // window.getComputedStyle stub
    window.getComputedStyle = function(el) {
        if (el && el.style) return el.style;
        // Return a minimal object with getPropertyValue for elements without style
        return {
            getPropertyValue: function() { return ''; },
            setProperty: function() {},
            length: 0
        };
    };
})();
)JS";
    JSValue r = JS_Eval(ctx, polyfills, strlen(polyfills),
                        "<dom-polyfills>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, r);
}

void DomBindings::cleanup(JSContext* ctx) {
    JS_FreeValue(ctx, document_proto);
    JS_FreeValue(ctx, element_proto);
    JS_FreeValue(ctx, event_proto);
    JS_FreeValue(ctx, nodelist_proto);
    JS_FreeValue(ctx, cssstyle_proto);
    JS_FreeValue(ctx, tokenlist_proto);
    document_proto = JS_UNINITIALIZED;
    element_proto  = JS_UNINITIALIZED;
    event_proto    = JS_UNINITIALIZED;
    nodelist_proto = JS_UNINITIALIZED;
    cssstyle_proto = JS_UNINITIALIZED;
    tokenlist_proto = JS_UNINITIALIZED;
    s_document = nullptr;
}

} // namespace bro::js
