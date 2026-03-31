#include "js/dom_bindings.h"
#include "js/runtime.h"
#include "js/event_dispatch.h"
#include "js/image_bindings.h"
#include "util/log.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/text_node.h"
#include "dom/comment_node.h"
#include "dom/event.h"

#include <litehtml.h>
#include <litehtml/html.h>
#include <litehtml/el_text.h>
#include <litehtml/render_item.h>
#include "layout/bro_el_text.h"

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <unordered_map>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// ===========================================================================
// Class IDs
// ===========================================================================

static JSClassID js_document_class_id = 0;
static JSClassID js_element_class_id  = 0;
static JSClassID js_node_class_id    = 0;  // generic Node wrapper (comments, text)
static JSClassID js_event_class_id    = 0;
static JSClassID js_nodelist_class_id = 0;
static JSClassID js_cssstyle_class_id = 0;
static JSClassID js_computed_class_id = 0;

// ===========================================================================
// Per-context state (supports multiple JSContexts on the same runtime)
// ===========================================================================

// Track whether JS classes have been registered on a given runtime.
static std::unordered_map<JSRuntime*, bool> s_classes_registered;

// Per-context Document pointer so element callbacks (appendChild etc.)
// can manage orphan ownership without a single static.
static std::unordered_map<JSContext*, bro::dom::Document*> s_ctx_documents;

static bro::dom::Document* getDocumentForCtx(JSContext* ctx) {
    auto it = s_ctx_documents.find(ctx);
    return it != s_ctx_documents.end() ? it->second : nullptr;
}

// Per-context factory callback for element.getContext()
static std::unordered_map<JSContext*, DomBindings::GetContextFactory> s_ctx_factories;

void DomBindings::setGetContextFactory(JSContext* ctx, GetContextFactory factory) {
    s_ctx_factories[ctx] = std::move(factory);
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
// Generic Node wrapper (comment nodes, text nodes in tree ops)
// ===========================================================================

static JSClassDef js_node_class = {
    "Node",
    nullptr,  // finalizer — Node lifetime managed by Document
    nullptr, nullptr, nullptr
};

// Unwrap a Node* from either js_element_class_id or js_node_class_id.
static bro::dom::Node* unwrapNode(JSContext* ctx, JSValueConst val)
{
    void* ptr = JS_GetOpaque(val, js_element_class_id);
    if (ptr) return static_cast<bro::dom::Node*>(static_cast<bro::dom::Element*>(ptr));
    ptr = JS_GetOpaque(val, js_node_class_id);
    if (ptr) return static_cast<bro::dom::Node*>(ptr);
    return nullptr;
}

// Wrap any Node as a JS value. Elements get the full Element wrapper.
// Comment and text nodes get a generic Node wrapper with basic properties.
static JSValue wrapAnyNode(JSContext* ctx, bro::dom::Node* node)
{
    if (!node) return JS_NULL;
    if (node->nodeType() == bro::dom::NodeType::Element)
        return DomBindings::wrapElement(ctx, static_cast<bro::dom::Element*>(node));

    // Check existing wrapper in __bro_node_map
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue nodeMap = JS_GetPropertyStr(ctx, global, "__bro_node_map");
    if (JS_IsUndefined(nodeMap)) {
        nodeMap = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__bro_node_map", JS_DupValue(ctx, nodeMap));
    }
    std::string key = std::to_string(node->nodeId());
    JSValue existing = JS_GetPropertyStr(ctx, nodeMap, key.c_str());
    if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) {
        JS_FreeValue(ctx, nodeMap);
        JS_FreeValue(ctx, global);
        return existing;
    }
    JS_FreeValue(ctx, existing);

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_node_class_id));
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, nodeMap);
        JS_FreeValue(ctx, global);
        return obj;
    }
    JS_SetOpaque(obj, node);

    // Set basic DOM properties
    JS_SetPropertyStr(ctx, obj, "nodeType",
        JS_NewInt32(ctx, static_cast<int32_t>(node->nodeType())));
    JS_SetPropertyStr(ctx, obj, "nodeName",
        JS_NewString(ctx, node->nodeName().c_str()));

    if (node->nodeType() == bro::dom::NodeType::Comment) {
        auto* comment = static_cast<bro::dom::CommentNode*>(node);
        JS_SetPropertyStr(ctx, obj, "nodeValue",
            JS_NewString(ctx, comment->data().c_str()));
        JS_SetPropertyStr(ctx, obj, "textContent",
            JS_NewString(ctx, comment->data().c_str()));
    } else if (node->nodeType() == bro::dom::NodeType::Text) {
        // Define nodeValue and textContent as live getter/setter pairs
        // so that Vue's `textNode.nodeValue = "..."` actually updates
        // the C++ TextNode and litehtml rendering.
        JSValue getNodeValue = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* n = static_cast<bro::dom::Node*>(JS_GetOpaque(this_val, js_node_class_id));
            if (!n || n->nodeType() != bro::dom::NodeType::Text) return JS_NULL;
            auto* tn = static_cast<bro::dom::TextNode*>(n);
            return JS_NewString(cx, tn->data().c_str());
        }, "get nodeValue", 0, JS_CFUNC_generic, 0);

        JSValue setNodeValue = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
            if (argc < 1) return JS_UNDEFINED;
            auto* n = static_cast<bro::dom::Node*>(JS_GetOpaque(this_val, js_node_class_id));
            if (!n || n->nodeType() != bro::dom::NodeType::Text) return JS_UNDEFINED;
            auto* tn = static_cast<bro::dom::TextNode*>(n);
            const char* str = JS_ToCString(cx, argv[0]);
            if (!str) return JS_UNDEFINED;
            std::string newText(str);
            JS_FreeCString(cx, str);

            if (tn->data() == newText) return JS_UNDEFINED; // no change

            tn->setData(newText);

            // Propagate to litehtml: find the matching text element and
            // update it in-place via BroElText::set_text().  If the text
            // element is a plain el_text (from initial parse), replace it
            // with a BroElText so future updates can use the fast path.
            auto* parent = n->parentNode();
            if (parent && parent->nodeType() == bro::dom::NodeType::Element) {
                auto* parentEl = static_cast<bro::dom::Element*>(parent);
                auto lhParent = parentEl->litehtmlElement();
                if (lhParent) {
                    // Find the litehtml text child by index
                    auto& siblings = parent->childNodes();
                    int textIdx = 0;
                    for (auto* sib : siblings) {
                        if (sib == n) break;
                        if (sib->nodeType() == bro::dom::NodeType::Text) textIdx++;
                    }
                    auto* htParent = dynamic_cast<litehtml::html_tag*>(lhParent.get());
                    if (htParent) {
                        auto& lhChildren = htParent->children();
                        int lhTextIdx = 0;
                        for (auto it = lhChildren.begin(); it != lhChildren.end(); ++it) {
                            if ((*it)->is_text()) {
                                if (lhTextIdx == textIdx) {
                                    // Try fast path: BroElText with set_text
                                    auto* bt = dynamic_cast<bro::layout::BroElText*>(it->get());
                                    if (bt) {
                                        bt->set_text(newText.c_str());
                                        bt->compute_styles(false);
                                    } else {
                                        // Slow path: replace el_text with BroElText
                                        auto doc = lhParent->get_document();
                                        if (doc) {
                                            auto oldEl = *it;
                                            auto newEl = std::make_shared<bro::layout::BroElText>(
                                                newText.c_str(), doc);
                                            *it = newEl;
                                            newEl->compute_styles(false);
                                            // Replace render item too
                                            auto ri = htParent->get_render_item();
                                            if (ri) {
                                                for (auto rit = ri->children().begin();
                                                     rit != ri->children().end(); ++rit) {
                                                    if (*rit && (*rit)->src_el() == oldEl) {
                                                        auto newRI = newEl->create_render_item(ri);
                                                        if (newRI) {
                                                            newRI = newRI->init();
                                                            *rit = newRI;
                                                        }
                                                        break;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    break;
                                }
                                lhTextIdx++;
                            }
                        }
                    }
                }
                parentEl->markDirty();
            }
            return JS_UNDEFINED;
        }, "set nodeValue", 1, JS_CFUNC_generic, 0);

        JSAtom nvAtom = JS_NewAtom(ctx, "nodeValue");
        JS_DefinePropertyGetSet(ctx, obj, nvAtom, getNodeValue, setNodeValue, 0);
        JS_FreeAtom(ctx, nvAtom);

        // textContent aliases nodeValue for text nodes — needs its own
        // getter/setter functions (DefinePropertyGetSet takes ownership)
        JSValue getTC = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* n = static_cast<bro::dom::Node*>(JS_GetOpaque(this_val, js_node_class_id));
            if (!n || n->nodeType() != bro::dom::NodeType::Text) return JS_NULL;
            auto* tn = static_cast<bro::dom::TextNode*>(n);
            return JS_NewString(cx, tn->data().c_str());
        }, "get textContent", 0, JS_CFUNC_generic, 0);
        // textContent setter — delegate to nodeValue setter logic
        // by looking up nodeValue descriptor and calling its setter
        JSValue setTC = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
            // Get the nodeValue setter and call it (same logic)
            JSAtom nvAtom2 = JS_NewAtom(cx, "nodeValue");
            JS_SetProperty(cx, this_val, nvAtom2, JS_DupValue(cx, argv[0]));
            JS_FreeAtom(cx, nvAtom2);
            return JS_UNDEFINED;
        }, "set textContent", 1, JS_CFUNC_generic, 0);
        JSAtom tcAtom = JS_NewAtom(ctx, "textContent");
        JS_DefinePropertyGetSet(ctx, obj, tcAtom, getTC, setTC, 0);
        JS_FreeAtom(ctx, tcAtom);
    }

    // Define parentNode/nextSibling/previousSibling as live getters
    // so they reflect the current tree state (not stale creation-time values).
    // We capture the Node* via closure using a CFunction with opaque magic.
    // Simplest approach: use defineProperty with a getter function.
    {
        // parentNode getter
        JSValue getParent = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* n = static_cast<bro::dom::Node*>(JS_GetOpaque(this_val, js_node_class_id));
            if (!n || !n->parentNode()) return JS_NULL;
            if (n->parentNode()->nodeType() == bro::dom::NodeType::Element)
                return DomBindings::wrapElement(cx, static_cast<bro::dom::Element*>(n->parentNode()));
            return wrapAnyNode(cx, n->parentNode());
        }, "get parentNode", 0, JS_CFUNC_generic, 0);

        JSValue getNextSibling = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* n = static_cast<bro::dom::Node*>(JS_GetOpaque(this_val, js_node_class_id));
            if (!n || !n->parentNode()) return JS_NULL;
            auto& kids = n->parentNode()->childNodes();
            for (size_t i = 0; i < kids.size(); ++i) {
                if (kids[i] == n && i + 1 < kids.size())
                    return wrapAnyNode(cx, kids[i + 1]);
            }
            return JS_NULL;
        }, "get nextSibling", 0, JS_CFUNC_generic, 0);

        JSValue getPrevSibling = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* n = static_cast<bro::dom::Node*>(JS_GetOpaque(this_val, js_node_class_id));
            if (!n || !n->parentNode()) return JS_NULL;
            auto& kids = n->parentNode()->childNodes();
            for (size_t i = 0; i < kids.size(); ++i) {
                if (kids[i] == n) {
                    if (i > 0) return wrapAnyNode(cx, kids[i - 1]);
                    return JS_NULL;
                }
            }
            return JS_NULL;
        }, "get previousSibling", 0, JS_CFUNC_generic, 0);

        JSAtom parentAtom = JS_NewAtom(ctx, "parentNode");
        JSAtom nextAtom = JS_NewAtom(ctx, "nextSibling");
        JSAtom prevAtom = JS_NewAtom(ctx, "previousSibling");
        JS_DefinePropertyGetSet(ctx, obj, parentAtom, getParent, JS_UNDEFINED, 0);
        JS_DefinePropertyGetSet(ctx, obj, nextAtom, getNextSibling, JS_UNDEFINED, 0);
        JS_DefinePropertyGetSet(ctx, obj, prevAtom, getPrevSibling, JS_UNDEFINED, 0);
        JS_FreeAtom(ctx, parentAtom);
        JS_FreeAtom(ctx, nextAtom);
        JS_FreeAtom(ctx, prevAtom);
    }

    // Cache the wrapper
    JS_SetPropertyStr(ctx, nodeMap, key.c_str(), JS_DupValue(ctx, obj));

    JS_FreeValue(ctx, nodeMap);
    JS_FreeValue(ctx, global);
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

    // Skip exotic lookup for properties handled by the prototype function list.
    // Without this, the exotic getter shadows prototype methods like setProperty().
    if (nameStr == "cssText" || nameStr == "setProperty" ||
        nameStr == "getPropertyValue" || nameStr == "removeProperty" ||
        nameStr == "length") return 0;

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
// ComputedStyleDeclaration (read-only, backed by litehtml css_properties)
// ===========================================================================

