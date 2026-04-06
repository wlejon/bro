#include "js/dom_bindings_internal.h"
#include "js/custom_elements.h"
#include "js/event_dispatch.h"
#include "util/log.h"
#include "dom/event.h"
#include "dom/shadow_root.h"
#include "layout/el_select.h"

#include "dataset_proxy.js.h"

#include <algorithm>

namespace bro::js {

// ===========================================================================
// Element wrapper
// ===========================================================================

// Cache: each canvas element gets at most one rendering context (per web spec).
struct CachedContext { JSValue val; std::string type; };
static std::unordered_map<bro::dom::Element*, CachedContext> s_canvas_contexts;

void cleanupCanvasContextCache(JSRuntime* rt) {
    for (auto& [el, cc] : s_canvas_contexts)
        JS_FreeValueRT(rt, cc.val);
    s_canvas_contexts.clear();
}

// During shutdown, element pointers may already be freed — skip cleanup.
static bool s_shutting_down = false;

void setElementFinalizerShutdown(bool shutting_down) {
    s_shutting_down = shutting_down;
}

// Release orphaned elements when the JS wrapper is garbage-collected.
static void js_element_finalizer(JSRuntime* rt, JSValue val)
{
    auto* el = static_cast<bro::dom::Element*>(
        JS_GetOpaque(val, js_element_class_id));
    if (!el) return;

    if (s_shutting_down) return;

    // Free any cached canvas context for this element
    auto ccIt = s_canvas_contexts.find(el);
    if (ccIt != s_canvas_contexts.end()) {
        JS_FreeValueRT(rt, ccIt->second.val);
        s_canvas_contexts.erase(ccIt);
    }

    if (!el->isAlive()) return;

    if (!el->parentNode()) {
        auto* doc = el->document();
        if (doc) doc->freeNode(el);
    }
}

static JSClassDef js_element_class = {
    "Element",
    js_element_finalizer, nullptr, nullptr, nullptr
};

bro::dom::Element* getElement(JSValueConst val)
{
    auto* el = static_cast<bro::dom::Element*>(
        JS_GetOpaque(val, js_element_class_id));
    if (el && !el->isAlive()) {
        LOG_ERROR("USE-AFTER-FREE: Element %u (tag=%s) accessed after destruction!",
                  el->nodeId(), "(freed)");
        return nullptr;
    }
    return el;
}

void invalidateWrapper(JSContext* ctx, bro::dom::Element* elem) {
    if (!elem) return;

    for (auto& child : elem->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element)
            invalidateWrapper(ctx, static_cast<bro::dom::Element*>(child));
    }

    auto* ctxDoc = getDocumentForCtx(ctx);
    if (ctxDoc) {
        if (!elem->id().empty())
            ctxDoc->unregisterElementId(elem->id());
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (!JS_IsUndefined(elemMap)) {
        std::string key = std::to_string(elem->nodeId());
        JSValue wrapper = JS_GetPropertyStr(ctx, elemMap, key.c_str());
        if (!JS_IsUndefined(wrapper) && !JS_IsNull(wrapper)) {
            JS_SetOpaque(wrapper, nullptr);
            JS_FreeValue(ctx, wrapper);
        }
        JSAtom atom = JS_NewAtom(ctx, key.c_str());
        JS_DeleteProperty(ctx, elemMap, atom, 0);
        JS_FreeAtom(ctx, atom);
    }
    JS_FreeValue(ctx, elemMap);
    JS_FreeValue(ctx, global);
}

// ---- Properties -----------------------------------------------------------

static JSValue js_element_get_id(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    return JS_NewString(ctx, el->id().c_str());
}

static JSValue js_element_get_slot(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewString(ctx, "");
    return JS_NewString(ctx, el->getAttribute("slot").c_str());
}

static JSValue js_element_set_slot(JSContext* ctx, JSValueConst this_val,
                                   JSValueConst val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    el->setAttribute("slot", jsToStdString(ctx, val));
    return JS_UNDEFINED;
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
    auto* doc = getDocumentForCtx(ctx);
    bool hasElementChild = false;
    for (auto* child : el->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element) { hasElementChild = true; break; }
    }
    if (hasElementChild) {
        auto oldKids = el->childNodes();
        for (auto* child : oldKids) {
            child->setParent(nullptr);
        }
        el->childNodes().clear();
        for (auto* child : oldKids) {
            if (child->nodeType() != bro::dom::NodeType::Element) {
                if (doc) doc->freeNode(child);
            }
        }
    }
    // Only build MutationObserver arrays if observers are registered,
    // because wrapAnyNode stores wrappers in __bro_node_map that leak
    // when the underlying text nodes are replaced.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue observers = JS_GetPropertyStr(ctx, global, "__bro_mutation_observers");
    bool hasObservers = !JS_IsUndefined(observers) && !JS_IsNull(observers);
    JS_FreeValue(ctx, observers);
    JS_FreeValue(ctx, global);

    if (hasObservers) {
        JSValue removedArr = JS_NewArray(ctx);
        uint32_t rmIdx = 0;
        for (auto* child : el->childNodes()) {
            JS_SetPropertyUint32(ctx, removedArr, rmIdx++, wrapAnyNode(ctx, child));
        }
        el->setTextContent(jsToStdString(ctx, val));
        JSValue addedArr = JS_NewArray(ctx);
        uint32_t addIdx = 0;
        for (auto* child : el->childNodes()) {
            JS_SetPropertyUint32(ctx, addedArr, addIdx++, wrapAnyNode(ctx, child));
        }
        notifyMutationObservers(ctx, this_val, "childList",
            nullptr, nullptr, addedArr, removedArr);
        JS_FreeValue(ctx, addedArr);
        JS_FreeValue(ctx, removedArr);
    } else {
        el->setTextContent(jsToStdString(ctx, val));
    }
    auto& style = el->computedStyle();
    // Check overflow-y first, then overflow shorthand
    auto oyIt = style.find("overflow-y");
    std::string ov = (oyIt != style.end()) ? oyIt->second : "";
    if (ov.empty()) {
        auto oIt = style.find("overflow");
        ov = (oIt != style.end()) ? oIt->second : "visible";
    }
    if (ov != "visible" && ov != "initial") {
        el->setScrollToBottom(true);
    }
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
    auto* doc = getDocumentForCtx(ctx);
    bool hasElementChild = false;
    for (auto* child : el->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element) { hasElementChild = true; break; }
    }
    if (hasElementChild) {
        auto oldKids = el->childNodes();
        for (auto* child : oldKids) {
            child->setParent(nullptr);
        }
        el->childNodes().clear();
        for (auto* child : oldKids) {
            if (child->nodeType() != bro::dom::NodeType::Element) {
                if (doc) doc->freeNode(child);
            }
        }
    }
    // Only wrap nodes for MutationObserver if observers are registered
    JSValue ihGlobal = JS_GetGlobalObject(ctx);
    JSValue ihObs = JS_GetPropertyStr(ctx, ihGlobal, "__bro_mutation_observers");
    bool ihHasObservers = !JS_IsUndefined(ihObs) && !JS_IsNull(ihObs);
    JS_FreeValue(ctx, ihObs);
    JS_FreeValue(ctx, ihGlobal);

    if (ihHasObservers) {
        JSValue ihRemovedArr = JS_NewArray(ctx);
        uint32_t ihRmIdx = 0;
        for (auto* child : el->childNodes()) {
            JS_SetPropertyUint32(ctx, ihRemovedArr, ihRmIdx++, wrapAnyNode(ctx, child));
        }
        el->setInnerHTML(jsToStdString(ctx, val));
        JSValue ihAddedArr = JS_NewArray(ctx);
        uint32_t ihAddIdx = 0;
        for (auto* child : el->childNodes()) {
            JS_SetPropertyUint32(ctx, ihAddedArr, ihAddIdx++, wrapAnyNode(ctx, child));
        }
        notifyMutationObservers(ctx, this_val, "childList",
            nullptr, nullptr, ihAddedArr, ihRemovedArr);
        JS_FreeValue(ctx, ihAddedArr);
        JS_FreeValue(ctx, ihRemovedArr);
    } else {
        el->setInnerHTML(jsToStdString(ctx, val));
    }
    return JS_UNDEFINED;
}

