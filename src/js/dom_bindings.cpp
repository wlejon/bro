#include "js/dom_bindings.h"
#include "js/runtime.h"
#include "util/log.h"
#include "dom/document.h"
#include "dom/element.h"

#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <cctype>
#include <cstring>

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
        auto childPtr = findSharedPtr(child);
        el->appendChild(childPtr);
        // Remove from orphans if it was there (Document now doesn't need to keep it alive).
        if (s_document) s_document->adoptOrphan(child);
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
    if (child) el->removeChild(static_cast<bro::dom::Node*>(child));
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

static JSValue js_element_remove(JSContext* ctx, JSValueConst this_val,
                                 int /*argc*/, JSValueConst* /*argv*/)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* parent = el->parentElement();
    if (parent) parent->removeChild(static_cast<bro::dom::Node*>(el));
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
    // TODO: implement querySelectorAll on Element; for now return null
    (void)this_val; (void)argc; (void)argv;
    (void)ctx;
    return JS_NULL;
}

static JSValue js_element_querySelectorAll(JSContext* ctx,
                                           JSValueConst this_val,
                                           int argc, JSValueConst* argv)
{
    // TODO: implement querySelectorAll on Element; for now return empty array
    (void)this_val; (void)argc; (void)argv;
    return JS_NewArray(ctx);
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
    if (type != "2d") return JS_NULL;
    if (s_getContextFactory) {
        return s_getContextFactory(ctx, el, type);
    }
    return JS_NULL;
}

// ---- Function list --------------------------------------------------------

static const JSCFunctionListEntry js_element_proto_funcs[] = {
    // Properties
    JS_CGETSET_DEF("id",            js_element_get_id,          js_element_set_id),
    JS_CGETSET_DEF("tagName",       js_element_get_tagName,     nullptr),
    JS_CGETSET_DEF("className",     js_element_get_className,   js_element_set_className),
    JS_CGETSET_DEF("textContent",   js_element_get_textContent, js_element_set_textContent),
    JS_CGETSET_DEF("innerHTML",     js_element_get_innerHTML,   js_element_set_innerHTML),
    JS_CGETSET_DEF("parentElement", js_element_get_parentElement, nullptr),
    JS_CGETSET_DEF("children",      js_element_get_children,    nullptr),
    JS_CGETSET_DEF("childNodes",    js_element_get_childNodes,  nullptr),
    JS_CGETSET_DEF("style",         js_element_get_style,       nullptr),
    // Methods
    JS_CFUNC_DEF("getAttribute",        1, js_element_getAttribute),
    JS_CFUNC_DEF("setAttribute",        2, js_element_setAttribute),
    JS_CFUNC_DEF("removeAttribute",     1, js_element_removeAttribute),
    JS_CFUNC_DEF("appendChild",         1, js_element_appendChild),
    JS_CFUNC_DEF("removeChild",         1, js_element_removeChild),
    JS_CFUNC_DEF("insertBefore",        2, js_element_insertBefore),
    JS_CFUNC_DEF("addEventListener",    2, js_element_addEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, js_element_removeEventListener),
    JS_CFUNC_DEF("querySelector",       1, js_element_querySelector),
    JS_CFUNC_DEF("querySelectorAll",    1, js_element_querySelectorAll),
    JS_CFUNC_DEF("remove",             0, js_element_remove),
    JS_CFUNC_DEF("getContext",          1, js_element_getContext),
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
    auto el = doc->createElement(tag);
    if (!el) return JS_NULL;
    // Document::createElement stores the element in orphans_, keeping it alive
    // until it is appended to a parent.
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

static const JSCFunctionListEntry js_document_proto_funcs[] = {
    // Properties
    JS_CGETSET_DEF("title",           js_document_get_title,           js_document_set_title),
    JS_CGETSET_DEF("body",            js_document_get_body,            nullptr),
    JS_CGETSET_DEF("documentElement", js_document_get_documentElement, nullptr),
    // Methods
    JS_CFUNC_DEF("getElementById",  1, js_document_getElementById),
    JS_CFUNC_DEF("createElement",   1, js_document_createElement),
    JS_CFUNC_DEF("createTextNode",  1, js_document_createTextNode),
    JS_CFUNC_DEF("querySelector",   1, js_document_querySelector),
    JS_CFUNC_DEF("querySelectorAll",1, js_document_querySelectorAll),
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

    // ----- Register classes on the runtime -----
    JS_NewClass(rt, js_document_class_id, &js_document_class);
    JS_NewClass(rt, js_element_class_id,  &js_element_class);
    JS_NewClass(rt, js_event_class_id,    &js_event_class);
    JS_NewClass(rt, js_nodelist_class_id, &js_nodelist_class);
    JS_NewClass(rt, js_cssstyle_class_id, &js_cssstyle_class);

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

    // ----- Stash Document pointer for orphan management -----
    s_document = static_cast<bro::dom::Document*>(document_ptr);

    // ----- Set global `document` -----
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue docObj = wrapDocument(ctx, document_ptr);
    JS_SetPropertyStr(ctx, global, "document", docObj);
    JS_FreeValue(ctx, global);
}

void DomBindings::cleanup(JSContext* ctx) {
    JS_FreeValue(ctx, document_proto);
    JS_FreeValue(ctx, element_proto);
    JS_FreeValue(ctx, event_proto);
    JS_FreeValue(ctx, nodelist_proto);
    JS_FreeValue(ctx, cssstyle_proto);
    document_proto = JS_UNINITIALIZED;
    element_proto  = JS_UNINITIALIZED;
    event_proto    = JS_UNINITIALIZED;
    nodelist_proto = JS_UNINITIALIZED;
    cssstyle_proto = JS_UNINITIALIZED;
    s_document = nullptr;
}

} // namespace bro::js