// Helper: convert a litehtml css_length to a CSS string
static std::string cssLengthToString(const litehtml::css_length& len) {
    if (len.is_predefined()) return "auto";
    if (len.units() == litehtml::css_units_percentage) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4g%%", len.val());
        return buf;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4gpx", len.val());
    return buf;
}

// Helper: convert a litehtml web_color to a CSS rgb/rgba string
static std::string webColorToString(const litehtml::web_color& c) {
    if (c.alpha == 255) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "rgb(%d, %d, %d)", c.red, c.green, c.blue);
        return buf;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "rgba(%d, %d, %d, %.4g)",
                  c.red, c.green, c.blue, c.alpha / 255.0);
    return buf;
}

// Helper: convert a litehtml border_style enum to CSS string
static std::string borderStyleToString(litehtml::border_style bs) {
    return litehtml::index_value(bs, border_style_strings);
}

// Get a computed CSS property value from a litehtml element
static std::string getComputedProperty(bro::dom::Element* el, const std::string& prop) {
    auto lhEl = el->litehtmlElement();
    if (!lhEl) return "";

    const auto& css = lhEl->css();

    // Display & layout
    if (prop == "display")    return litehtml::index_value(css.get_display(), style_display_strings);
    if (prop == "position")   return litehtml::index_value(css.get_position(), element_position_strings);
    if (prop == "visibility") return litehtml::index_value(css.get_visibility(), visibility_strings);
    if (prop == "overflow")   return litehtml::index_value(css.get_overflow(), overflow_strings);
    if (prop == "float")      return litehtml::index_value(css.get_float(), element_float_strings);
    if (prop == "clear")      return litehtml::index_value(css.get_clear(), element_clear_strings);
    if (prop == "box-sizing") return litehtml::index_value(css.get_box_sizing(), box_sizing_strings);
    if (prop == "text-align") return litehtml::index_value(css.get_text_align(), text_align_strings);
    if (prop == "white-space") return litehtml::index_value(css.get_white_space(), white_space_strings);
    if (prop == "vertical-align") return litehtml::index_value(css.get_vertical_align(), vertical_align_strings);
    if (prop == "text-transform") return litehtml::index_value(css.get_text_transform(), text_transform_strings);

    // Dimensions
    if (prop == "width")      return cssLengthToString(css.get_width());
    if (prop == "height")     return cssLengthToString(css.get_height());
    if (prop == "min-width")  return cssLengthToString(css.get_min_width());
    if (prop == "min-height") return cssLengthToString(css.get_min_height());
    if (prop == "max-width")  return cssLengthToString(css.get_max_width());
    if (prop == "max-height") return cssLengthToString(css.get_max_height());

    // Margins
    if (prop == "margin-top")    return cssLengthToString(css.get_margins().top);
    if (prop == "margin-right")  return cssLengthToString(css.get_margins().right);
    if (prop == "margin-bottom") return cssLengthToString(css.get_margins().bottom);
    if (prop == "margin-left")   return cssLengthToString(css.get_margins().left);

    // Padding
    if (prop == "padding-top")    return cssLengthToString(css.get_padding().top);
    if (prop == "padding-right")  return cssLengthToString(css.get_padding().right);
    if (prop == "padding-bottom") return cssLengthToString(css.get_padding().bottom);
    if (prop == "padding-left")   return cssLengthToString(css.get_padding().left);

    // Borders (width)
    if (prop == "border-top-width")    return cssLengthToString(css.get_borders().top.width);
    if (prop == "border-right-width")  return cssLengthToString(css.get_borders().right.width);
    if (prop == "border-bottom-width") return cssLengthToString(css.get_borders().bottom.width);
    if (prop == "border-left-width")   return cssLengthToString(css.get_borders().left.width);

    // Borders (style)
    if (prop == "border-top-style")    return borderStyleToString(css.get_borders().top.style);
    if (prop == "border-right-style")  return borderStyleToString(css.get_borders().right.style);
    if (prop == "border-bottom-style") return borderStyleToString(css.get_borders().bottom.style);
    if (prop == "border-left-style")   return borderStyleToString(css.get_borders().left.style);

    // Borders (color)
    if (prop == "border-top-color")    return webColorToString(css.get_borders().top.color);
    if (prop == "border-right-color")  return webColorToString(css.get_borders().right.color);
    if (prop == "border-bottom-color") return webColorToString(css.get_borders().bottom.color);
    if (prop == "border-left-color")   return webColorToString(css.get_borders().left.color);

    // Colors
    if (prop == "color")            return webColorToString(css.get_color());
    if (prop == "background-color") {
        const auto& bg = css.get_bg();
        return webColorToString(bg.m_color);
    }

    // Font
    if (prop == "font-size") {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4gpx", static_cast<double>(css.get_font_size()));
        return buf;
    }
    if (prop == "line-height") {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4gpx", static_cast<double>(css.line_height().computed_value));
        return buf;
    }

    // Positioning offsets
    if (prop == "top")    return cssLengthToString(css.get_offsets().top);
    if (prop == "right")  return cssLengthToString(css.get_offsets().right);
    if (prop == "bottom") return cssLengthToString(css.get_offsets().bottom);
    if (prop == "left")   return cssLengthToString(css.get_offsets().left);

    // z-index
    if (prop == "z-index") {
        int z = css.get_z_index();
        return std::to_string(z);
    }

    // Flex
    if (prop == "flex-grow") {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.4g", css.get_flex_grow()); return buf;
    }
    if (prop == "flex-shrink") {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.4g", css.get_flex_shrink()); return buf;
    }
    if (prop == "flex-basis") return cssLengthToString(css.get_flex_basis());
    if (prop == "order") return std::to_string(css.get_order());

    // Cursor
    if (prop == "cursor") return css.get_cursor();

    // Transition properties — return empty for now (litehtml doesn't compute these)
    // but returning "" lets Vue know there's no transition rather than erroring
    if (prop == "transition-duration" || prop == "transition-delay" ||
        prop == "transition-property" || prop == "transition-timing-function" ||
        prop == "animation-duration" || prop == "animation-delay" ||
        prop == "animation-name") {
        return "0s";
    }

    // Fall through to inline styles for properties we don't handle
    return el->style().getProperty(prop);
}