// ---- Tree navigation ------------------------------------------------------

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

    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    for (auto* child : el->childNodes()) {
        JS_SetPropertyUint32(ctx, arr, idx++, wrapAnyNode(ctx, child));
    }
    JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, static_cast<int32_t>(idx)));
    return arr;
}

static JSValue js_element_get_classList(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    // Return cached DOMTokenList if available
    JSValue cached = JS_GetPropertyStr(ctx, this_val, "__bro_classList");
    if (!JS_IsUndefined(cached) && !JS_IsNull(cached))
        return cached;
    JS_FreeValue(ctx, cached);
    JSValue tl = wrapTokenList(ctx, el);
    JS_SetPropertyStr(ctx, this_val, "__bro_classList", JS_DupValue(ctx, tl));
    return tl;
}

static JSValue js_element_get_parentNode(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_NULL;
    auto* parent = el->parentNode();
    if (!parent || parent->nodeType() != bro::dom::NodeType::Element) return JS_NULL;
    return DomBindings::wrapElement(ctx, static_cast<bro::dom::Element*>(parent));
}

static JSValue wrapChildAtIndex(JSContext* ctx, bro::dom::Node* parent, size_t idx) {
    if (!parent) return JS_NULL;
    auto& kids = parent->childNodes();
    if (idx >= kids.size()) return JS_NULL;
    return wrapAnyNode(ctx, kids[idx]);
}

static int findChildIndex(bro::dom::Node* node) {
    if (!node || !node->parentNode()) return -1;
    auto& kids = node->parentNode()->childNodes();
    for (size_t i = 0; i < kids.size(); ++i) {
        if (kids[i] == node) return static_cast<int>(i);
    }
    return -1;
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

// ---- Node properties ------------------------------------------------------

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
    // Return cached CSSStyleDeclaration if available
    JSValue cached = JS_GetPropertyStr(ctx, this_val, "__bro_style");
    if (!JS_IsUndefined(cached) && !JS_IsNull(cached))
        return cached;
    JS_FreeValue(ctx, cached);
    JSValue s = wrapStyleProxy(ctx, &el->style());
    JS_SetPropertyStr(ctx, this_val, "__bro_style", JS_DupValue(ctx, s));
    return s;
}

// ---- Node standard properties ---------------------------------------------

static JSValue js_element_get_isConnected(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_FALSE;
    // Walk up the tree; connected if we reach the document's root element.
    // Document is not a Node in our tree, so check against documentElement.
    auto* doc = el->document();
    if (!doc) return JS_FALSE;
    auto* docEl = doc->documentElement();
    if (!docEl) return JS_FALSE;

    auto* node = static_cast<bro::dom::Node*>(el);
    while (node) {
        if (node == docEl)
            return JS_TRUE;
        // Cross shadow boundary
        auto* parent = node->parentNode();
        if (parent && parent->nodeType() == bro::dom::NodeType::DocumentFragment) {
            auto* sr = dynamic_cast<bro::dom::ShadowRoot*>(parent);
            if (sr && sr->host()) {
                node = sr->host();
                continue;
            }
        }
        node = parent;
    }
    return JS_FALSE;
}

static JSValue js_element_getRootNode(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el) return JS_NULL;

    // Check options.composed
    bool composed = false;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue compVal = JS_GetPropertyStr(ctx, argv[0], "composed");
        composed = JS_ToBool(ctx, compVal);
        JS_FreeValue(ctx, compVal);
    }

    auto* node = static_cast<bro::dom::Node*>(el);
    while (node->parentNode()) {
        auto* parent = node->parentNode();
        // Cross shadow boundary if composed
        if (composed && parent->nodeType() == bro::dom::NodeType::DocumentFragment) {
            auto* sr = dynamic_cast<bro::dom::ShadowRoot*>(parent);
            if (sr && sr->host()) {
                node = sr->host();
                continue;
            }
        }
        node = parent;
    }

    // If the root is the documentElement, return the document object
    auto* doc = el->document();
    if (doc && node == doc->documentElement()) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue docObj = JS_GetPropertyStr(ctx, global, "document");
        JS_FreeValue(ctx, global);
        return docObj;
    }

    if (node->nodeType() == bro::dom::NodeType::Element)
        return DomBindings::wrapElement(ctx, static_cast<bro::dom::Element*>(node));
    return JS_NULL;
}

static JSValue js_element_hasChildNodes(JSContext* ctx, JSValueConst this_val,
                                        int /*argc*/, JSValueConst* /*argv*/)
{
    auto* el = getElement(this_val);
    if (!el) return JS_FALSE;
    return JS_NewBool(ctx, !el->childNodes().empty());
}

// ---- Form control properties ----------------------------------------------