// Exotic methods for ComputedStyleDeclaration — read-only property access
static int js_computed_get_own_property(JSContext* ctx,
                                        JSPropertyDescriptor* desc,
                                        JSValueConst obj, JSAtom prop)
{
    auto* el = static_cast<bro::dom::Element*>(
        JS_GetOpaque(obj, js_computed_class_id));
    if (!el) return 0;

    // JS_AtomToCString returns NULL for Symbol atoms, handled below
    const char* name = JS_AtomToCString(ctx, prop);
    if (!name) return 0;
    std::string nameStr(name);
    JS_FreeCString(ctx, name);

    if (nameStr.empty()) return 0;

    // Skip JS builtins and prototype methods that could cause recursion
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

    // Skip numeric, underscore/dollar prefixed, and non-alpha-start names
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
    "CSSStyleDeclaration",  // same name as browser uses for computed styles
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
    // Computed styles are read-only — silently ignore
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_computed_proto_funcs[] = {
    JS_CFUNC_DEF("getPropertyValue", 1, js_computed_getPropertyValue),
    JS_CFUNC_DEF("setProperty",      2, js_computed_setProperty),
};

// window.getComputedStyle(element) — returns a ComputedStyleDeclaration
static JSValue js_window_getComputedStyle(JSContext* ctx,
                                          JSValueConst /*this_val*/,
                                          int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_NULL;

    // Try to get Element from the JS value
    auto* el = static_cast<bro::dom::Element*>(
        JS_GetOpaque(argv[0], js_element_class_id));

    if (!el) {
        // Return minimal stub for non-element values
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

static JSValue wrapTokenList(JSContext* ctx, bro::dom::Element* elem) {
    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_tokenlist_class_id));
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, elem);
    return obj;
}

// ===========================================================================
// Element wrapper
// ===========================================================================

// Release orphaned elements when the JS wrapper is garbage-collected.
// Elements that are in the live DOM tree have their lifetime managed by
// the Document.  But elements that were never inserted (e.g. temporary
// DocumentFragments created by jQuery) need to be freed here.
static void js_element_finalizer(JSRuntime* /*rt*/, JSValue val)
{
    auto* el = static_cast<bro::dom::Element*>(
        JS_GetOpaque(val, js_element_class_id));
    if (!el || !el->isAlive()) return;

    // Only free if the element is NOT in the DOM tree (no parent).
    // Elements in the tree are freed by removeChild → freeNode.
    if (!el->parentNode()) {
        auto* doc = el->document();
        if (doc) doc->freeNode(el);
    }
}

static JSClassDef js_element_class = {
    "Element",
    js_element_finalizer, nullptr, nullptr, nullptr
};