static JSValue js_element_get_value(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    // For <select>, return the value of the selected <option>
    if (auto* sel = el->selectControl()) {
        auto opts = sel->getOptions();
        int idx = sel->selectedIndex();
        if (idx >= 0 && idx < static_cast<int>(opts.size()))
            return JS_NewString(ctx, opts[idx].value.c_str());
        return JS_NewString(ctx, "");
    }
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

static JSValue js_element_get_selected(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_FALSE;
    return JS_NewBool(ctx, el->attributes().count("selected") > 0);
}

static JSValue js_element_set_selected(JSContext* ctx, JSValueConst this_val,
                                       JSValueConst val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    if (JS_ToBool(ctx, val))
        el->setAttribute("selected", "");
    else
        el->removeAttribute("selected");
    return JS_UNDEFINED;
}

static JSValue js_element_get_selectedIndex(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_NewInt32(ctx, -1);
    if (auto* sel = el->selectControl())
        return JS_NewInt32(ctx, sel->selectedIndex());
    return JS_NewInt32(ctx, -1);
}

static JSValue js_element_set_selectedIndex(JSContext* ctx, JSValueConst this_val,
                                            JSValueConst val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    if (auto* sel = el->selectControl()) {
        int idx;
        JS_ToInt32(ctx, &idx, val);
        sel->setSelectedIndex(idx);
    }
    return JS_UNDEFINED;
}

static JSValue js_element_get_type(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    std::string t = el->getAttribute("type");
    if (t.empty()) t = "text";
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

// ---- Attribute methods ----------------------------------------------------

static JSValue js_element_getAttribute(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_UNDEFINED;
    std::string name = jsToStdString(ctx, argv[0]);
    if (!el->hasAttribute(name)) return JS_NULL;
    std::string val = el->getAttribute(name);
    return JS_NewString(ctx, val.c_str());
}

static JSValue js_element_setAttribute(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 2) return JS_UNDEFINED;
    std::string name = jsToStdString(ctx, argv[0]);
    std::string newVal = jsToStdString(ctx, argv[1]);
    std::string oldVal = el->getAttribute(name);
    el->setAttribute(name, newVal);
    if (oldVal != newVal) {
        fireAttributeChangedCallback(ctx, this_val, name, oldVal, newVal);
        notifyMutationObservers(ctx, this_val, "attributes",
            name.c_str(), oldVal.empty() ? nullptr : oldVal.c_str(),
            JS_NULL, JS_NULL);
    }
    return JS_UNDEFINED;
}

static JSValue js_element_removeAttribute(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_UNDEFINED;
    std::string name = jsToStdString(ctx, argv[0]);
    std::string oldVal = el->getAttribute(name);
    el->removeAttribute(name);
    if (!oldVal.empty()) {
        notifyMutationObservers(ctx, this_val, "attributes",
            name.c_str(), oldVal.c_str(), JS_NULL, JS_NULL);
    }
    return JS_UNDEFINED;
}

static JSValue js_element_hasAttribute(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_FALSE;
    std::string name = jsToStdString(ctx, argv[0]);
    return JS_NewBool(ctx, el->hasAttribute(name));
}

// ---- DOM manipulation methods ---------------------------------------------

static JSValue js_element_appendChild(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_UNDEFINED;
    auto* child = unwrapNode(ctx, argv[0]);
    if (child) {
        auto* doc = getDocumentForCtx(ctx);
        if (child->nodeName() == "#DOCUMENT-FRAGMENT" ||
            child->nodeType() == bro::dom::NodeType::DocumentFragment) {
            auto kids = child->childNodes();
            // Build addedNodes array for MutationObserver
            JSValue addedArr = JS_NewArray(ctx);
            uint32_t addedIdx = 0;
            for (auto* kid : kids) {
                el->appendChild(kid);
                if (doc && kid->nodeType() == bro::dom::NodeType::Element)
                    doc->markStructureDirty();
                JS_SetPropertyUint32(ctx, addedArr, addedIdx++, wrapAnyNode(ctx, kid));
            }
            for (auto* kid : kids) {
                if (kid->nodeType() == bro::dom::NodeType::Element) {
                    JSValue w = DomBindings::wrapElement(ctx, kid);
                    fireConnectedCallback(ctx, w);
                    JS_FreeValue(ctx, w);
                }
            }
            notifyMutationObservers(ctx, this_val, "childList",
                nullptr, nullptr, addedArr, JS_NULL);
            JS_FreeValue(ctx, addedArr);
        } else {
            el->appendChild(child);
            if (doc && child->nodeType() == bro::dom::NodeType::Element) {
                doc->markStructureDirty();
            }
            if (child->nodeType() == bro::dom::NodeType::Element) {
                JSValue w = DomBindings::wrapElement(ctx, child);
                fireConnectedCallback(ctx, w);
                JS_FreeValue(ctx, w);
            }
            JSValue addedArr = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, addedArr, 0, wrapAnyNode(ctx, child));
            notifyMutationObservers(ctx, this_val, "childList",
                nullptr, nullptr, addedArr, JS_NULL);
            JS_FreeValue(ctx, addedArr);
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
        // Build removedNodes before removal
        JSValue removedArr = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, removedArr, 0, wrapAnyNode(ctx, child));
        if (child->nodeType() == bro::dom::NodeType::Element) {
            auto* childElem = static_cast<bro::dom::Element*>(child);
            JSValue w = DomBindings::wrapElement(ctx, childElem);
            fireDisconnectedCallback(ctx, w);
            JS_FreeValue(ctx, w);
            if (doc && !childElem->id().empty())
                doc->unregisterElementId(childElem->id());
            if (doc) doc->markStructureDirty();
        }
        el->removeChild(child);
        notifyMutationObservers(ctx, this_val, "childList",
            nullptr, nullptr, JS_NULL, removedArr);
        JS_FreeValue(ctx, removedArr);
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
            doc->markStructureDirty();
            JSValue w = DomBindings::wrapElement(ctx, newElem);
            fireConnectedCallback(ctx, w);
            JS_FreeValue(ctx, w);
        }
        JSValue addedArr = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, addedArr, 0, wrapAnyNode(ctx, newChild));
        notifyMutationObservers(ctx, this_val, "childList",
            nullptr, nullptr, addedArr, JS_NULL);
        JS_FreeValue(ctx, addedArr);
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
        // Capture removed node wrapper before invalidation
        JSValue removedArr = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, removedArr, 0, wrapAnyNode(ctx, oldChild));
        if (oldChild->nodeType() == bro::dom::NodeType::Element) {
            JSValue w = DomBindings::wrapElement(ctx, oldChild);
            fireDisconnectedCallback(ctx, w);
            JS_FreeValue(ctx, w);
        }
        el->insertBefore(newChild, oldChild);
        if (doc) doc->markStructureDirty();
        if (oldChild->nodeType() == bro::dom::NodeType::Element) {
            auto* oldElem = static_cast<bro::dom::Element*>(oldChild);
            if (doc && !oldElem->id().empty())
                doc->unregisterElementId(oldElem->id());
            invalidateWrapper(ctx, oldElem);
        }
        el->removeChild(oldChild);
        if (doc) doc->freeNode(oldChild);
        if (newChild->nodeType() == bro::dom::NodeType::Element) {
            JSValue w = DomBindings::wrapElement(ctx, newChild);
            fireConnectedCallback(ctx, w);
            JS_FreeValue(ctx, w);
        }
        JSValue addedArr = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, addedArr, 0, wrapAnyNode(ctx, newChild));
        notifyMutationObservers(ctx, this_val, "childList",
            nullptr, nullptr, addedArr, removedArr);
        JS_FreeValue(ctx, addedArr);
        JS_FreeValue(ctx, removedArr);
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

    auto* clone = doc->createElement(el->tagName());
    if (!clone) return JS_NULL;

    for (auto& [name, val] : el->attributes()) {
        if (name == "id") continue;
        clone->setAttribute(name, val);
    }

    if (deep) {
        for (auto* child : el->childNodes()) {
            if (child->nodeType() == bro::dom::NodeType::Element) {
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
        if (doc) doc->markStructureDirty();
        invalidateWrapper(ctx, el);
        parent->removeChild(el);
        if (doc) doc->freeNode(el);
    }
    return JS_UNDEFINED;
}

// ---- Event listeners ------------------------------------------------------

static const char* kListenersKey = "__bro_listeners";

static JSValue js_element_addEventListener(JSContext* ctx,
                                           JSValueConst this_val,
                                           int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;

    std::string type = jsToStdString(ctx, argv[0]);

    // Parse options (3rd arg): boolean useCapture or options object
    bool capture = false;
    bool once = false;
    bool passive = false;
    if (argc >= 3) {
        if (JS_IsBool(argv[2])) {
            capture = JS_ToBool(ctx, argv[2]);
        } else if (JS_IsObject(argv[2])) {
            JSValue capVal = JS_GetPropertyStr(ctx, argv[2], "capture");
            capture = JS_ToBool(ctx, capVal);
            JS_FreeValue(ctx, capVal);
            JSValue onceVal = JS_GetPropertyStr(ctx, argv[2], "once");
            once = JS_ToBool(ctx, onceVal);
            JS_FreeValue(ctx, onceVal);
            JSValue passiveVal = JS_GetPropertyStr(ctx, argv[2], "passive");
            passive = JS_ToBool(ctx, passiveVal);
            JS_FreeValue(ctx, passiveVal);
        }
    }

    JSAtom key = JS_NewAtom(ctx, kListenersKey);
    JSValue arr = JS_GetProperty(ctx, this_val, key);
    if (JS_IsUndefined(arr) || JS_IsException(arr)) {
        arr = JS_NewArray(ctx);
        JS_SetProperty(ctx, this_val, JS_DupAtom(ctx, key), JS_DupValue(ctx, arr));
    }

    JSValue entry = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entry, "type", JS_NewString(ctx, type.c_str()));
    JS_SetPropertyStr(ctx, entry, "cb", JS_DupValue(ctx, argv[1]));
    JS_SetPropertyStr(ctx, entry, "capture", JS_NewBool(ctx, capture));
    JS_SetPropertyStr(ctx, entry, "once", JS_NewBool(ctx, once));
    JS_SetPropertyStr(ctx, entry, "passive", JS_NewBool(ctx, passive));

    int64_t len = 0;
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    JS_ToInt64(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);
    JS_SetPropertyInt64(ctx, arr, len, entry);

    JS_FreeValue(ctx, arr);
    JS_FreeAtom(ctx, key);

    el->addEventListener(type, static_cast<int64_t>(len));

    return JS_UNDEFINED;
}

static JSValue js_element_removeEventListener(JSContext* ctx,
                                              JSValueConst this_val,
                                              int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;

    std::string type = jsToStdString(ctx, argv[0]);

    // Parse options (3rd arg): boolean useCapture or options object with capture
    bool capture = false;
    if (argc >= 3) {
        if (JS_IsBool(argv[2])) {
            capture = JS_ToBool(ctx, argv[2]);
        } else if (JS_IsObject(argv[2])) {
            JSValue capVal = JS_GetPropertyStr(ctx, argv[2], "capture");
            capture = JS_ToBool(ctx, capVal);
            JS_FreeValue(ctx, capVal);
        }
    }

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
                // Per spec, capture flag must match for removal
                JSValue capVal = JS_GetPropertyStr(ctx, entry, "capture");
                bool entryCapture = JS_ToBool(ctx, capVal);
                JS_FreeValue(ctx, capVal);

                JSValue cb = JS_GetPropertyStr(ctx, entry, "cb");
                bool same = JS_IsFunction(ctx, cb) &&
                            JS_VALUE_GET_PTR(cb) == JS_VALUE_GET_PTR(argv[1]) &&
                            entryCapture == capture;
                JS_FreeValue(ctx, cb);
                if (same) {
                    // Splice out by shifting remaining entries down
                    JS_FreeValue(ctx, entry);
                    for (int64_t j = i; j < len - 1; ++j) {
                        JSValue next = JS_GetPropertyInt64(ctx, arr, j + 1);
                        JS_SetPropertyInt64(ctx, arr, j, next);
                    }
                    JS_SetPropertyStr(ctx, arr, "length", JS_NewInt64(ctx, len - 1));
                    break;
                }
            }
        }
        JS_FreeValue(ctx, entry);
    }

    JS_FreeValue(ctx, arr);
    return JS_UNDEFINED;
}

// ---- Query / selector methods ---------------------------------------------

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

static JSValue js_element_contains(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_FALSE;
    auto* other = static_cast<bro::dom::Element*>(
        DomBindings::unwrapElement(ctx, argv[0]));
    if (!other) return JS_FALSE;
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
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_NewInt32(ctx, 0);
    auto* other = static_cast<bro::dom::Element*>(
        DomBindings::unwrapElement(ctx, argv[0]));
    if (!other) return JS_NewInt32(ctx, 0);
    if (el == other) return JS_NewInt32(ctx, 0);

    auto* node = static_cast<bro::dom::Node*>(other);
    while (node) {
        if (node == el) return JS_NewInt32(ctx, 16 | 4);
        node = node->parentNode();
    }
    node = static_cast<bro::dom::Node*>(el);
    while (node) {
        if (node == other) return JS_NewInt32(ctx, 8 | 2);
        node = node->parentNode();
    }
    return JS_NewInt32(ctx, el->nodeId() < other->nodeId() ? 4 : 2);
}

static JSValue js_element_get_ownerDocument(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el || !el->document()) return JS_NULL;
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
    return wrapLiveHTMLCollection(ctx, el, nullptr, tag);
}

static JSValue js_element_getElementsByClassName(JSContext* ctx,
                                                 JSValueConst this_val,
                                                 int argc, JSValueConst* argv)
{
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_NewArray(ctx);
    std::string cls = jsToStdString(ctx, argv[0]);
    return wrapLiveHTMLCollection(ctx, el, nullptr, "." + cls);
}

static JSValue js_element_getContext(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_NULL;
    if (el->tagName() != "canvas" && el->tagName() != "CANVAS") return JS_NULL;
    const char* typeStr = JS_ToCString(ctx, argv[0]);
    std::string type = typeStr ? typeStr : "";
    if (typeStr) JS_FreeCString(ctx, typeStr);
    if (type != "2d" && type != "webgl" && type != "webgl2") return JS_NULL;

    // Return cached context if one already exists for this element.
    // Verify the element is still in the DOM — if it was removed and a new
    // element was allocated at the same address, the stale entry must be evicted.
    auto cacheIt = s_canvas_contexts.find(el);
    if (cacheIt != s_canvas_contexts.end()) {
        // Check if element is still connected to the document tree
        auto* n = static_cast<bro::dom::Element*>(el);
        while (n->parentNode()) n = static_cast<bro::dom::Element*>(n->parentNode());
        bool connected = (n->tagName() == "html" || n->tagName() == "HTML");
        if (!connected) {
            // Stale entry — element disconnected from DOM, pointer reused
            JS_FreeValue(ctx, cacheIt->second.val);
            s_canvas_contexts.erase(cacheIt);
        } else {
            if (cacheIt->second.type != type) return JS_NULL;
            return JS_DupValue(ctx, cacheIt->second.val);
        }
    }

    auto factoryIt = s_ctx_factories.find(ctx);
    if (factoryIt != s_ctx_factories.end() && factoryIt->second) {
        JSValue val = factoryIt->second(ctx, el, type);
        if (!JS_IsNull(val) && !JS_IsUndefined(val) && !JS_IsException(val)) {
            s_canvas_contexts[el] = { JS_DupValue(ctx, val), type };
        }
        return val;
    }
    return JS_NULL;
}

// ---- Width / height (canvas element support) ------------------------------

static JSValue js_element_get_width(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val); if (!el) return JS_UNDEFINED;
    std::string v = el->getAttribute("width");
    if (!v.empty()) return JS_NewInt32(ctx, std::atoi(v.c_str()));
    return JS_NewInt32(ctx, 300);
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
    return JS_NewInt32(ctx, 150);
}

static JSValue js_element_set_height(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* el = getElement(this_val); if (!el) return JS_UNDEFINED;
    int h; JS_ToInt32(ctx, &h, val);
    el->setAttribute("height", std::to_string(h));
    return JS_UNDEFINED;
}

// ---- Layout measurement ---------------------------------------------------

static const htmlayout::layout::LayoutBox& getLayoutBox(bro::dom::Element* el) {
    static const htmlayout::layout::LayoutBox empty{};
    if (!el) return empty;
    return el->layoutBox();
}