static inline bro::dom::Element* getElement(JSValueConst val)
{
    auto* el = static_cast<bro::dom::Element*>(
        JS_GetOpaque(val, js_element_class_id));
    if (el && !el->isAlive()) {
        LOG_ERROR("USE-AFTER-FREE: Element %u (tag=%s) accessed after destruction!",
                  el->nodeId(), "(freed)");
        return nullptr;  // prevent crash
    }
    return el;
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

// Invalidate a JS wrapper by nulling its opaque pointer and removing from elem map.
// After this, getElement() returns nullptr for stale wrappers (canary check).
// Must be called BEFORE the C++ Element is freed.
static void invalidateWrapper(JSContext* ctx, bro::dom::Element* elem) {
    if (!elem) return;

    // Recurse into children first
    for (auto& child : elem->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element)
            invalidateWrapper(ctx, static_cast<bro::dom::Element*>(child));
    }

    auto* ctxDoc = getDocumentForCtx(ctx);
    if (ctxDoc) {
        if (!elem->id().empty())
            ctxDoc->unregisterElementId(elem->id());
    }

    // Find and null-out the JS wrapper
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (!JS_IsUndefined(elemMap)) {
        std::string key = std::to_string(elem->nodeId());
        JSValue wrapper = JS_GetPropertyStr(ctx, elemMap, key.c_str());
        if (!JS_IsUndefined(wrapper) && !JS_IsNull(wrapper)) {
            // Null the opaque pointer so getElement() returns nullptr
            JS_SetOpaque(wrapper, nullptr);
            JS_FreeValue(ctx, wrapper);
        }
        // Delete from map
        JSAtom atom = JS_NewAtom(ctx, key.c_str());
        JS_DeleteProperty(ctx, elemMap, atom, 0);
        JS_FreeAtom(ctx, atom);
    }
    JS_FreeValue(ctx, elemMap);
    JS_FreeValue(ctx, global);
}

static JSValue js_element_set_textContent(JSContext* ctx, JSValueConst this_val,
                                          JSValueConst val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    // Invalidate JS wrappers for children that will be freed
    for (auto& child : el->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element)
            invalidateWrapper(ctx, static_cast<bro::dom::Element*>(child));
    }
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
    for (auto& child : el->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element)
            invalidateWrapper(ctx, static_cast<bro::dom::Element*>(child));
    }
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
    auto* el = getElement(this_val);
    if (!el) return JS_NewArray(ctx);

    // childNodes returns ALL child nodes (elements, text, comments).
    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    for (auto* child : el->childNodes()) {
        JS_SetPropertyUint32(ctx, arr, idx++, wrapAnyNode(ctx, child));
    }

    // Add a length property for NodeList compatibility
    JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, static_cast<int32_t>(idx)));

    return arr;
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
// Wrap a single Node as a JS value. For Elements, returns the cached wrapper.
// For text nodes, creates a minimal object with nodeType + nodeValue.
// Does NOT set nextSibling/previousSibling (caller sets if needed).
static JSValue wrapSingleChild(JSContext* ctx, bro::dom::Node* node,
                                bro::dom::Node* /*parentNode*/ = nullptr) {
    return wrapAnyNode(ctx, node);
}

// Find node's index in parent's childNodes
static int findChildIndex(bro::dom::Node* node) {
    if (!node || !node->parentNode()) return -1;
    auto& kids = node->parentNode()->childNodes();
    for (size_t i = 0; i < kids.size(); ++i) {
        if (kids[i] == node) return static_cast<int>(i);
    }
    return -1;
}

// Get next sibling as a wrapped JS value with its own nextSibling set
// to enable jQuery's firstChild→nextSibling chain traversal.
// Only creates 1 wrapper per call (the immediate sibling).
// Wrap child at index. wrapAnyNode already defines lazy nextSibling/
// previousSibling getters for non-Element nodes, so no eager recursion needed.
static JSValue wrapChildAtIndex(JSContext* ctx, bro::dom::Node* parent, size_t idx) {
    if (!parent) return JS_NULL;
    auto& kids = parent->childNodes();
    if (idx >= kids.size()) return JS_NULL;
    return wrapAnyNode(ctx, kids[idx]);
}

static JSValue js_element_get_firstChild(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el || el->childNodes().empty()) return JS_NULL;
    return wrapChildAtIndex(ctx, el, 0);
}

static JSValue js_element_get_lastChild(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el || el->childNodes().empty()) return JS_NULL;
    return wrapChildAtIndex(ctx, el, el->childNodes().size() - 1);
}

static JSValue js_element_get_nextSibling(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el || !el->parentNode()) return JS_NULL;
    int idx = findChildIndex(el);
    if (idx < 0) return JS_NULL;
    return wrapChildAtIndex(ctx, el->parentNode(), static_cast<size_t>(idx + 1));
}

static JSValue js_element_get_previousSibling(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el || !el->parentNode()) return JS_NULL;
    int idx = findChildIndex(el);
    if (idx <= 0) return JS_NULL;
    return wrapChildAtIndex(ctx, el->parentNode(), static_cast<size_t>(idx - 1));
}

static JSValue js_element_get_nextElementSibling(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el || !el->parentNode()) return JS_NULL;
    auto& siblings = el->parentNode()->childNodes();
    bool found = false;
    for (size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i] == el) { found = true; continue; }
        if (found && siblings[i]->nodeType() == bro::dom::NodeType::Element)
            return DomBindings::wrapElement(ctx, static_cast<bro::dom::Element*>(siblings[i]));
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
        if (siblings[i] == el) return lastElem ? DomBindings::wrapElement(ctx, lastElem) : JS_NULL;
        if (siblings[i]->nodeType() == bro::dom::NodeType::Element)
            lastElem = static_cast<bro::dom::Element*>(siblings[i]);
    }
    return JS_NULL;
}