static JSValue js_element_get_clientWidth(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    auto& box = getLayoutBox(el);
    float cw = box.contentRect.width + box.padding.left + box.padding.right;
    if (cw > 0) return JS_NewInt32(ctx, static_cast<int>(cw));
    return js_element_get_width(ctx, this_val);
}

static JSValue js_element_get_clientHeight(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    auto& box = getLayoutBox(el);
    float ch = box.contentRect.height + box.padding.top + box.padding.bottom;
    if (ch > 0) return JS_NewInt32(ctx, static_cast<int>(ch));
    return js_element_get_height(ctx, this_val);
}

static JSValue js_element_get_offsetWidth(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    auto& box = getLayoutBox(el);
    return JS_NewInt32(ctx, static_cast<int>(box.fullWidth()));
}

static JSValue js_element_get_offsetHeight(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    auto& box = getLayoutBox(el);
    return JS_NewInt32(ctx, static_cast<int>(box.fullHeight()));
}

static JSValue js_element_get_offsetLeft(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    auto& box = getLayoutBox(el);
    return JS_NewInt32(ctx, static_cast<int>(box.contentRect.x - box.padding.left - box.border.left));
}

static JSValue js_element_get_offsetTop(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    auto& box = getLayoutBox(el);
    return JS_NewInt32(ctx, static_cast<int>(box.contentRect.y - box.padding.top - box.border.top));
}

static JSValue js_element_get_scrollWidth(JSContext* ctx, JSValueConst this_val) {
    return js_element_get_offsetWidth(ctx, this_val);
}

static JSValue js_element_get_scrollHeight(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewInt32(ctx, 0);
    auto& box = el->layoutBox();
    // scrollHeight should return the full content height, not the clamped visible height.
    // For overflow elements, naturalHeight holds the unclamped content extent.
    float h = box.naturalHeight > box.contentRect.height ? box.naturalHeight : box.contentRect.height;
    if (h > 0)
        return JS_NewInt32(ctx, static_cast<int>(h));
    return js_element_get_offsetHeight(ctx, this_val);
}

static JSValue js_element_get_scrollLeft(JSContext* ctx, JSValueConst /*this_val*/) {
    (void)ctx;
    return JS_NewInt32(ctx, 0);
}

static JSValue js_element_get_scrollTop(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewInt32(ctx, 0);
    return JS_NewFloat64(ctx, static_cast<double>(el->scrollTopValue()));
}

static JSValue js_element_set_scrollLeft(JSContext* /*ctx*/, JSValueConst /*this_val*/, JSValueConst /*val*/) {
    return JS_UNDEFINED;
}

static JSValue js_element_set_scrollTop(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    double v = 0;
    JS_ToFloat64(ctx, &v, val);
    el->setScrollTopValue(static_cast<float>(v));
    if (el->document()) el->document()->markDirty();
    return JS_UNDEFINED;
}

// ---- innerText ------------------------------------------------------------

static void collectInnerText(const bro::dom::Element* el, std::string& out) {
    for (const auto& child : el->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Text) {
            auto* text = static_cast<const bro::dom::TextNode*>(child);
            out += text->data();
        } else if (child->nodeType() == bro::dom::NodeType::Element) {
            auto* childEl = static_cast<const bro::dom::Element*>(child);
            const auto& tag = childEl->tagName();
            if (tag == "SCRIPT" || tag == "STYLE" || tag == "script" || tag == "style")
                continue;
            auto& style = childEl->computedStyle();
            auto dIt = style.find("display");
            std::string display = (dIt != style.end()) ? dIt->second : "inline";
            if (display == "none") continue;
            bool isBlock = (display == "block" || display == "list-item" || display == "table");
            if (isBlock && !out.empty() && out.back() != '\n')
                out += '\n';
            collectInnerText(childEl, out);
            if (isBlock && !out.empty() && out.back() != '\n')
                out += '\n';
        }
    }
}

static JSValue js_element_get_innerText(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    std::string result;
    collectInnerText(el, result);
    while (!result.empty() && result.back() == '\n')
        result.pop_back();
    return JS_NewString(ctx, result.c_str());
}

// ---- Misc methods ---------------------------------------------------------

static JSValue js_element_scrollIntoView(JSContext* /*ctx*/, JSValueConst /*this_val*/,
                                         int /*argc*/, JSValueConst* /*argv*/) {
    return JS_UNDEFINED;
}

static JSValue js_element_getBoundingClientRect(JSContext* ctx, JSValueConst this_val,
                                                 int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    auto& box = getLayoutBox(el);

    // Accumulate absolute position by walking up the layout parent chain.
    // Layout positions are parent-relative (content area origin), where the
    // parent is determined by the composed tree (including shadow DOM wrappers
    // that contain <slot> elements). We must walk this composed/layout parent
    // chain rather than the DOM parent chain to correctly account for shadow
    // DOM wrapper elements like .screen-content, .body, .tab-content.
    float x = box.contentRect.x - box.padding.left - box.border.left;
    float y = box.contentRect.y - box.padding.top - box.border.top;
    for (auto* lp = el->layoutParent(); lp; lp = lp->layoutParent()) {
        auto& pb = lp->layoutBox();
        x += pb.contentRect.x;
        y += pb.contentRect.y;
        // Account for element scroll (same as draw traversal)
        y -= lp->scrollTopValue();
    }

    JSValue rect = JS_NewObject(ctx);
    float w = box.fullWidth();
    float h = box.fullHeight();
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

static JSValue js_element_dispatchEvent(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_FALSE;

    JSValue typeVal = JS_GetPropertyStr(ctx, argv[0], "type");
    const char* typeStr = JS_ToCString(ctx, typeVal);
    JS_FreeValue(ctx, typeVal);
    if (!typeStr) return JS_FALSE;

    std::string type = typeStr;
    JS_FreeCString(ctx, typeStr);

    JSValue bubblesVal = JS_GetPropertyStr(ctx, argv[0], "bubbles");
    bool bubbles = JS_ToBool(ctx, bubblesVal);
    JS_FreeValue(ctx, bubblesVal);

    JSValue cancelableVal = JS_GetPropertyStr(ctx, argv[0], "cancelable");
    bool cancelable = JS_ToBool(ctx, cancelableVal);
    JS_FreeValue(ctx, cancelableVal);

    bro::dom::Event evt(type, bubbles, cancelable);
    bro::js::dispatchDomEvent(ctx, el, evt, argv[0]);

    return JS_NewBool(ctx, !evt.defaultPrevented());
}

static JSValue js_element_click(JSContext* ctx, JSValueConst this_val,
                                int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* doc = getDocumentForCtx(ctx);
    if (doc) doc->setActiveElement(el);
    dom::MouseEvent event("click");
    dispatchDomEvent(ctx, el, event);
    return JS_UNDEFINED;
}

static JSValue js_element_focus(JSContext* ctx, JSValueConst this_val,
                                int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* doc = getDocumentForCtx(ctx);
    if (!doc) return JS_UNDEFINED;
    auto* prev = doc->activeElement();
    if (prev == el) return JS_UNDEFINED; // already focused
    doc->setActiveElement(el);

    // Dispatch blur on previous, then focus on new element
    if (prev) {
        dom::FocusEvent blurEvt("blur", false, false);
        blurEvt.setRelatedTarget(el);
        dispatchDomEvent(ctx, prev, blurEvt);
    }
    {
        dom::FocusEvent focusEvt("focus", false, false);
        focusEvt.setRelatedTarget(prev);
        dispatchDomEvent(ctx, el, focusEvt);
    }
    // Bubbling variants
    if (prev) {
        dom::FocusEvent focusoutEvt("focusout", true, false);
        focusoutEvt.setRelatedTarget(el);
        dispatchDomEvent(ctx, prev, focusoutEvt);
    }
    {
        dom::FocusEvent focusinEvt("focusin", true, false);
        focusinEvt.setRelatedTarget(prev);
        dispatchDomEvent(ctx, el, focusinEvt);
    }
    return JS_UNDEFINED;
}

static JSValue js_element_blur(JSContext* ctx, JSValueConst this_val,
                               int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* doc = getDocumentForCtx(ctx);
    if (!doc) return JS_UNDEFINED;
    // activeElement() returns body when focusedElement_ is null, so check
    // the actual focused element to know if el is really focused
    if (doc->activeElement() != el) return JS_UNDEFINED;
    doc->setActiveElement(nullptr);

    // Dispatch blur/focusout on the element
    {
        dom::FocusEvent blurEvt("blur", false, false);
        blurEvt.setRelatedTarget(nullptr);
        dispatchDomEvent(ctx, el, blurEvt);
    }
    {
        dom::FocusEvent focusoutEvt("focusout", true, false);
        focusoutEvt.setRelatedTarget(nullptr);
        dispatchDomEvent(ctx, el, focusoutEvt);
    }
    return JS_UNDEFINED;
}

// ---- outerHTML ------------------------------------------------------------

static JSValue js_element_get_outerHTML(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    std::string tag = el->tagName();
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

static JSValue js_element_set_outerHTML(JSContext* ctx, JSValueConst this_val,
                                        JSValueConst val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* parent = el->parentElement();
    if (!parent) return JS_UNDEFINED;

    // Capture removed node for MutationObserver
    JSValue removedArr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, removedArr, 0, JS_DupValue(ctx, this_val));

    std::string html = jsToStdString(ctx, val);
    el->setOuterHTML(html);

    // Notify MutationObserver on the parent
    JSValue parentJs = DomBindings::wrapElement(ctx, parent);
    JSValue addedArr = JS_NewArray(ctx);
    uint32_t addIdx = 0;
    for (auto* child : parent->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element) {
            JS_SetPropertyUint32(ctx, addedArr, addIdx++,
                DomBindings::wrapElement(ctx, static_cast<bro::dom::Element*>(child)));
        }
    }
    notifyMutationObservers(ctx, parentJs, "childList",
        nullptr, nullptr, addedArr, removedArr);
    JS_FreeValue(ctx, addedArr);
    JS_FreeValue(ctx, removedArr);
    JS_FreeValue(ctx, parentJs);

    // Invalidate the old element wrapper
    JS_SetOpaque(this_val, nullptr);

    return JS_UNDEFINED;
}