static JSValue js_element_get_firstElementChild(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_NULL;
    for (auto& child : el->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element)
            return DomBindings::wrapElement(ctx, static_cast<bro::dom::Element*>(child));
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
            return DomBindings::wrapElement(ctx, static_cast<bro::dom::Element*>(*it));
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

// ---- Form control properties (value, checked, type, disabled, placeholder) ----

static JSValue js_element_get_value(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    return JS_NewString(ctx, el->getAttribute("value").c_str());
}

static JSValue js_element_set_value(JSContext* ctx, JSValueConst this_val,
                                    JSValueConst val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    el->setAttribute("value", jsToStdString(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_element_get_checked(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_FALSE;
    // "checked" is a boolean attribute — presence in map means true
    // (value can be "" for <input checked> or "checked" for setAttribute)
    return JS_NewBool(ctx, el->attributes().count("checked") > 0);
}

static JSValue js_element_set_checked(JSContext* ctx, JSValueConst this_val,
                                      JSValueConst val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    if (JS_ToBool(ctx, val))
        el->setAttribute("checked", "");
    else
        el->removeAttribute("checked");
    return JS_UNDEFINED;
}

static JSValue js_element_get_type(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    std::string t = el->getAttribute("type");
    if (t.empty()) t = "text"; // default input type
    return JS_NewString(ctx, t.c_str());
}

static JSValue js_element_set_type(JSContext* ctx, JSValueConst this_val,
                                   JSValueConst val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    el->setAttribute("type", jsToStdString(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_element_get_disabled(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_FALSE;
    return JS_NewBool(ctx, !el->getAttribute("disabled").empty());
}

static JSValue js_element_set_disabled(JSContext* ctx, JSValueConst this_val,
                                       JSValueConst val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    if (JS_ToBool(ctx, val))
        el->setAttribute("disabled", "disabled");
    else
        el->removeAttribute("disabled");
    return JS_UNDEFINED;
}

static JSValue js_element_get_placeholder(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    return JS_NewString(ctx, el->getAttribute("placeholder").c_str());
}

static JSValue js_element_set_placeholder(JSContext* ctx, JSValueConst this_val,
                                          JSValueConst val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    el->setAttribute("placeholder", jsToStdString(ctx, val));
    return JS_UNDEFINED;
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

static JSValue js_element_appendChild(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_UNDEFINED;
    auto* child = unwrapNode(ctx, argv[0]);
    if (child) {
        auto* doc = getDocumentForCtx(ctx);
        // DocumentFragment: move all children to parent instead
        if (child->nodeName() == "#DOCUMENT-FRAGMENT" ||
            child->nodeType() == bro::dom::NodeType::DocumentFragment) {
            auto kids = child->childNodes();
            for (auto* kid : kids) {
                el->appendChild(kid);
                if (doc && kid->nodeType() == bro::dom::NodeType::Element)
                    doc->syncAppendToLitehtml(static_cast<bro::dom::Element*>(kid), el);
            }
        } else {
            el->appendChild(child);
            if (doc && child->nodeType() == bro::dom::NodeType::Element) {
                doc->syncAppendToLitehtml(static_cast<bro::dom::Element*>(child), el);
            }
        }
    }
    return argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
}

static JSValue js_element_removeChild(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_UNDEFINED;
    auto* child = unwrapNode(ctx, argv[0]);
    if (child) {
        auto* doc = getDocumentForCtx(ctx);
        if (child->nodeType() == bro::dom::NodeType::Element) {
            auto* childElem = static_cast<bro::dom::Element*>(child);
            if (doc && !childElem->id().empty())
                doc->unregisterElementId(childElem->id());
            if (doc) doc->syncRemoveFromLitehtml(childElem, el);
            invalidateWrapper(ctx, childElem);
        }
        el->removeChild(child);
        if (doc) doc->freeNode(child);
    }
    return argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
}

static JSValue js_element_insertBefore(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 2) return JS_UNDEFINED;
    auto* newChild = unwrapNode(ctx, argv[0]);
    bro::dom::Node* refChild = nullptr;
    if (!JS_IsNull(argv[1])) {
        refChild = unwrapNode(ctx, argv[1]);
    }
    if (newChild) {
        el->insertBefore(newChild, refChild);
        auto* doc = getDocumentForCtx(ctx);
        if (doc && newChild->nodeType() == bro::dom::NodeType::Element) {
            auto* newElem = static_cast<bro::dom::Element*>(newChild);
            if (refChild && refChild->nodeType() == bro::dom::NodeType::Element) {
                doc->syncInsertBeforeLitehtml(newElem,
                    static_cast<bro::dom::Element*>(refChild), el);
            } else {
                doc->syncAppendToLitehtml(newElem, el);
            }
        }
    }
    return argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
}

static JSValue js_element_replaceChild(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 2) return JS_UNDEFINED;
    auto* newChild = unwrapNode(ctx, argv[0]);
    auto* oldChild = unwrapNode(ctx, argv[1]);
    if (newChild && oldChild) {
        auto* doc = getDocumentForCtx(ctx);
        // Sync: insert new before old, then remove old
        el->insertBefore(newChild, oldChild);
        if (doc && newChild->nodeType() == bro::dom::NodeType::Element &&
            oldChild->nodeType() == bro::dom::NodeType::Element) {
            doc->syncInsertBeforeLitehtml(
                static_cast<bro::dom::Element*>(newChild),
                static_cast<bro::dom::Element*>(oldChild), el);
        }
        if (oldChild->nodeType() == bro::dom::NodeType::Element) {
            auto* oldElem = static_cast<bro::dom::Element*>(oldChild);
            if (doc && !oldElem->id().empty())
                doc->unregisterElementId(oldElem->id());
            if (doc) doc->syncRemoveFromLitehtml(oldElem, el);
            invalidateWrapper(ctx, oldElem);
        }
        el->removeChild(oldChild);
        if (doc) doc->freeNode(oldChild);
    }
    return argc >= 2 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
}

static JSValue js_element_cloneNode(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    auto* doc = getDocumentForCtx(ctx);
    if (!el || !doc) return JS_NULL;

    bool deep = false;
    if (argc >= 1) deep = JS_ToBool(ctx, argv[0]);

    // Create a new element with the same tag
    auto* clone = doc->createElement(el->tagName());
    if (!clone) return JS_NULL;

    // Copy all attributes (skip "id" — cloned nodes must not duplicate IDs)
    for (auto& [name, val] : el->attributes()) {
        if (name == "id") continue;
        clone->setAttribute(name, val);
    }

    if (deep) {
        // Deep clone: recursively clone children
        for (auto* child : el->childNodes()) {
            if (child->nodeType() == bro::dom::NodeType::Element) {
                // Wrap child, call cloneNode(true), unwrap, appendChild
                JSValue childJs = DomBindings::wrapElement(ctx, static_cast<bro::dom::Element*>(child));
                JSValue trueVal = JS_TRUE;
                JSValue clonedJs = js_element_cloneNode(ctx, childJs, 1, &trueVal);
                auto* clonedElem = static_cast<bro::dom::Element*>(DomBindings::unwrapElement(ctx, clonedJs));
                if (clonedElem) {
                    clone->appendChild(clonedElem);
                }
                JS_FreeValue(ctx, clonedJs);
                JS_FreeValue(ctx, childJs);
            } else if (child->nodeType() == bro::dom::NodeType::Text) {
                auto* textNode = static_cast<bro::dom::TextNode*>(child);
                auto* clonedText = doc->createTextNode(textNode->data());
                clone->appendChild(clonedText);
            }
        }
    }

    return DomBindings::wrapElement(ctx, clone);
}

static JSValue js_element_remove(JSContext* ctx, JSValueConst this_val,
                                 int /*argc*/, JSValueConst* /*argv*/)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* parent = el->parentNode();
    if (parent) {
        auto* doc = getDocumentForCtx(ctx);
        if (doc && !el->id().empty())
            doc->unregisterElementId(el->id());
        if (doc && parent->nodeType() == bro::dom::NodeType::Element) {
            doc->syncRemoveFromLitehtml(
                el, static_cast<bro::dom::Element*>(parent));
        }
        invalidateWrapper(ctx, el);
        parent->removeChild(el);
        if (doc) doc->freeNode(el);
    }
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
        // Reuse the already-allocated atom (JS_SetProperty consumes a ref).
        JS_SetProperty(ctx, this_val, JS_DupAtom(ctx, key), JS_DupValue(ctx, arr));
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
                // Compare callback identity — matches if it's the same JS function ref.
                // JS_VALUE_GET_PTR compares the underlying object pointer.
                bool same = JS_IsFunction(ctx, cb) &&
                            JS_VALUE_GET_PTR(cb) == JS_VALUE_GET_PTR(argv[1]);
                JS_FreeValue(ctx, cb);
                if (same) {
                    JS_SetPropertyInt64(ctx, arr, i, JS_UNDEFINED);
                    JS_FreeValue(ctx, entry);
                    break;
                }
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
    auto factoryIt = s_ctx_factories.find(ctx);
    if (factoryIt != s_ctx_factories.end() && factoryIt->second) {
        return factoryIt->second(ctx, el, type);
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

// --- Layout measurement (reads from litehtml render items) ---

static litehtml::position getLayoutPos(bro::dom::Element* el) {
    litehtml::position pos = {0, 0, 0, 0};
    if (!el) return pos;
    auto lh = el->litehtmlElement();
    if (!lh) return pos;
    auto ri = lh->get_render_item();
    if (!ri) return pos;
    pos = ri->pos();
    return pos;
}

static JSValue js_element_get_clientWidth(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    auto pos = getLayoutPos(el);
    if (pos.width > 0) return JS_NewInt32(ctx, pos.width);
    // Fallback to attribute-based width for canvas etc
    return js_element_get_width(ctx, this_val);
}

static JSValue js_element_get_clientHeight(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    auto pos = getLayoutPos(el);
    if (pos.height > 0) return JS_NewInt32(ctx, pos.height);
    return js_element_get_height(ctx, this_val);
}

static JSValue js_element_get_offsetWidth(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    auto pos = getLayoutPos(el);
    return JS_NewInt32(ctx, pos.width);
}

static JSValue js_element_get_offsetHeight(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    auto pos = getLayoutPos(el);
    return JS_NewInt32(ctx, pos.height);
}

static JSValue js_element_get_offsetLeft(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    auto pos = getLayoutPos(el);
    return JS_NewInt32(ctx, pos.x);
}

static JSValue js_element_get_offsetTop(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    auto pos = getLayoutPos(el);
    return JS_NewInt32(ctx, pos.y);
}

static JSValue js_element_get_scrollWidth(JSContext* ctx, JSValueConst this_val) {
    return js_element_get_offsetWidth(ctx, this_val); // no scroll support yet
}

static JSValue js_element_get_scrollHeight(JSContext* ctx, JSValueConst this_val) {
    return js_element_get_offsetHeight(ctx, this_val);
}

static JSValue js_element_get_scrollLeft(JSContext* ctx, JSValueConst /*this_val*/) {
    (void)ctx;
    return JS_NewInt32(ctx, 0);
}

static JSValue js_element_get_scrollTop(JSContext* ctx, JSValueConst /*this_val*/) {
    (void)ctx;
    return JS_NewInt32(ctx, 0);
}

static JSValue js_element_set_scrollLeft(JSContext* /*ctx*/, JSValueConst /*this_val*/, JSValueConst /*val*/) {
    return JS_UNDEFINED; // stub
}

static JSValue js_element_set_scrollTop(JSContext* /*ctx*/, JSValueConst /*this_val*/, JSValueConst /*val*/) {
    return JS_UNDEFINED; // stub
}

static JSValue js_element_getBoundingClientRect(JSContext* ctx, JSValueConst this_val,
                                                 int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    auto pos = getLayoutPos(el);

    JSValue rect = JS_NewObject(ctx);
    float x = static_cast<float>(pos.x);
    float y = static_cast<float>(pos.y);
    float w = static_cast<float>(pos.width);
    float h = static_cast<float>(pos.height);
    JS_SetPropertyStr(ctx, rect, "x",      JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, rect, "y",      JS_NewFloat64(ctx, y));
    JS_SetPropertyStr(ctx, rect, "width",  JS_NewFloat64(ctx, w));
    JS_SetPropertyStr(ctx, rect, "height", JS_NewFloat64(ctx, h));
    JS_SetPropertyStr(ctx, rect, "top",    JS_NewFloat64(ctx, y));
    JS_SetPropertyStr(ctx, rect, "left",   JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, rect, "bottom", JS_NewFloat64(ctx, y + h));
    JS_SetPropertyStr(ctx, rect, "right",  JS_NewFloat64(ctx, x + w));
    return rect;
}

// --- dispatchEvent ---

static JSValue js_element_dispatchEvent(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_FALSE;

    // Read event type from JS event object
    JSValue typeVal = JS_GetPropertyStr(ctx, argv[0], "type");
    const char* typeStr = JS_ToCString(ctx, typeVal);
    JS_FreeValue(ctx, typeVal);
    if (!typeStr) return JS_FALSE;

    std::string type = typeStr;
    JS_FreeCString(ctx, typeStr);

    // Read bubbles/cancelable
    JSValue bubblesVal = JS_GetPropertyStr(ctx, argv[0], "bubbles");
    bool bubbles = JS_ToBool(ctx, bubblesVal);
    JS_FreeValue(ctx, bubblesVal);

    JSValue cancelableVal = JS_GetPropertyStr(ctx, argv[0], "cancelable");
    bool cancelable = JS_ToBool(ctx, cancelableVal);
    JS_FreeValue(ctx, cancelableVal);

    // Create C++ event and dispatch through the DOM
    bro::dom::Event evt(type, bubbles, cancelable);

    // Copy any detail property from CustomEvent
    // (the JS event object is passed through to listeners by event_dispatch)

    // Use the existing dispatch mechanism
    bro::js::dispatchDomEvent(ctx, el, evt);

    return JS_NewBool(ctx, !evt.defaultPrevented());
}

// --- focus / blur ---

static JSValue js_element_focus(JSContext* ctx, JSValueConst this_val,
                                int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* doc = getDocumentForCtx(ctx);
    if (doc) doc->setActiveElement(el);
    return JS_UNDEFINED;
}

static JSValue js_element_blur(JSContext* ctx, JSValueConst this_val,
                               int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* doc = getDocumentForCtx(ctx);
    if (doc && doc->activeElement() == el) {
        doc->setActiveElement(nullptr);
    }
    return JS_UNDEFINED;
}

// --- outerHTML ---

static JSValue js_element_get_outerHTML(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    // Build outerHTML: opening tag + innerHTML + closing tag
    std::string tag = el->tagName();
    // Lowercase the tag
    for (auto& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string result = "<" + tag;
    for (auto& [name, val] : el->attributes()) {
        result += " " + name + "=\"" + val + "\"";
    }
    result += ">";
    result += el->innerHTML();
    result += "</" + tag + ">";
    return JS_NewString(ctx, result.c_str());
}

// --- insertAdjacentHTML ---

static JSValue js_element_insertAdjacentHTML(JSContext* ctx, JSValueConst this_val,
                                              int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el || argc < 2) return JS_UNDEFINED;

    const char* position = JS_ToCString(ctx, argv[0]);
    const char* html = JS_ToCString(ctx, argv[1]);
    if (!position || !html) {
        JS_FreeCString(ctx, position);
        JS_FreeCString(ctx, html);
        return JS_UNDEFINED;
    }

    std::string pos(position);
    JS_FreeCString(ctx, position);

    // Use a temporary element to parse the HTML
    auto* doc = el->document();
    if (!doc) { JS_FreeCString(ctx, html); return JS_UNDEFINED; }

    // Create a wrapper element, set innerHTML, then move children
    auto* temp = doc->createElement("div");
    if (temp) {
        temp->setInnerHTML(html);
        auto children = temp->childNodes();
        std::vector<bro::dom::Node*> toMove(children.begin(), children.end());

        if (pos == "beforebegin") {
            auto* parent = el->parentElement();
            if (parent) {
                for (auto* child : toMove) parent->insertBefore(child, el);
            }
        } else if (pos == "afterbegin") {
            auto* first = el->childNodes().empty() ? nullptr : el->childNodes().front();
            for (auto* child : toMove) el->insertBefore(child, first);
        } else if (pos == "beforeend") {
            for (auto* child : toMove) el->appendChild(child);
        } else if (pos == "afterend") {
            auto* parent = el->parentElement();
            if (parent) {
                // Find the next sibling
                bro::dom::Node* ref = nullptr;
                auto& siblings = parent->childNodes();
                for (size_t i = 0; i < siblings.size(); ++i) {
                    if (siblings[i] == el && i + 1 < siblings.size()) {
                        ref = siblings[i + 1];
                        break;
                    }
                }
                for (auto* child : toMove) parent->insertBefore(child, ref);
            }
        }
        // temp is now empty, will be cleaned up by document
    }

    JS_FreeCString(ctx, html);
    return JS_UNDEFINED;
}

// --- dataset proxy (data-* attributes) ---

static JSValue js_element_get_dataset(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewObject(ctx);

    JSValue obj = JS_NewObject(ctx);
    for (auto& [name, val] : el->attributes()) {
        if (name.size() > 5 && name.substr(0, 5) == "data-") {
            // Convert data-foo-bar to fooBar (camelCase)
            std::string key;
            bool capitalize = false;
            for (size_t i = 5; i < name.size(); ++i) {
                if (name[i] == '-') {
                    capitalize = true;
                } else {
                    key += capitalize
                        ? static_cast<char>(std::toupper(static_cast<unsigned char>(name[i])))
                        : name[i];
                    capitalize = false;
                }
            }
            JS_SetPropertyStr(ctx, obj, key.c_str(), JS_NewString(ctx, val.c_str()));
        }
    }
    return obj;
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
    JS_CGETSET_DEF("offsetWidth",   js_element_get_offsetWidth, nullptr),
    JS_CGETSET_DEF("offsetHeight",  js_element_get_offsetHeight, nullptr),
    JS_CGETSET_DEF("offsetLeft",    js_element_get_offsetLeft, nullptr),
    JS_CGETSET_DEF("offsetTop",     js_element_get_offsetTop, nullptr),
    JS_CGETSET_DEF("scrollWidth",   js_element_get_scrollWidth, nullptr),
    JS_CGETSET_DEF("scrollHeight",  js_element_get_scrollHeight, nullptr),
    JS_CGETSET_DEF("scrollLeft",    js_element_get_scrollLeft, js_element_set_scrollLeft),
    JS_CGETSET_DEF("scrollTop",     js_element_get_scrollTop, js_element_set_scrollTop),
    JS_CGETSET_DEF("outerHTML",     js_element_get_outerHTML, nullptr),
    JS_CGETSET_DEF("dataset",       js_element_get_dataset, nullptr),
    JS_CGETSET_DEF("ownerDocument", js_element_get_ownerDocument, nullptr),
    // Form control properties
    JS_CGETSET_DEF("value",       js_element_get_value,       js_element_set_value),
    JS_CGETSET_DEF("checked",     js_element_get_checked,     js_element_set_checked),
    JS_CGETSET_DEF("type",        js_element_get_type,        js_element_set_type),
    JS_CGETSET_DEF("disabled",    js_element_get_disabled,    js_element_set_disabled),
    JS_CGETSET_DEF("placeholder", js_element_get_placeholder, js_element_set_placeholder),
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
    JS_CFUNC_DEF("dispatchEvent",             1, js_element_dispatchEvent),
    JS_CFUNC_DEF("getBoundingClientRect",     0, js_element_getBoundingClientRect),
    JS_CFUNC_DEF("focus",                     0, js_element_focus),
    JS_CFUNC_DEF("blur",                      0, js_element_blur),
    JS_CFUNC_DEF("insertAdjacentHTML",        2, js_element_insertAdjacentHTML),
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
    auto* el = doc->createElement(tag);
    if (!el) return JS_NULL;
    return DomBindings::wrapElement(ctx, el);
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
    auto* el = doc->createElement(tag);
    if (!el) return JS_NULL;
    return DomBindings::wrapElement(ctx, el);
}

static JSValue js_document_createTextNode(JSContext* ctx,
                                          JSValueConst this_val,
                                          int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NULL;
    std::string text = jsToStdString(ctx, argv[0]);
    auto* textNode = doc->createTextNode(text);
    if (!textNode) return JS_NULL;
    return wrapAnyNode(ctx, textNode);
}

static JSValue js_document_createDocumentFragment(JSContext* ctx,
                                                  JSValueConst this_val,
                                                  int /*argc*/, JSValueConst* /*argv*/)
{
    // Model fragment as an element with tag "#DOCUMENT-FRAGMENT".
    // When appendChild receives a fragment, it moves all children.
    auto* doc = getDocument(this_val);
    if (!doc) return JS_NULL;
    auto* el = doc->createElement("#DOCUMENT-FRAGMENT");
    if (!el) return JS_NULL;
    return DomBindings::wrapElement(ctx, el);
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
    auto* doc = getDocument(this_val);
    if (!doc) return JS_NULL;
    std::string text = (argc >= 1) ? jsToStdString(ctx, argv[0]) : "";
    auto* comment = doc->createComment(text);
    if (!comment) return JS_NULL;
    return wrapAnyNode(ctx, comment);
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
                                             JSValueConst this_val,
                                             int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return wrapNodeList(ctx, {});
    std::string name = jsToStdString(ctx, argv[0]);
    auto results = doc->querySelectorAll("[name=\"" + name + "\"]");
    return wrapNodeList(ctx, results);
}

static JSValue js_document_get_defaultView(JSContext* ctx, JSValueConst /*this_val*/)
{
    // Return window (globalThis)
    return JS_GetGlobalObject(ctx);
}

static JSValue js_document_get_activeElement(JSContext* ctx, JSValueConst this_val)
{
    auto* doc = getDocument(this_val);
    if (!doc) return JS_NULL;
    auto* el = doc->activeElement();
    if (!el) return JS_NULL;
    return DomBindings::wrapElement(ctx, el);
}

static const JSCFunctionListEntry js_document_proto_funcs[] = {
    // Properties
    JS_CGETSET_DEF("title",           js_document_get_title,           js_document_set_title),
    JS_CGETSET_DEF("body",            js_document_get_body,            nullptr),
    JS_CGETSET_DEF("documentElement", js_document_get_documentElement, nullptr),
    JS_CGETSET_DEF("nodeType",        js_document_get_nodeType,        nullptr),
    JS_CGETSET_DEF("nodeName",        js_document_get_nodeName,        nullptr),
    JS_CGETSET_DEF("defaultView",     js_document_get_defaultView,     nullptr),
    JS_CGETSET_DEF("activeElement",  js_document_get_activeElement,   nullptr),
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
    return obj;
}

// ===========================================================================
// install() – register everything
// ===========================================================================

void DomBindings::install(JSContext* ctx, void* document_ptr)
{
    JSRuntime* rt = JS_GetRuntime(ctx);

    // ----- Allocate class IDs (idempotent — no-op if already non-zero) -----
    JS_NewClassID(rt, &js_document_class_id);
    JS_NewClassID(rt, &js_element_class_id);
    JS_NewClassID(rt, &js_node_class_id);
    JS_NewClassID(rt, &js_event_class_id);
    JS_NewClassID(rt, &js_nodelist_class_id);
    JS_NewClassID(rt, &js_cssstyle_class_id);
    JS_NewClassID(rt, &js_computed_class_id);
    JS_NewClassID(rt, &js_tokenlist_class_id);

    // ----- Register classes on the runtime (once per runtime) -----
    if (!s_classes_registered[rt]) {
        JS_NewClass(rt, js_document_class_id, &js_document_class);
        JS_NewClass(rt, js_element_class_id,  &js_element_class);
        JS_NewClass(rt, js_node_class_id,     &js_node_class);
        JS_NewClass(rt, js_event_class_id,    &js_event_class);
        JS_NewClass(rt, js_nodelist_class_id, &js_nodelist_class);
        JS_NewClass(rt, js_cssstyle_class_id, &js_cssstyle_class);
        JS_NewClass(rt, js_computed_class_id, &js_computed_class);
        JS_NewClass(rt, js_tokenlist_class_id, &js_tokenlist_class);
        s_classes_registered[rt] = true;
    }

    // ----- Create prototypes (per-context via JS_SetClassProto) -----

    // Document prototype
    JSValue doc_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, doc_proto, js_document_proto_funcs,
                               sizeof(js_document_proto_funcs) / sizeof(js_document_proto_funcs[0]));
    JS_SetClassProto(ctx, js_document_class_id, doc_proto);

    // Element prototype
    JSValue elem_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, elem_proto, js_element_proto_funcs,
                               sizeof(js_element_proto_funcs) / sizeof(js_element_proto_funcs[0]));
    JS_SetClassProto(ctx, js_element_class_id, elem_proto);

    // Event prototype
    JSValue evt_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, evt_proto, js_event_proto_funcs,
                               sizeof(js_event_proto_funcs) / sizeof(js_event_proto_funcs[0]));
    JS_SetClassProto(ctx, js_event_class_id, evt_proto);

    // NodeList prototype
    JSValue nl_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, nl_proto, js_nodelist_proto_funcs,
                               sizeof(js_nodelist_proto_funcs) / sizeof(js_nodelist_proto_funcs[0]));
    JS_SetClassProto(ctx, js_nodelist_class_id, nl_proto);

    // CSSStyleDeclaration prototype
    JSValue css_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, css_proto, js_cssstyle_proto_funcs,
                               sizeof(js_cssstyle_proto_funcs) / sizeof(js_cssstyle_proto_funcs[0]));
    JS_SetClassProto(ctx, js_cssstyle_class_id, css_proto);

    // ComputedStyleDeclaration prototype
    JSValue comp_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, comp_proto, js_computed_proto_funcs,
                               sizeof(js_computed_proto_funcs) / sizeof(js_computed_proto_funcs[0]));
    JS_SetClassProto(ctx, js_computed_class_id, comp_proto);

    // DOMTokenList prototype
    JSValue tl_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, tl_proto, js_tokenlist_proto_funcs,
                               sizeof(js_tokenlist_proto_funcs) / sizeof(js_tokenlist_proto_funcs[0]));
    JS_SetClassProto(ctx, js_tokenlist_class_id, tl_proto);

    // ----- Stash Document pointer for orphan management (per-context) -----
    s_ctx_documents[ctx] = static_cast<bro::dom::Document*>(document_ptr);

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

    // window.getComputedStyle is registered as a native C++ function (see below)

    // Stub DOM type constructors needed by Vue and other frameworks
    if (typeof Element === 'undefined')
        globalThis.Element = class Element {};
    if (typeof SVGElement === 'undefined')
        globalThis.SVGElement = class SVGElement {};
    if (typeof MathMLElement === 'undefined')
        globalThis.MathMLElement = class MathMLElement {};
    if (typeof HTMLElement === 'undefined')
        globalThis.HTMLElement = class HTMLElement {};

    // Event constructor (used by el.dispatchEvent(new Event('input')))
    globalThis.Event = class Event {
        constructor(type, opts) {
            this.type = type;
            this.bubbles = !!(opts && opts.bubbles);
            this.cancelable = !!(opts && opts.cancelable);
            this.composed = !!(opts && opts.composed);
            this.defaultPrevented = false;
            this.target = null;
            this.currentTarget = null;
            this.timeStamp = performance.now();
            this._stopped = false;
            this._immediateStopped = false;
        }
        preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
        stopPropagation() { this._stopped = true; }
        stopImmediatePropagation() { this._stopped = true; this._immediateStopped = true; }
    };
    globalThis.CustomEvent = class CustomEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.detail = (opts && opts.detail !== undefined) ? opts.detail : null;
        }
    };
    globalThis.MouseEvent = class MouseEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.clientX = (opts && opts.clientX) || 0;
            this.clientY = (opts && opts.clientY) || 0;
            this.button = (opts && opts.button) || 0;
        }
    };
    globalThis.KeyboardEvent = class KeyboardEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.key = (opts && opts.key) || '';
            this.code = (opts && opts.code) || '';
            this.ctrlKey = !!(opts && opts.ctrlKey);
            this.shiftKey = !!(opts && opts.shiftKey);
            this.altKey = !!(opts && opts.altKey);
            this.metaKey = !!(opts && opts.metaKey);
        }
    };
    globalThis.InputEvent = class InputEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.data = (opts && opts.data) || null;
            this.inputType = (opts && opts.inputType) || '';
        }
    };
    globalThis.FocusEvent = class FocusEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.relatedTarget = (opts && opts.relatedTarget) || null;
        }
    };

    // queueMicrotask — schedules a microtask via Promise
    if (typeof queueMicrotask === 'undefined') {
        globalThis.queueMicrotask = function(cb) {
            Promise.resolve().then(cb);
        };
    }

    // MutationObserver — simplified implementation
    // Fires callbacks asynchronously after DOM mutations via microtask
    globalThis.MutationObserver = class MutationObserver {
        constructor(callback) {
            this._callback = callback;
            this._targets = [];
            this._records = [];
            this._scheduled = false;
        }
        observe(target, options) {
            this._targets.push({ target, options });
            // Hook into the global mutation observer registry
            if (!globalThis.__bro_mutation_observers)
                globalThis.__bro_mutation_observers = [];
            if (!globalThis.__bro_mutation_observers.includes(this))
                globalThis.__bro_mutation_observers.push(this);
        }
        disconnect() {
            this._targets = [];
            if (globalThis.__bro_mutation_observers) {
                var idx = globalThis.__bro_mutation_observers.indexOf(this);
                if (idx >= 0) globalThis.__bro_mutation_observers.splice(idx, 1);
            }
        }
        takeRecords() {
            var r = this._records;
            this._records = [];
            return r;
        }
        // Called internally when mutations happen
        _notify(records) {
            this._records = this._records.concat(records);
            if (!this._scheduled) {
                this._scheduled = true;
                var self = this;
                queueMicrotask(function() {
                    self._scheduled = false;
                    var r = self._records;
                    self._records = [];
                    if (r.length > 0) self._callback(r, self);
                });
            }
        }
    };

    // document.activeElement is now a native C++ getter (see js_document_proto_funcs)
})();
)JS";
    JSValue r = JS_Eval(ctx, polyfills, strlen(polyfills),
                        "<dom-polyfills>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, r);

    // Register native getComputedStyle on window (globalThis)
    {
        JSValue g = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, g, "getComputedStyle",
            JS_NewCFunction(ctx, js_window_getComputedStyle, "getComputedStyle", 1));
        JS_FreeValue(ctx, g);
    }
}