// ---- insertAdjacentHTML ---------------------------------------------------

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

    auto* doc = el->document();
    if (!doc) { JS_FreeCString(ctx, html); return JS_UNDEFINED; }

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
    }

    JS_FreeCString(ctx, html);
    return JS_UNDEFINED;
}

// ---- Convenience mutation methods -----------------------------------------

// Helper: convert a JS argument to a Node. If it's a string, create a TextNode.
static bro::dom::Node* nodeOrTextFromArg(JSContext* ctx, JSValueConst val) {
    auto* node = unwrapNode(ctx, val);
    if (node) return node;
    // If it's a string, create a text node
    if (JS_IsString(val)) {
        auto* doc = getDocumentForCtx(ctx);
        if (doc) return doc->createTextNode(jsToStdString(ctx, val));
    }
    return nullptr;
}

// Helper: after appending/inserting a node, fire connected callback if element
static void fireConnectedIfElement(JSContext* ctx, bro::dom::Node* node) {
    if (node->nodeType() == bro::dom::NodeType::Element) {
        JSValue w = DomBindings::wrapElement(ctx, node);
        fireConnectedCallback(ctx, w);
        JS_FreeValue(ctx, w);
    }
}

static JSValue js_element_append(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* doc = getDocumentForCtx(ctx);
    for (int i = 0; i < argc; ++i) {
        auto* node = nodeOrTextFromArg(ctx, argv[i]);
        if (!node) continue;
        el->appendChild(node);
        if (doc && node->nodeType() == bro::dom::NodeType::Element)
            doc->markStructureDirty();
        fireConnectedIfElement(ctx, node);
    }
    return JS_UNDEFINED;
}

static JSValue js_element_prepend(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* doc = getDocumentForCtx(ctx);
    auto* ref = el->childNodes().empty() ? nullptr : el->childNodes().front();
    for (int i = 0; i < argc; ++i) {
        auto* node = nodeOrTextFromArg(ctx, argv[i]);
        if (!node) continue;
        el->insertBefore(node, ref);
        if (doc && node->nodeType() == bro::dom::NodeType::Element)
            doc->markStructureDirty();
        fireConnectedIfElement(ctx, node);
    }
    return JS_UNDEFINED;
}

static JSValue js_element_before(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* parent = el->parentNode();
    if (!parent) return JS_UNDEFINED;
    auto* doc = getDocumentForCtx(ctx);
    for (int i = 0; i < argc; ++i) {
        auto* node = nodeOrTextFromArg(ctx, argv[i]);
        if (!node) continue;
        parent->insertBefore(node, el);
        if (doc && node->nodeType() == bro::dom::NodeType::Element)
            doc->markStructureDirty();
        fireConnectedIfElement(ctx, node);
    }
    return JS_UNDEFINED;
}

static JSValue js_element_after(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* parent = el->parentNode();
    if (!parent) return JS_UNDEFINED;
    auto* doc = getDocumentForCtx(ctx);
    // Find the node after 'el' to use as reference
    bro::dom::Node* ref = nullptr;
    auto& siblings = parent->childNodes();
    for (size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i] == el && i + 1 < siblings.size()) {
            ref = siblings[i + 1];
            break;
        }
    }
    for (int i = 0; i < argc; ++i) {
        auto* node = nodeOrTextFromArg(ctx, argv[i]);
        if (!node) continue;
        parent->insertBefore(node, ref);
        if (doc && node->nodeType() == bro::dom::NodeType::Element)
            doc->markStructureDirty();
        fireConnectedIfElement(ctx, node);
    }
    return JS_UNDEFINED;
}

static JSValue js_element_replaceWith(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* parent = el->parentNode();
    if (!parent) return JS_UNDEFINED;
    auto* doc = getDocumentForCtx(ctx);

    // Insert all new nodes before this element
    for (int i = 0; i < argc; ++i) {
        auto* node = nodeOrTextFromArg(ctx, argv[i]);
        if (!node) continue;
        parent->insertBefore(node, el);
        if (doc && node->nodeType() == bro::dom::NodeType::Element)
            doc->markStructureDirty();
        fireConnectedIfElement(ctx, node);
    }

    // Remove the old element
    if (doc && !el->id().empty())
        doc->unregisterElementId(el->id());
    if (doc) doc->markStructureDirty();
    JSValue w = DomBindings::wrapElement(ctx, el);
    fireDisconnectedCallback(ctx, w);
    JS_FreeValue(ctx, w);
    invalidateWrapper(ctx, el);
    parent->removeChild(el);
    if (doc) doc->freeNode(el);

    return JS_UNDEFINED;
}