void DomBindings::cleanup(JSContext* ctx) {
    // Per-context prototypes are owned by JS_SetClassProto and freed
    // when the context is destroyed — no manual free needed.
    s_ctx_documents.erase(ctx);
    s_ctx_factories.erase(ctx);
}

void DomBindings::cleanupRuntime(JSRuntime* rt) {
    s_classes_registered.erase(rt);
}

void DomBindings::sweepOrphanedWrappers(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (JS_IsUndefined(elemMap) || JS_IsNull(elemMap)) {
        JS_FreeValue(ctx, elemMap);
        JS_FreeValue(ctx, global);
        return;
    }

    // Collect keys of orphaned entries
    JSPropertyEnum* props = nullptr;
    uint32_t len = 0;
    JS_GetOwnPropertyNames(ctx, &props, &len, elemMap,
                           JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY);

    for (uint32_t i = 0; i < len; i++) {
        JSValue val = JS_GetProperty(ctx, elemMap, props[i].atom);
        auto* el = static_cast<bro::dom::Element*>(
            JS_GetOpaque(val, js_element_class_id));
        if (el && el->isAlive() && !el->parentNode() &&
            el->childNodes().empty() &&
            el->tagName() == "#DOCUMENT-FRAGMENT") {
            // Empty DocumentFragment with no parent — temporary container
            // created by jQuery's buildFragment, safe to free.
            auto* doc = el->document();
            JS_SetOpaque(val, nullptr);
            JS_DeleteProperty(ctx, elemMap, props[i].atom, 0);
            if (doc) doc->freeNode(el);
        }
        JS_FreeValue(ctx, val);
    }

    JS_FreePropertyEnum(ctx, props, len);
    JS_FreeValue(ctx, elemMap);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