static JSValue js_element_replaceChildren(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* doc = getDocumentForCtx(ctx);
    // Remove all existing children
    auto oldKids = el->childNodes();
    for (auto* child : oldKids) {
        if (child->nodeType() == bro::dom::NodeType::Element) {
            JSValue w = DomBindings::wrapElement(ctx, child);
            fireDisconnectedCallback(ctx, w);
            JS_FreeValue(ctx, w);
        }
        child->setParent(nullptr);
    }
    el->childNodes().clear();
    for (auto* child : oldKids) {
        if (child->nodeType() != bro::dom::NodeType::Element) {
            if (doc) doc->freeNode(child);
        }
    }
    // Append new children
    for (int i = 0; i < argc; ++i) {
        auto* node = nodeOrTextFromArg(ctx, argv[i]);
        if (!node) continue;
        el->appendChild(node);
        if (doc && node->nodeType() == bro::dom::NodeType::Element)
            doc->markStructureDirty();
        fireConnectedIfElement(ctx, node);
    }
    return JS_UNDEFINED;
}

static JSValue js_element_insertAdjacentElement(JSContext* ctx, JSValueConst this_val,
                                                int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el || argc < 2) return JS_NULL;
    std::string pos = jsToStdString(ctx, argv[0]);
    auto* newEl = unwrapNode(ctx, argv[1]);
    if (!newEl) return JS_NULL;
    auto* doc = getDocumentForCtx(ctx);

    if (pos == "beforebegin") {
        auto* parent = el->parentNode();
        if (parent) parent->insertBefore(newEl, el);
    } else if (pos == "afterbegin") {
        auto* first = el->childNodes().empty() ? nullptr : el->childNodes().front();
        el->insertBefore(newEl, first);
    } else if (pos == "beforeend") {
        el->appendChild(newEl);
    } else if (pos == "afterend") {
        auto* parent = el->parentNode();
        if (parent) {
            bro::dom::Node* ref = nullptr;
            auto& siblings = parent->childNodes();
            for (size_t i = 0; i < siblings.size(); ++i) {
                if (siblings[i] == el && i + 1 < siblings.size()) {
                    ref = siblings[i + 1];
                    break;
                }
            }
            parent->insertBefore(newEl, ref);
        }
    }
    if (doc && newEl->nodeType() == bro::dom::NodeType::Element)
        doc->markStructureDirty();
    fireConnectedIfElement(ctx, newEl);
    return argc >= 2 ? JS_DupValue(ctx, argv[1]) : JS_NULL;
}

static JSValue js_element_insertAdjacentText(JSContext* ctx, JSValueConst this_val,
                                             int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el || argc < 2) return JS_UNDEFINED;
    std::string pos = jsToStdString(ctx, argv[0]);
    std::string text = jsToStdString(ctx, argv[1]);
    auto* doc = getDocumentForCtx(ctx);
    if (!doc) return JS_UNDEFINED;
    auto* textNode = doc->createTextNode(text);
    if (!textNode) return JS_UNDEFINED;

    if (pos == "beforebegin") {
        auto* parent = el->parentNode();
        if (parent) parent->insertBefore(textNode, el);
    } else if (pos == "afterbegin") {
        auto* first = el->childNodes().empty() ? nullptr : el->childNodes().front();
        el->insertBefore(textNode, first);
    } else if (pos == "beforeend") {
        el->appendChild(textNode);
    } else if (pos == "afterend") {
        auto* parent = el->parentNode();
        if (parent) {
            bro::dom::Node* ref = nullptr;
            auto& siblings = parent->childNodes();
            for (size_t i = 0; i < siblings.size(); ++i) {
                if (siblings[i] == el && i + 1 < siblings.size()) {
                    ref = siblings[i + 1];
                    break;
                }
            }
            parent->insertBefore(textNode, ref);
        }
    }
    return JS_UNDEFINED;
}

// ---- Attribute methods (additional) ---------------------------------------

static JSValue js_element_toggleAttribute(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_FALSE;
    std::string name = jsToStdString(ctx, argv[0]);
    if (argc >= 2) {
        bool force = JS_ToBool(ctx, argv[1]);
        if (force) {
            el->setAttribute(name, "");
            return JS_TRUE;
        } else {
            el->removeAttribute(name);
            return JS_FALSE;
        }
    }
    if (el->hasAttribute(name)) {
        el->removeAttribute(name);
        return JS_FALSE;
    } else {
        el->setAttribute(name, "");
        return JS_TRUE;
    }
}

static JSValue js_element_getAttributeNames(JSContext* ctx, JSValueConst this_val,
                                            int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewArray(ctx);
    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    for (auto& [name, val] : el->attributes()) {
        JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, name.c_str()));
    }
    return arr;
}

// ---- dataset proxy --------------------------------------------------------

static std::string camelToDataAttr(const std::string& camel) {
    std::string attr = "data-";
    for (char c : camel) {
        if (std::isupper(static_cast<unsigned char>(c))) {
            attr += '-';
            attr += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            attr += c;
        }
    }
    return attr;
}

static JSValue js_element_get_dataset(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewObject(ctx);

    // Return cached dataset Proxy if available
    JSValue cached = JS_GetPropertyStr(ctx, this_val, "__bro_dataset");
    if (!JS_IsUndefined(cached) && !JS_IsNull(cached))
        return cached;
    JS_FreeValue(ctx, cached);

    // Build the target object with current data-* attributes
    JSValue target = JS_NewObject(ctx);
    for (auto& [name, val] : el->attributes()) {
        if (name.size() > 5 && name.substr(0, 5) == "data-") {
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
            JS_SetPropertyStr(ctx, target, key.c_str(), JS_NewString(ctx, val.c_str()));
        }
    }

    // Store element reference on the target for the Proxy handler to use
    JS_SetPropertyStr(ctx, target, "__bro_el_id",
                      JS_NewInt32(ctx, static_cast<int32_t>(el->nodeId())));

    // Create a Proxy that intercepts set/deleteProperty to update data-* attrs
    JSValue proxyFactory = JS_Eval(ctx, js_dataset_proxy, strlen(js_dataset_proxy),
                                   "<dataset-proxy>", JS_EVAL_TYPE_GLOBAL);
    JSValue proxy = JS_Call(ctx, proxyFactory, JS_UNDEFINED, 1, &target);
    JS_FreeValue(ctx, proxyFactory);
    JS_FreeValue(ctx, target);
    // Cache the proxy on the element wrapper
    JS_SetPropertyStr(ctx, this_val, "__bro_dataset", JS_DupValue(ctx, proxy));
    return proxy;
}

// ---- <template>.content ---------------------------------------------------

static JSValue js_element_get_content(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    if (el->tagName() != "TEMPLATE" && el->tagName() != "template")
        return JS_UNDEFINED;

    auto* doc = getDocumentForCtx(ctx);
    if (!doc) return JS_UNDEFINED;

    std::string storedHtml = el->getAttribute("data-bro-template-html");
    if (!storedHtml.empty()) {
        auto* temp = doc->createElement("div");
        doc->parseInnerHTML(temp, storedHtml);

        auto* frag = doc->createElement("#DOCUMENT-FRAGMENT");
        auto kids = temp->childNodes();
        for (auto* kid : kids) {
            frag->appendChild(kid);
        }
        doc->freeNode(temp);
        return DomBindings::wrapElement(ctx, frag);
    }

    auto* frag = doc->createElement("#DOCUMENT-FRAGMENT");
    if (!frag) return JS_NULL;

    for (auto* child : el->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element) {
            JSValue childJs = DomBindings::wrapElement(ctx, child);
            JSValue trueVal = JS_TRUE;
            JSValue clonedJs = js_element_cloneNode(ctx, childJs, 1, &trueVal);
            auto* clonedElem = static_cast<bro::dom::Element*>(
                DomBindings::unwrapElement(ctx, clonedJs));
            if (clonedElem) frag->appendChild(clonedElem);
            JS_FreeValue(ctx, clonedJs);
            JS_FreeValue(ctx, childJs);
        } else if (child->nodeType() == bro::dom::NodeType::Text) {
            auto* text = static_cast<bro::dom::TextNode*>(child);
            auto* cloned = doc->createTextNode(text->data());
            frag->appendChild(cloned);
        }
    }

    return DomBindings::wrapElement(ctx, frag);
}

// ---- Shadow DOM support (on Element) --------------------------------------

static JSValue js_element_attachShadow(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el) return JS_ThrowTypeError(ctx, "Invalid element");

    bro::dom::ShadowRoot::Mode mode = bro::dom::ShadowRoot::Mode::Open;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue modeVal = JS_GetPropertyStr(ctx, argv[0], "mode");
        const char* modeStr = JS_ToCString(ctx, modeVal);
        if (modeStr) {
            if (std::string(modeStr) == "closed")
                mode = bro::dom::ShadowRoot::Mode::Closed;
            JS_FreeCString(ctx, modeStr);
        }
        JS_FreeValue(ctx, modeVal);
    }

    auto* sr = el->attachShadow(mode);
    if (!sr) return JS_ThrowTypeError(ctx, "Element already has a shadow root");
    return wrapShadowRoot(ctx, sr);
}

static JSValue js_element_get_assignedSlot(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NULL;
    auto* parent = el->parentElement();
    if (!parent || !parent->hasShadow()) return JS_NULL;
    auto* sr = parent->shadowRoot();
    if (!sr) return JS_NULL;
    auto* slot = sr->assignedSlot(el);
    if (!slot) return JS_NULL;
    return DomBindings::wrapElement(ctx, slot);
}

static JSValue js_element_assignedNodes(JSContext* ctx, JSValueConst this_val,
                                        int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewArray(ctx);
    if (el->tagName() != "SLOT") return JS_NewArray(ctx);
    auto* sr = el->containingShadowRoot();
    if (!sr) return JS_NewArray(ctx);
    auto nodes = sr->assignedNodes(el);
    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    for (auto* node : nodes) {
        JSValue w;
        if (node->nodeType() == bro::dom::NodeType::Element)
            w = DomBindings::wrapElement(ctx, node);
        else
            w = wrapAnyNode(ctx, node);
        JS_SetPropertyUint32(ctx, arr, idx++, w);
    }
    return arr;
}

static JSValue js_element_assignedElements(JSContext* ctx, JSValueConst this_val,
                                           int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewArray(ctx);
    if (el->tagName() != "SLOT") return JS_NewArray(ctx);
    auto* sr = el->containingShadowRoot();
    if (!sr) return JS_NewArray(ctx);
    auto nodes = sr->assignedNodes(el);
    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    for (auto* node : nodes) {
        if (node->nodeType() == bro::dom::NodeType::Element) {
            JS_SetPropertyUint32(ctx, arr, idx++,
                                 DomBindings::wrapElement(ctx, node));
        }
    }
    return arr;
}

static JSValue js_element_get_shadowRoot(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NULL;
    auto* sr = el->shadowRoot();
    if (!sr) return JS_NULL;
    if (sr->mode() == bro::dom::ShadowRoot::Mode::Closed) return JS_NULL;
    return wrapShadowRoot(ctx, sr);
}

// ===========================================================================
// Element prototype function list
// ===========================================================================

static const JSCFunctionListEntry js_element_proto_funcs[] = {
    // Properties
    JS_CGETSET_DEF("id",            js_element_get_id,          js_element_set_id),
    JS_CGETSET_DEF("slot",          js_element_get_slot,        js_element_set_slot),
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
    JS_CGETSET_DEF("isConnected",            js_element_get_isConnected,            nullptr),
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
    JS_CGETSET_DEF("outerHTML",     js_element_get_outerHTML, js_element_set_outerHTML),
    JS_CGETSET_DEF("innerText",     js_element_get_innerText, nullptr),
    JS_CGETSET_DEF("dataset",       js_element_get_dataset, nullptr),
    JS_CGETSET_DEF("ownerDocument", js_element_get_ownerDocument, nullptr),
    JS_CGETSET_DEF("content",      js_element_get_content, nullptr),
    // Form control properties
    JS_CGETSET_DEF("value",       js_element_get_value,       js_element_set_value),
    JS_CGETSET_DEF("checked",     js_element_get_checked,     js_element_set_checked),
    JS_CGETSET_DEF("selected",      js_element_get_selected,      js_element_set_selected),
    JS_CGETSET_DEF("selectedIndex", js_element_get_selectedIndex, js_element_set_selectedIndex),
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
    JS_CFUNC_DEF("click",                     0, js_element_click),
    JS_CFUNC_DEF("focus",                     0, js_element_focus),
    JS_CFUNC_DEF("blur",                      0, js_element_blur),
    JS_CFUNC_DEF("insertAdjacentHTML",        2, js_element_insertAdjacentHTML),
    JS_CFUNC_DEF("insertAdjacentElement",    2, js_element_insertAdjacentElement),
    JS_CFUNC_DEF("insertAdjacentText",       2, js_element_insertAdjacentText),
    JS_CFUNC_DEF("append",                   0, js_element_append),
    JS_CFUNC_DEF("prepend",                  0, js_element_prepend),
    JS_CFUNC_DEF("before",                   0, js_element_before),
    JS_CFUNC_DEF("after",                    0, js_element_after),
    JS_CFUNC_DEF("replaceWith",              0, js_element_replaceWith),
    JS_CFUNC_DEF("replaceChildren",          0, js_element_replaceChildren),
    JS_CFUNC_DEF("toggleAttribute",          1, js_element_toggleAttribute),
    JS_CFUNC_DEF("getAttributeNames",        0, js_element_getAttributeNames),
    JS_CFUNC_DEF("getContext",                1, js_element_getContext),
    JS_CFUNC_DEF("scrollIntoView",            0, js_element_scrollIntoView),
    JS_CFUNC_DEF("getRootNode",              0, js_element_getRootNode),
    JS_CFUNC_DEF("hasChildNodes",            0, js_element_hasChildNodes),
    JS_CFUNC_DEF("attachShadow",              1, js_element_attachShadow),
    JS_CGETSET_DEF("shadowRoot",   js_element_get_shadowRoot, nullptr),
    // Slot APIs
    JS_CGETSET_DEF("assignedSlot", js_element_get_assignedSlot, nullptr),
    JS_CFUNC_DEF("assignedNodes",             0, js_element_assignedNodes),
    JS_CFUNC_DEF("assignedElements",          0, js_element_assignedElements),
};

// ===========================================================================
// Registration
// ===========================================================================

void registerElementClasses(JSRuntime* rt) {
    JS_NewClass(rt, js_element_class_id, &js_element_class);
}

void installElementPrototypes(JSContext* ctx) {
    JSValue elem_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, elem_proto, js_element_proto_funcs,
                               sizeof(js_element_proto_funcs) / sizeof(js_element_proto_funcs[0]));
    JS_SetClassProto(ctx, js_element_class_id, elem_proto);
}

} // namespace bro::js
