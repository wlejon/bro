#include "js/dom_bindings_internal.h"
#include "js/custom_elements.h"
#include "js/event_dispatch.h"
#include "util/log.h"
#include "dom/event.h"
#include "dom/shadow_root.h"
#include "engine/engine.h"
#include "dom/document.h"
#include "layout/el_input.h"
#include "layout/el_select.h"
#include "layout/el_textarea.h"
#include "layout/el_video.h"
#include "canvas/canvas_scene.h"
#include "js/imagebitmap_bindings.h"
#include "css/transform.h"
#include "dom/element_geometry.h"
#include "layout/svg_geometry.h"

#include <qjsbind/qjsbind.h>

extern "C" {
#include "libregexp.h"
}

#include "dataset_proxy.js.h"

#include <algorithm>
#include <cmath>
#include <functional>

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

bool isElementFinalizerShutdown() {
    return s_shutting_down;
}

// Release orphaned elements when the JS wrapper is garbage-collected.
static void js_element_finalizer(JSRuntime* rt, JSValue val)
{
    auto* el = static_cast<bro::dom::Element*>(
        JS_GetOpaque(val, js_element_class_id));
    if (!el) return;

    if (s_shutting_down) return;

    // Backstop for the wrapper pointer cache: whatever removed this wrapper from
    // the element map, the object is now being collected, so make sure the
    // element no longer points at it. Guards the fast path in wrapElement()
    // against a freed wrapper even if an eager-clear site was missed.
    if (el->isAlive() && el->jsWrapper() == JS_VALUE_GET_PTR(val))
        el->setJsWrapper(nullptr);

    // Detached-document (DOMParser) bookkeeping: this wrapper is leaving the
    // weak registry, so the document holder will never touch it again. Safe to
    // dereference el here — a non-null opaque means neither the holder nor
    // fireNodeFreed invalidated it, so its document is still alive.
    dropDetachedElementWrapper(el, JS_VALUE_GET_PTR(val));

    // Free any cached canvas context for this element
    auto ccIt = s_canvas_contexts.find(el);
    if (ccIt != s_canvas_contexts.end()) {
        JS_FreeValueRT(rt, ccIt->second.val);
        s_canvas_contexts.erase(ccIt);
    }

    if (!el->isAlive()) return;

    // Offscreen canvases (document.createElement('canvas') with no append)
    // are pinned to life by the JS wrapper alone. When the wrapper is GC'd,
    // mark the backing CanvasScene detached so the engine's per-frame cleanup
    // pass collects it — and clear the scene's callback userdata before the
    // Element is freed below, so it never dereferences a dangling pointer.
    if (auto* cs = static_cast<bro::canvas::CanvasScene*>(el->canvasScene())) {
        cs->onElementFinalized();
        el->setCanvasScene(nullptr);
    }

    if (!el->parentNode()) {
        auto* doc = el->document();
        // Never free the document element: for a detached (DOMParser) document
        // it is the parentless ROOT of a tree that stays reachable through the
        // Document wrapper — collecting its (uncached-strongly) wrapper must
        // not eat the whole parsed tree. The main document's root never gets
        // here (its wrapper is pinned by __bro_elem_map until shutdown).
        if (doc && doc->documentElement() != el) doc->freeNode(el);
    }
}

// JSClassDef replaced by qjsbind::Class in installElementBindings()

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

    // The map entry (and the strong ref it holds) is about to go, and the
    // wrapper's opaque is nulled below — so drop the element's fast-path pointer
    // to it first, or a later wrap would return the now-inert wrapper.
    elem->setJsWrapper(nullptr);

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
            if (child->nodeType() == bro::dom::NodeType::Element) {
                // Detach without nuking the JS wrapper — JS may still hold a
                // reference and re-insert this element (e.g. jQuery's
                // buildFragment uses textContent="" to clear a temp and then
                // reparents the children). Just unregister the id so the
                // detached subtree doesn't match document.getElementById.
                auto* elChild = static_cast<bro::dom::Element*>(child);
                if (doc && !elChild->id().empty()) doc->unregisterElementId(elChild->id());
            } else {
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

    std::string text = jsToStdString(ctx, val);
    if (hasObservers) {
        JSValue removedArr = JS_NewArray(ctx);
        uint32_t rmIdx = 0;
        for (auto* child : el->childNodes()) {
            JS_SetPropertyUint32(ctx, removedArr, rmIdx++, wrapAnyNode(ctx, child));
        }
        el->setTextContent(text);
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
        el->setTextContent(text);
    }
    // A scroll container gets stuck to the bottom when text is appended into it
    // (the log/console pattern). But clearing it — textContent = "" — is the
    // opposite intent: it must NOT auto-scroll, or a subsequent rebuild via
    // appendChild lands scrolled to the bottom. So only stick on non-empty text.
    if (!text.empty()) {
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
            if (child->nodeType() == bro::dom::NodeType::Element) {
                // Detach without nuking the JS wrapper — see matching note
                // in js_element_set_textContent above.
                auto* elChild = static_cast<bro::dom::Element*>(child);
                if (doc && !elChild->id().empty()) doc->unregisterElementId(elChild->id());
            } else {
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
        upgradeCustomElementsInSubtree(ctx, el);
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
        upgradeCustomElementsInSubtree(ctx, el);
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
    // Document fragments created via document.createDocumentFragment() are
    // modeled as Elements with a reserved tag name. Report the spec nodeType
    // so JS code (jQuery's parseHTML / buildFragment, etc.) can distinguish.
    if (el->tagName() == "#DOCUMENT-FRAGMENT") return JS_NewInt32(ctx, 11);
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
    if (el->tagName() == "SELECT" || el->tagName() == "select") {
        // No ElSelect yet (no layout pass has touched this element) —
        // read straight off the DOM instead of falling through to the
        // generic getAttribute("value") below, which is always empty for
        // a <select> (only its <option> children carry a value). Mirrors
        // js_element_set_value's DOM-attribute fallback so get/set stay
        // consistent before first layout — e.g. a headless script that
        // creates a <select>, sets .value, and reads it back without an
        // intervening flush()/layout.
        // Spec: an option's value falls back to its text only when the value
        // attribute is ABSENT — an explicit value="" stays "" (placeholder
        // options depend on it).
        auto optionValue = [](bro::dom::Element* o) {
            return o->hasAttribute("value") ? o->getAttribute("value")
                                            : o->textContent();
        };
        bro::dom::Element* first = nullptr;
        for (auto* child : el->children()) {
            if (child->tagName() != "OPTION" && child->tagName() != "option") continue;
            if (!first) first = child;
            if (child->hasAttribute("selected")) {
                std::string ov = optionValue(child);
                return JS_NewString(ctx, ov.c_str());
            }
        }
        if (first) {
            std::string ov = optionValue(first);
            return JS_NewString(ctx, ov.c_str());
        }
        return JS_NewString(ctx, "");
    }
    // <textarea>: the live value lives in the "value" attribute once any
    // edit has happened; before that, fall back to textContent (initial
    // content from HTML, e.g. `<textarea>foo</textarea>`).
    const std::string& tag = el->tagName();
    if (tag == "TEXTAREA" || tag == "textarea") {
        if (el->hasAttribute("value"))
            return JS_NewString(ctx, el->getAttribute("value").c_str());
        std::string t = el->textContent();
        return JS_NewString(ctx, t.c_str());
    }
    return JS_NewString(ctx, el->getAttribute("value").c_str());
}

static JSValue js_element_set_value(JSContext* ctx, JSValueConst this_val,
                                    JSValueConst val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    std::string s = jsToStdString(ctx, val);
    // For <select>, sync the selection with the new value. We do BOTH:
    //   1. Stamp the `selected` attribute on the matching <option> (and
    //      clear it from siblings). This is what initSelectedIndex reads
    //      when the SelectControl is lazily created during the first
    //      layout pass — required because callers commonly run
    //      `select.value = "..."` at script-load time, before any render
    //      has triggered ElSelect construction.
    //   2. If the SelectControl already exists, also update its
    //      selectedIndex directly so the displayed selection moves
    //      immediately without waiting for a re-layout.
    if (el->tagName() == "SELECT" || el->tagName() == "select") {
        int matchIdx = -1, idx = 0;
        for (auto* child : el->children()) {
            if (child->tagName() != "OPTION" && child->tagName() != "option") continue;
            // Value-attribute-absent → text fallback; explicit value="" stays
            // "" (matches the getter and ElSelect::getOptions()).
            std::string ov = child->hasAttribute("value")
                                 ? child->getAttribute("value")
                                 : child->textContent();
            if (matchIdx < 0 && ov == s) {
                matchIdx = idx;
                child->setAttribute("selected", "");
            } else if (child->hasAttribute("selected")) {
                child->removeAttribute("selected");
            }
            ++idx;
        }
        if (auto* sel = el->selectControl(); sel && matchIdx >= 0) {
            sel->setSelectedIndex(matchIdx);
        }
        return JS_UNDEFINED;
    }
    // <textarea>: write to the "value" attribute, which is the storage
    // shared with the typing pipeline (handleKeyDown / handleTextInput).
    // textContent stays as the initial HTML content (defaultValue).
    if (el->tagName() == "TEXTAREA" || el->tagName() == "textarea") {
        el->setAttribute("value", s);
        // A programmatic value write invalidates the control's undo history
        // (browser behavior — Ctrl+Z can't cross a script's rewrite).
        if (auto* ta = el->textareaControl()) ta->clearHistory();
        return JS_UNDEFINED;
    }
    el->setAttribute("value", s);
    if (auto* inp = el->inputControl()) inp->clearHistory();
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// <video> / HTMLMediaElement bindings — dispatch through Element::videoControl()
// ---------------------------------------------------------------------------

// Fire a spec-trusted, non-bubbling, non-cancelable media event.
static void fireMediaEvent(JSContext* ctx, bro::dom::Element* el, const char* type) {
    if (!ctx || !el) return;
    bro::dom::Event evt(type, false, false);
    evt.setIsTrusted(true);
    dispatchDomEvent(ctx, el, evt);
}

static JSValue js_element_video_play(JSContext* ctx, JSValueConst this_val,
                                     int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (el) if (auto* v = el->videoControl()) {
        bool wasPaused = !v->isPlaying();
        v->play();
        if (wasPaused) fireMediaEvent(ctx, el, "play");
    }
    // HTMLMediaElement.play() returns Promise<void>. Callers may or may not
    // await; returning a resolved promise covers both shapes.
    return qjsbind::make_resolved_promise(ctx, JS_UNDEFINED);
}

static JSValue js_element_video_pause(JSContext* ctx, JSValueConst this_val,
                                      int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (el) if (auto* v = el->videoControl()) {
        bool wasPlaying = v->isPlaying();
        v->pause();
        if (wasPlaying) fireMediaEvent(ctx, el, "pause");
    }
    return JS_UNDEFINED;
}

static JSValue js_element_video_load(JSContext* ctx, JSValueConst this_val,
                                     int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* v = el->videoControl();
    if (!v) return JS_UNDEFINED;
    std::string src = el->getAttribute("src");
    if (src.empty()) return JS_UNDEFINED;
    v->load(src);
    return JS_UNDEFINED;
}

static JSValue js_element_video_canPlayType(JSContext* ctx, JSValueConst /*this_val*/,
                                            int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewString(ctx, "");
    std::string mime = jsToStdString(ctx, argv[0]);
    // Lowercase prefix match — bro ships a VP9/Opus WebM pipeline.
    auto startsWith = [&](const char* p) {
        size_t n = strlen(p);
        return mime.size() >= n &&
               std::equal(mime.begin(), mime.begin() + n, p,
                          [](char a, char b){ return std::tolower((unsigned char)a) == b; });
    };
    if (startsWith("video/webm") || startsWith("audio/webm") ||
        startsWith("audio/ogg") || mime.find("opus") != std::string::npos ||
        mime.find("vp9") != std::string::npos || mime.find("vp8") != std::string::npos) {
        return JS_NewString(ctx, "probably");
    }
    return JS_NewString(ctx, "");
}

static JSValue js_element_video_get_currentTime(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    double t = 0.0;
    if (el) if (auto* v = el->videoControl()) t = v->currentTime();
    return JS_NewFloat64(ctx, t);
}

static JSValue js_element_video_set_currentTime(JSContext* ctx, JSValueConst this_val,
                                                JSValueConst val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    double t = 0.0;
    JS_ToFloat64(ctx, &t, val);
    if (auto* v = el->videoControl()) {
        fireMediaEvent(ctx, el, "seeking");
        v->seekTo(t);
        fireMediaEvent(ctx, el, "seeked");
        fireMediaEvent(ctx, el, "timeupdate");
    }
    return JS_UNDEFINED;
}

static JSValue js_element_video_get_duration(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewFloat64(ctx, std::nan(""));
    auto* v = el->videoControl();
    if (!v || !v->hasPipeline()) return JS_NewFloat64(ctx, std::nan(""));
    return JS_NewFloat64(ctx, v->duration());
}

static JSValue js_element_video_get_paused(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_TRUE;
    if (auto* v = el->videoControl()) return JS_NewBool(ctx, !v->isPlaying());
    return JS_TRUE;
}

static JSValue js_element_video_get_ended(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_FALSE;
    if (auto* v = el->videoControl()) return JS_NewBool(ctx, v->isEnded());
    return JS_FALSE;
}

static JSValue js_element_video_get_seeking(JSContext* ctx, JSValueConst /*this_val*/) {
    // bro's seekTo() is synchronous from the JS perspective — seeking completes
    // before the setter returns, so `seeking` is never observably true.
    return JS_FALSE;
}

static JSValue js_element_video_get_readyState(JSContext* ctx, JSValueConst this_val) {
    // HAVE_NOTHING=0, HAVE_METADATA=1, HAVE_CURRENT_DATA=2,
    // HAVE_FUTURE_DATA=3, HAVE_ENOUGH_DATA=4.
    auto* el = getElement(this_val);
    if (!el) return JS_NewInt32(ctx, 0);
    auto* v = el->videoControl();
    if (!v || !v->hasPipeline()) return JS_NewInt32(ctx, 0);
    if (v->isReady()) return JS_NewInt32(ctx, 4);
    return JS_NewInt32(ctx, 1);
}

static JSValue js_element_video_get_networkState(JSContext* ctx, JSValueConst this_val) {
    // NETWORK_EMPTY=0, NETWORK_IDLE=1, NETWORK_LOADING=2, NETWORK_NO_SOURCE=3.
    auto* el = getElement(this_val);
    if (!el) return JS_NewInt32(ctx, 0);
    std::string src = el->getAttribute("src");
    if (src.empty()) return JS_NewInt32(ctx, 0);
    auto* v = el->videoControl();
    if (v && v->hasPipeline()) return JS_NewInt32(ctx, 1);
    return JS_NewInt32(ctx, 3);
}

static JSValue js_element_video_get_currentSrc(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewString(ctx, "");
    // currentSrc reflects the resolved URL of the currently-loaded resource,
    // not the raw src attribute. Empty until load() has picked a resource.
    if (auto* v = el->videoControl()) {
        return JS_NewString(ctx, v->currentSrc().c_str());
    }
    return JS_NewString(ctx, "");
}

static JSValue js_element_video_get_videoWidth(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (el) if (auto* v = el->videoControl()) return JS_NewInt32(ctx, v->videoWidth());
    return JS_NewInt32(ctx, 0);
}

static JSValue js_element_video_get_videoHeight(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (el) if (auto* v = el->videoControl()) return JS_NewInt32(ctx, v->videoHeight());
    return JS_NewInt32(ctx, 0);
}

// ---- Attribute-reflected media properties -----------------------------------

static JSValue js_element_video_get_src(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewString(ctx, "");
    return JS_NewString(ctx, el->getAttribute("src").c_str());
}

static JSValue js_element_video_set_src(JSContext* ctx, JSValueConst this_val,
                                        JSValueConst val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    std::string s = jsToStdString(ctx, val);
    el->setAttribute("src", s);
    // Setting src triggers a fresh resource selection — reload via the pipeline.
    if (auto* v = el->videoControl()) {
        if (!s.empty()) v->load(s);
    }
    // <iframe src=...>: assigning src (re)loads the embedded sub-document.
    if (el->tagName() == "IFRAME" || el->tagName() == "iframe") {
        auto it = s_ctx_engines.find(ctx);
        if (it != s_ctx_engines.end() && it->second)
            static_cast<bro::engine::Engine*>(it->second)->reloadIframe(el);
    }
    return JS_UNDEFINED;
}

// iframe.reload() — rebuild the embedded sub-document from its current src.
static JSValue js_element_iframe_reload(JSContext* ctx, JSValueConst this_val,
                                        int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    if (el->tagName() != "IFRAME" && el->tagName() != "iframe") return JS_UNDEFINED;
    auto it = s_ctx_engines.find(ctx);
    if (it != s_ctx_engines.end() && it->second)
        static_cast<bro::engine::Engine*>(it->second)->reloadIframe(el);
    return JS_UNDEFINED;
}

// iframe.capture() — read back the pixels the embedded sub-document last
// rendered as an ImageData ({width, height, data:Uint8ClampedArray}, top-down
// RGBA). This is the host's "look": the same frame the user sees, handed to
// script for encoding/vision. Returns null if the iframe hasn't rendered yet
// (call after the 'load' event and a rendered frame).
static JSValue js_element_iframe_capture(JSContext* ctx, JSValueConst this_val,
                                         int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_NULL;
    if (el->tagName() != "IFRAME" && el->tagName() != "iframe") return JS_NULL;
    auto it = s_ctx_engines.find(ctx);
    if (it == s_ctx_engines.end() || !it->second) return JS_NULL;

    int w = 0, h = 0;
    auto pixels = static_cast<bro::engine::Engine*>(it->second)
                      ->captureIframe(el, w, h);
    if (pixels.empty() || w <= 0 || h <= 0) return JS_NULL;

    JSValue abuf = JS_NewArrayBufferCopy(ctx, pixels.data(), pixels.size());
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue u8cCtor = JS_GetPropertyStr(ctx, global, "Uint8ClampedArray");
    JSValue dataArr = JS_CallConstructor(ctx, u8cCtor, 1, &abuf);
    JS_FreeValue(ctx, u8cCtor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, abuf);

    return ImageBitmapBindings::makeImageData(ctx, w, h, dataArr);
}

static JSValue js_element_video_get_autoplay(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    return JS_NewBool(ctx, el && el->hasAttribute("autoplay"));
}

static JSValue js_element_video_set_autoplay(JSContext* ctx, JSValueConst this_val,
                                             JSValueConst val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    if (JS_ToBool(ctx, val)) el->setAttribute("autoplay", "");
    else el->removeAttribute("autoplay");
    return JS_UNDEFINED;
}

static JSValue js_element_video_get_controls(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    return JS_NewBool(ctx, el && el->hasAttribute("controls"));
}

static JSValue js_element_video_set_controls(JSContext* ctx, JSValueConst this_val,
                                             JSValueConst val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    if (JS_ToBool(ctx, val)) el->setAttribute("controls", "");
    else el->removeAttribute("controls");
    return JS_UNDEFINED;
}

static JSValue js_element_video_get_loop(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    return JS_NewBool(ctx, el && el->hasAttribute("loop"));
}

static JSValue js_element_video_set_loop(JSContext* ctx, JSValueConst this_val,
                                         JSValueConst val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    bool b = JS_ToBool(ctx, val);
    if (b) el->setAttribute("loop", "");
    else el->removeAttribute("loop");
    if (auto* v = el->videoControl()) v->setLoopEnabled(b);
    return JS_UNDEFINED;
}

static JSValue js_element_video_get_preload(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewString(ctx, "metadata");
    std::string p = el->getAttribute("preload");
    return JS_NewString(ctx, p.empty() ? "metadata" : p.c_str());
}

static JSValue js_element_video_set_preload(JSContext* ctx, JSValueConst this_val,
                                            JSValueConst val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    el->setAttribute("preload", jsToStdString(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_element_video_get_defaultMuted(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    return JS_NewBool(ctx, el && el->hasAttribute("muted"));
}

static JSValue js_element_video_set_defaultMuted(JSContext* ctx, JSValueConst this_val,
                                                 JSValueConst val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    if (JS_ToBool(ctx, val)) el->setAttribute("muted", "");
    else el->removeAttribute("muted");
    return JS_UNDEFINED;
}

// ---- Pipeline-backed state props --------------------------------------------

static JSValue js_element_video_get_volume(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewFloat64(ctx, 1.0);
    if (auto* v = el->videoControl()) return JS_NewFloat64(ctx, v->volume());
    return JS_NewFloat64(ctx, 1.0);
}

static JSValue js_element_video_set_volume(JSContext* ctx, JSValueConst this_val,
                                           JSValueConst val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* v = el->videoControl();
    if (!v) return JS_UNDEFINED;
    double d = 1.0;
    JS_ToFloat64(ctx, &d, val);
    double prev = v->volume();
    v->setVolume(d);
    if (v->volume() != prev) fireMediaEvent(ctx, el, "volumechange");
    return JS_UNDEFINED;
}

static JSValue js_element_video_get_muted(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_FALSE;
    if (auto* v = el->videoControl()) return JS_NewBool(ctx, v->muted());
    // Before the pipeline attaches, fall back to the attribute (default muted).
    return JS_NewBool(ctx, el->hasAttribute("muted"));
}

static JSValue js_element_video_set_muted(JSContext* ctx, JSValueConst this_val,
                                          JSValueConst val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    bool b = JS_ToBool(ctx, val);
    auto* v = el->videoControl();
    if (!v) return JS_UNDEFINED;
    bool prev = v->muted();
    v->setMuted(b);
    if (v->muted() != prev) fireMediaEvent(ctx, el, "volumechange");
    return JS_UNDEFINED;
}

static JSValue js_element_video_get_playbackRate(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewFloat64(ctx, 1.0);
    if (auto* v = el->videoControl()) return JS_NewFloat64(ctx, v->playbackRate());
    return JS_NewFloat64(ctx, 1.0);
}

static JSValue js_element_video_set_playbackRate(JSContext* ctx, JSValueConst this_val,
                                                 JSValueConst val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* v = el->videoControl();
    if (!v) return JS_UNDEFINED;
    double d = 1.0;
    JS_ToFloat64(ctx, &d, val);
    double prev = v->playbackRate();
    v->setPlaybackRate(d);
    if (v->playbackRate() != prev) fireMediaEvent(ctx, el, "ratechange");
    return JS_UNDEFINED;
}

static JSValue js_element_video_get_defaultPlaybackRate(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewFloat64(ctx, 1.0);
    if (auto* v = el->videoControl()) return JS_NewFloat64(ctx, v->defaultPlaybackRate());
    return JS_NewFloat64(ctx, 1.0);
}

static JSValue js_element_video_set_defaultPlaybackRate(JSContext* ctx, JSValueConst this_val,
                                                        JSValueConst val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* v = el->videoControl();
    if (!v) return JS_UNDEFINED;
    double d = 1.0;
    JS_ToFloat64(ctx, &d, val);
    v->setDefaultPlaybackRate(d);
    return JS_UNDEFINED;
}

// ---- TimeRanges -------------------------------------------------------------

// Build a {length, start(i), end(i)} object representing a single [0, dur] run.
// Used for buffered/seekable/played — bro decodes sequentially with no gaps.
static JSValue buildTimeRanges(JSContext* ctx, double duration, bool present) {
    JSValue obj = JS_NewObject(ctx);
    int32_t length = present ? 1 : 0;
    JS_SetPropertyStr(ctx, obj, "length", JS_NewInt32(ctx, length));

    JSValue durVal = JS_NewFloat64(ctx, duration);

    JSValue startFn = JS_NewCFunctionData(ctx,
        [](JSContext* c, JSValueConst, int, JSValueConst*, int, JSValue*) -> JSValue {
            return JS_NewFloat64(c, 0.0);
        }, 1, 0, 0, nullptr);
    JSValue endFn = JS_NewCFunctionData(ctx,
        [](JSContext* c, JSValueConst, int argc, JSValueConst* argv,
           int, JSValue* fdata) -> JSValue {
            (void)argc; (void)argv;
            double d = 0.0;
            JS_ToFloat64(c, &d, fdata[0]);
            return JS_NewFloat64(c, d);
        }, 1, 0, 1, &durVal);
    JS_FreeValue(ctx, durVal);

    JS_SetPropertyStr(ctx, obj, "start", startFn);
    JS_SetPropertyStr(ctx, obj, "end", endFn);
    return obj;
}

static JSValue js_element_video_get_buffered(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return buildTimeRanges(ctx, 0.0, false);
    auto* v = el->videoControl();
    bool present = v && v->hasPipeline();
    return buildTimeRanges(ctx, v ? v->duration() : 0.0, present);
}

static JSValue js_element_video_get_seekable(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return buildTimeRanges(ctx, 0.0, false);
    auto* v = el->videoControl();
    bool present = v && v->hasPipeline();
    return buildTimeRanges(ctx, v ? v->duration() : 0.0, present);
}

static JSValue js_element_video_get_played(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return buildTimeRanges(ctx, 0.0, false);
    auto* v = el->videoControl();
    if (!v || !v->hasPipeline()) return buildTimeRanges(ctx, 0.0, false);
    return buildTimeRanges(ctx, v->currentTime(), v->currentTime() > 0.0);
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

// Recursively gather a <select>'s <option> descendants in document order,
// descending into <optgroup> per the HTMLOptionsCollection contract.
static void collectSelectOptions(bro::dom::Element* el,
                                 std::vector<bro::dom::Element*>& out)
{
    for (auto* child : el->children()) {
        const std::string& tag = child->tagName();
        if (tag == "OPTION" || tag == "option") {
            out.push_back(child);
        } else if (tag == "OPTGROUP" || tag == "optgroup") {
            collectSelectOptions(child, out);
        }
    }
}

// HTMLSelectElement.options — the list of <option> elements. Returned as a
// NodeList-shaped collection (indexed + length + iterable), which is all the
// HTMLOptionsCollection surface app code relies on (Array.from, [i], .length).
static JSValue js_element_get_options(JSContext* ctx, JSValueConst this_val)
{
    auto* el = getElement(this_val);
    if (!el) return JS_NewArray(ctx);
    const std::string& tag = el->tagName();
    if (tag != "SELECT" && tag != "select") return JS_UNDEFINED;
    std::vector<bro::dom::Element*> opts;
    collectSelectOptions(el, opts);
    return wrapNodeList(ctx, opts);
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
    // Boolean HTML attribute: presence of the attribute means true, regardless
    // of value. `<input disabled>` parses to disabled="" and still counts.
    return JS_NewBool(ctx, el->hasAttribute("disabled"));
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

// ---------------------------------------------------------------------------
// HTMLInputElement / HTMLTextAreaElement / HTMLSelectElement form IDL
// ---------------------------------------------------------------------------

// Boolean reflected attribute: presence of the attribute → true.
static JSValue bool_attr_get(JSContext* ctx, bro::dom::Element* el,
                             const char* name) {
    return JS_NewBool(ctx, el && el->hasAttribute(name));
}
static JSValue bool_attr_set(JSContext* ctx, bro::dom::Element* el,
                             const char* name, JSValueConst val) {
    if (!el) return JS_UNDEFINED;
    if (JS_ToBool(ctx, val)) el->setAttribute(name, "");
    else el->removeAttribute(name);
    return JS_UNDEFINED;
}
// String reflected attribute.
static JSValue str_attr_get(JSContext* ctx, bro::dom::Element* el,
                            const char* name) {
    if (!el) return JS_NewString(ctx, "");
    return JS_NewString(ctx, el->getAttribute(name).c_str());
}
static JSValue str_attr_set(JSContext* ctx, bro::dom::Element* el,
                            const char* name, JSValueConst val) {
    if (!el) return JS_UNDEFINED;
    el->setAttribute(name, jsToStdString(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_element_get_required(JSContext* ctx, JSValueConst this_val) {
    return bool_attr_get(ctx, getElement(this_val), "required");
}
static JSValue js_element_set_required(JSContext* ctx, JSValueConst this_val, JSValueConst v) {
    return bool_attr_set(ctx, getElement(this_val), "required", v);
}
static JSValue js_element_get_readOnly(JSContext* ctx, JSValueConst this_val) {
    return bool_attr_get(ctx, getElement(this_val), "readonly");
}
static JSValue js_element_set_readOnly(JSContext* ctx, JSValueConst this_val, JSValueConst v) {
    return bool_attr_set(ctx, getElement(this_val), "readonly", v);
}
static JSValue js_element_get_multiple(JSContext* ctx, JSValueConst this_val) {
    return bool_attr_get(ctx, getElement(this_val), "multiple");
}
static JSValue js_element_set_multiple(JSContext* ctx, JSValueConst this_val, JSValueConst v) {
    return bool_attr_set(ctx, getElement(this_val), "multiple", v);
}
static JSValue js_element_get_pattern(JSContext* ctx, JSValueConst this_val) {
    return str_attr_get(ctx, getElement(this_val), "pattern");
}
static JSValue js_element_set_pattern(JSContext* ctx, JSValueConst this_val, JSValueConst v) {
    return str_attr_set(ctx, getElement(this_val), "pattern", v);
}
static JSValue js_element_get_min(JSContext* ctx, JSValueConst this_val) {
    return str_attr_get(ctx, getElement(this_val), "min");
}
static JSValue js_element_set_min(JSContext* ctx, JSValueConst this_val, JSValueConst v) {
    return str_attr_set(ctx, getElement(this_val), "min", v);
}
static JSValue js_element_get_max(JSContext* ctx, JSValueConst this_val) {
    return str_attr_get(ctx, getElement(this_val), "max");
}
static JSValue js_element_set_max(JSContext* ctx, JSValueConst this_val, JSValueConst v) {
    return str_attr_set(ctx, getElement(this_val), "max", v);
}
static JSValue js_element_get_step(JSContext* ctx, JSValueConst this_val) {
    return str_attr_get(ctx, getElement(this_val), "step");
}
static JSValue js_element_set_step(JSContext* ctx, JSValueConst this_val, JSValueConst v) {
    return str_attr_set(ctx, getElement(this_val), "step", v);
}
static JSValue js_element_get_name(JSContext* ctx, JSValueConst this_val) {
    return str_attr_get(ctx, getElement(this_val), "name");
}
static JSValue js_element_set_name(JSContext* ctx, JSValueConst this_val, JSValueConst v) {
    return str_attr_set(ctx, getElement(this_val), "name", v);
}
static JSValue js_element_get_autocomplete(JSContext* ctx, JSValueConst this_val) {
    return str_attr_get(ctx, getElement(this_val), "autocomplete");
}
static JSValue js_element_set_autocomplete(JSContext* ctx, JSValueConst this_val, JSValueConst v) {
    return str_attr_set(ctx, getElement(this_val), "autocomplete", v);
}

// minLength / maxLength: numeric reflected attributes. Spec: default -1 if
// missing/invalid for minLength; default -1 (interpreted as "no limit") for
// maxLength on input/textarea.
static JSValue num_attr_get(JSContext* ctx, bro::dom::Element* el,
                            const char* name, int defaultVal) {
    if (!el || !el->hasAttribute(name)) return JS_NewInt32(ctx, defaultVal);
    const std::string& s = el->getAttribute(name);
    try { return JS_NewInt32(ctx, std::stoi(s)); }
    catch (...) { return JS_NewInt32(ctx, defaultVal); }
}
static JSValue js_element_get_minLength(JSContext* ctx, JSValueConst this_val) {
    return num_attr_get(ctx, getElement(this_val), "minlength", -1);
}
static JSValue js_element_set_minLength(JSContext* ctx, JSValueConst this_val, JSValueConst v) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    int32_t n = 0; JS_ToInt32(ctx, &n, v);
    el->setAttribute("minlength", std::to_string(n));
    return JS_UNDEFINED;
}
static JSValue js_element_get_maxLength(JSContext* ctx, JSValueConst this_val) {
    return num_attr_get(ctx, getElement(this_val), "maxlength", -1);
}
static JSValue js_element_set_maxLength(JSContext* ctx, JSValueConst this_val, JSValueConst v) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    int32_t n = 0; JS_ToInt32(ctx, &n, v);
    el->setAttribute("maxlength", std::to_string(n));
    return JS_UNDEFINED;
}
static JSValue js_element_get_size(JSContext* ctx, JSValueConst this_val) {
    return num_attr_get(ctx, getElement(this_val), "size", 0);
}
static JSValue js_element_set_size(JSContext* ctx, JSValueConst this_val, JSValueConst v) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    int32_t n = 0; JS_ToInt32(ctx, &n, v);
    el->setAttribute("size", std::to_string(n));
    return JS_UNDEFINED;
}

// form: nearest ancestor <form>, unless the control has a `form="id"` attr —
// then look up the document by id. Returns null if no owning form.
static JSValue js_element_get_form(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NULL;
    const std::string& formId = el->getAttribute("form");
    if (!formId.empty() && el->document()) {
        auto* owner = el->document()->getElementById(formId);
        if (owner && (owner->tagName() == "FORM" || owner->tagName() == "form"))
            return DomBindings::wrapElement(ctx, owner);
        return JS_NULL;
    }
    for (auto* p = el->parentElement(); p; p = p->parentElement()) {
        if (p->tagName() == "FORM" || p->tagName() == "form")
            return DomBindings::wrapElement(ctx, p);
    }
    return JS_NULL;
}

// ---- UTF-8 (internal) ⇄ UTF-16 (JS) selection-offset conversion -----------
// The text controls (ElInput / ElTextarea) store selection offsets as BYTE
// indices into the UTF-8 value. The web API speaks UTF-16 code units over the
// JS string (`value.slice(0, selectionStart)` must be coherent), so every
// JS-visible offset converts at this boundary. A 4-byte UTF-8 sequence
// (astral, e.g. emoji) is TWO UTF-16 units; 1–3-byte sequences are one.

// Length in bytes of the UTF-8 sequence starting with lead byte `c`. An
// invalid lead byte counts as a 1-byte / 1-unit character so walks terminate.
static int utf8SeqLenAt(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// Byte offset into `s` → UTF-16 code-unit index. Offsets landing mid-sequence
// resolve to the preceding character boundary. Clamped to [0, len].
static int utf8ByteToUtf16(const std::string& s, int byte) {
    const int n = static_cast<int>(s.size());
    byte = std::clamp(byte, 0, n);
    int i = 0, units = 0;
    while (i < byte) {
        const int len = utf8SeqLenAt(static_cast<unsigned char>(s[static_cast<size_t>(i)]));
        if (i + len > byte) break;  // mid-sequence → preceding boundary
        units += (len == 4) ? 2 : 1;
        i += len;
    }
    return units;
}

// UTF-16 code-unit index → byte offset into `s`. An index landing between the
// two units of a surrogate pair resolves to the preceding character boundary
// (the byte domain cannot name the middle of a code point). Clamped.
static int utf16ToUtf8Byte(const std::string& s, int u16) {
    const int n = static_cast<int>(s.size());
    if (u16 < 0) u16 = 0;
    int i = 0, units = 0;
    while (i < n && units < u16) {
        const int len = utf8SeqLenAt(static_cast<unsigned char>(s[static_cast<size_t>(i)]));
        const int u = (len == 4) ? 2 : 1;
        if (units + u > u16) break;  // mid-astral → preceding boundary
        units += u;
        i += std::min(len, n - i);
    }
    return i;
}

// Decode the UTF-8 value into UTF-16 code units (JS string semantics) —
// libregexp's 16-bit subject form, and what a `u`/`v`-flagged RegExp matches
// over. A truncated/invalid sequence decodes as U+FFFD, one unit per bad byte.
static std::vector<uint16_t> utf8ToUtf16Units(const std::string& s) {
    std::vector<uint16_t> out;
    out.reserve(s.size());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0xFFFD;
        int len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
            cp = (uint32_t(c & 0x1F) << 6) | (s[i + 1] & 0x3F);
            len = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
            cp = (uint32_t(c & 0x0F) << 12) | (uint32_t(s[i + 1] & 0x3F) << 6) |
                 (s[i + 2] & 0x3F);
            len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
            cp = (uint32_t(c & 0x07) << 18) | (uint32_t(s[i + 1] & 0x3F) << 12) |
                 (uint32_t(s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
            len = 4;
        }
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
            cp -= 0x10000;
            out.push_back(static_cast<uint16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            if (cp > 0x10FFFF) cp = 0xFFFD;
            out.push_back(static_cast<uint16_t>(cp));
        }
        i += static_cast<size_t>(len);
    }
    return out;
}

// The value string the control's selection offsets index — must match
// js_element_get_value for the editable types: <input> reads the "value"
// attribute (which the typing/IME pipeline keeps live, preedit included);
// <textarea> reads the attribute once any edit happened, else textContent.
static std::string selectionValueOf(bro::dom::Element* el) {
    const std::string& tag = el->tagName();
    if (tag == "TEXTAREA" || tag == "textarea") {
        if (el->hasAttribute("value")) return el->getAttribute("value");
        return el->textContent();
    }
    return el->getAttribute("value");
}

// Selection API: delegates to ElInput. Non-text inputs return null / throw
// per spec — bro returns safe defaults instead of throwing (matches our
// overall "prefer no-op to exception" style for IDL edge cases).
static JSValue js_element_get_selectionStart(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NULL;
    if (auto* inp = el->inputControl())
        return JS_NewInt32(ctx, utf8ByteToUtf16(selectionValueOf(el), inp->selectionStart()));
    if (auto* ta = el->textareaControl())
        return JS_NewInt32(ctx, utf8ByteToUtf16(selectionValueOf(el), ta->selectionStart()));
    return JS_NULL;
}
static JSValue js_element_set_selectionStart(JSContext* ctx, JSValueConst this_val, JSValueConst v) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    int32_t n = 0; JS_ToInt32(ctx, &n, v);
    if (auto* inp = el->inputControl()) {
        const int b = utf16ToUtf8Byte(selectionValueOf(el), n);
        inp->setSelectionRange(b, inp->selectionEnd() < b ? b : inp->selectionEnd());
    } else if (auto* ta = el->textareaControl()) {
        const int b = utf16ToUtf8Byte(selectionValueOf(el), n);
        ta->setSelectionRange(b, ta->selectionEnd() < b ? b : ta->selectionEnd());
    }
    return JS_UNDEFINED;
}
static JSValue js_element_get_selectionEnd(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NULL;
    if (auto* inp = el->inputControl())
        return JS_NewInt32(ctx, utf8ByteToUtf16(selectionValueOf(el), inp->selectionEnd()));
    if (auto* ta = el->textareaControl())
        return JS_NewInt32(ctx, utf8ByteToUtf16(selectionValueOf(el), ta->selectionEnd()));
    return JS_NULL;
}
static JSValue js_element_set_selectionEnd(JSContext* ctx, JSValueConst this_val, JSValueConst v) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    int32_t n = 0; JS_ToInt32(ctx, &n, v);
    if (auto* inp = el->inputControl()) {
        inp->setSelectionRange(inp->selectionStart(), utf16ToUtf8Byte(selectionValueOf(el), n));
    } else if (auto* ta = el->textareaControl()) {
        ta->setSelectionRange(ta->selectionStart(), utf16ToUtf8Byte(selectionValueOf(el), n));
    }
    return JS_UNDEFINED;
}
static JSValue js_element_setSelectionRange(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el || argc < 2) return JS_UNDEFINED;
    int32_t start = 0, end = 0;
    JS_ToInt32(ctx, &start, argv[0]);
    JS_ToInt32(ctx, &end, argv[1]);
    const std::string val = selectionValueOf(el);
    const int bs = utf16ToUtf8Byte(val, start);
    const int be = utf16ToUtf8Byte(val, end);
    if (auto* inp = el->inputControl()) inp->setSelectionRange(bs, be);
    else if (auto* ta = el->textareaControl()) ta->setSelectionRange(bs, be);
    return JS_UNDEFINED;
}
static JSValue js_element_select(JSContext* ctx, JSValueConst this_val,
                                 int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    if (auto* inp = el->inputControl()) inp->selectAll();
    else if (auto* ta = el->textareaControl()) ta->selectAll();
    return JS_UNDEFINED;
}

// ---- Constraint validation -----------------------------------------------

namespace {
struct ValidityReport {
    bool valueMissing = false;
    bool typeMismatch = false;
    bool patternMismatch = false;
    bool tooShort = false;
    bool tooLong = false;
    bool rangeUnderflow = false;
    bool rangeOverflow = false;
    bool stepMismatch = false;
    bool badInput = false;
    bool customError = false;
    bool valid() const {
        return !(valueMissing || typeMismatch || patternMismatch || tooShort ||
                 tooLong || rangeUnderflow || rangeOverflow || stepMismatch ||
                 badInput || customError);
    }
};

// Candidate for constraint validation — roughly HTML's "willValidate":
// form-associated controls that are not disabled / readonly / type=hidden/
// button/submit/reset/image, and not inside a <datalist>.
bool willValidate(bro::dom::Element* el) {
    if (!el) return false;
    const std::string& tag = el->tagName();
    const bool isInput = (tag == "INPUT" || tag == "input");
    const bool isTextarea = (tag == "TEXTAREA" || tag == "textarea");
    const bool isSelect = (tag == "SELECT" || tag == "select");
    if (!isInput && !isTextarea && !isSelect) return false;
    if (el->hasAttribute("disabled") || el->hasAttribute("readonly")) return false;
    if (isInput) {
        const std::string& t = el->getAttribute("type");
        if (t == "hidden" || t == "button" || t == "submit" ||
            t == "reset"  || t == "image") return false;
    }
    return true;
}

std::string controlValue(bro::dom::Element* el) {
    if (!el) return "";
    const std::string& tag = el->tagName();
    if (tag == "TEXTAREA" || tag == "textarea") {
        // Textarea stores live value as textContent; a value attribute on
        // textarea is non-spec and ignored by real browsers.
        return el->textContent();
    }
    if (tag == "SELECT" || tag == "select") {
        if (auto* sel = el->selectControl()) {
            auto opts = sel->getOptions();
            int idx = sel->selectedIndex();
            if (idx >= 0 && idx < static_cast<int>(opts.size()))
                return opts[idx].value;
        }
        return "";
    }
    return el->getAttribute("value");
}

ValidityReport computeValidity(JSContext* ctx, bro::dom::Element* el) {
    ValidityReport r;
    if (!el) return r;
    if (!el->customValidity().empty()) r.customError = true;
    if (!willValidate(el)) return r;

    const std::string tag = el->tagName();
    const std::string type = (tag == "INPUT" || tag == "input")
                             ? el->getAttribute("type") : "";
    const std::string value = controlValue(el);
    const bool empty = value.empty();

    // valueMissing
    if (el->hasAttribute("required")) {
        if (type == "checkbox") {
            if (!el->hasAttribute("checked")) r.valueMissing = true;
        } else if (type == "radio") {
            // For radios: any radio in the group with the same name must be checked.
            // Scope: same form (or document if no form).
            bool anyChecked = false;
            const std::string& name = el->getAttribute("name");
            // Search from the form ancestor or document root.
            bro::dom::Element* root = nullptr;
            for (auto* p = el->parentElement(); p; p = p->parentElement()) {
                if (p->tagName() == "FORM" || p->tagName() == "form") { root = p; break; }
            }
            std::function<void(bro::dom::Element*)> walk = [&](bro::dom::Element* e){
                if (!e || anyChecked) return;
                if ((e->tagName() == "INPUT" || e->tagName() == "input") &&
                    e->getAttribute("type") == "radio" &&
                    e->getAttribute("name") == name &&
                    e->hasAttribute("checked")) { anyChecked = true; return; }
                for (auto* c : e->children()) walk(c);
            };
            if (root) walk(root);
            else if (el->document() && el->document()->documentElement()) {
                walk(el->document()->documentElement());
            }
            if (!anyChecked) r.valueMissing = true;
        } else if (empty) {
            r.valueMissing = true;
        }
    }

    // typeMismatch / badInput — only when value is non-empty
    if (!empty) {
        if (type == "email") {
            // Minimal: one '@' with something on each side and a '.' in domain.
            auto at = value.find('@');
            if (at == std::string::npos || at == 0 || at == value.size() - 1 ||
                value.find('.', at) == std::string::npos) {
                r.typeMismatch = true;
            }
        } else if (type == "url") {
            // Minimal: has a scheme "xx:".
            auto colon = value.find(':');
            if (colon == std::string::npos || colon < 2) r.typeMismatch = true;
        } else if (type == "number" || type == "range") {
            try {
                size_t n = 0;
                (void)std::stod(value, &n);
                if (n != value.size()) r.badInput = true;
            } catch (...) { r.badInput = true; }
        }
    }

    // patternMismatch — applies to text-like inputs when non-empty. Per spec
    // the pattern is an ECMAScript RegExp (current spec says the 'v' flag; we
    // compile with 'u', the spec's sanctioned approximation) implicitly
    // anchored as ^(?:pattern)$, so it uses QuickJS's own regexp engine
    // (libregexp — the dialect app JS actually speaks: named groups,
    // lookbehind, \u{...}), not std::regex. 'u' rather than 'v' because this
    // libregexp only combines surrogate pairs at exec under LRE_FLAG_UNICODE —
    // v-mode astral subjects would mis-match. Matching runs over the UTF-16
    // form of the value, per JS string semantics. An invalid pattern is
    // ignored (the constraint matches everything).
    if (ctx && !empty && el->hasAttribute("pattern")) {
        const std::string source = "^(?:" + el->getAttribute("pattern") + ")$";
        char errorMsg[64];
        int bcLen = 0;
        uint8_t* bc = lre_compile(&bcLen, errorMsg, sizeof errorMsg,
                                  source.c_str(), source.size(),
                                  LRE_FLAG_UNICODE, ctx);
        if (bc) {
            const std::vector<uint16_t> subject = utf8ToUtf16Units(value);
            const int captureCount = lre_get_capture_count(bc);
            std::vector<uint8_t*> capture(static_cast<size_t>(captureCount) * 2);
            const int rc = lre_exec(capture.data(), bc,
                                    reinterpret_cast<const uint8_t*>(subject.data()),
                                    /*cindex=*/0, static_cast<int>(subject.size()),
                                    /*cbuf_type=*/1, ctx);
            // rc: 1 = match, 0 = no match, <0 = engine error (treat an engine
            // error like an invalid pattern: constraint ignored).
            if (rc == 0) r.patternMismatch = true;
            js_free(ctx, bc);
        }
    }

    // minLength/maxLength — code-unit count on value.
    if (!empty && el->hasAttribute("minlength")) {
        try {
            int min = std::stoi(el->getAttribute("minlength"));
            if (min > 0 && static_cast<int>(value.size()) < min) r.tooShort = true;
        } catch (...) {}
    }
    if (el->hasAttribute("maxlength")) {
        try {
            int max = std::stoi(el->getAttribute("maxlength"));
            if (max >= 0 && static_cast<int>(value.size()) > max) r.tooLong = true;
        } catch (...) {}
    }

    // range underflow/overflow/stepMismatch — number / range only.
    if (!r.badInput && !empty && (type == "number" || type == "range")) {
        double num = 0.0;
        try { num = std::stod(value); } catch (...) { num = 0.0; }
        if (el->hasAttribute("min")) {
            try { if (num < std::stod(el->getAttribute("min"))) r.rangeUnderflow = true; }
            catch (...) {}
        }
        if (el->hasAttribute("max")) {
            try { if (num > std::stod(el->getAttribute("max"))) r.rangeOverflow = true; }
            catch (...) {}
        }
        if (el->hasAttribute("step")) {
            const std::string& s = el->getAttribute("step");
            if (s != "any") {
                try {
                    double step = std::stod(s);
                    double base = el->hasAttribute("min") ? std::stod(el->getAttribute("min")) : 0.0;
                    double off = num - base;
                    // Fuzzy modulo to avoid fp noise.
                    double rem = off - std::round(off / step) * step;
                    if (step > 0 && std::fabs(rem) > 1e-9) r.stepMismatch = true;
                } catch (...) {}
            }
        }
    }

    return r;
}

std::string defaultValidationMessage(const ValidityReport& r,
                                     bro::dom::Element* el) {
    if (r.customError) return el->customValidity();
    if (r.valueMissing)    return "Please fill out this field.";
    if (r.typeMismatch)    return "Please enter a value of the correct type.";
    if (r.patternMismatch) return "Please match the requested format.";
    if (r.tooShort)        return "Please lengthen this text.";
    if (r.tooLong)         return "Please shorten this text.";
    if (r.rangeUnderflow)  return "Value is below the minimum.";
    if (r.rangeOverflow)   return "Value is above the maximum.";
    if (r.stepMismatch)    return "Please select a valid value.";
    if (r.badInput)        return "Please enter a valid value.";
    return "";
}
} // namespace

static JSValue js_element_get_validity(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    ValidityReport r = computeValidity(ctx, el);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "valueMissing",   JS_NewBool(ctx, r.valueMissing));
    JS_SetPropertyStr(ctx, o, "typeMismatch",   JS_NewBool(ctx, r.typeMismatch));
    JS_SetPropertyStr(ctx, o, "patternMismatch",JS_NewBool(ctx, r.patternMismatch));
    JS_SetPropertyStr(ctx, o, "tooShort",       JS_NewBool(ctx, r.tooShort));
    JS_SetPropertyStr(ctx, o, "tooLong",        JS_NewBool(ctx, r.tooLong));
    JS_SetPropertyStr(ctx, o, "rangeUnderflow", JS_NewBool(ctx, r.rangeUnderflow));
    JS_SetPropertyStr(ctx, o, "rangeOverflow",  JS_NewBool(ctx, r.rangeOverflow));
    JS_SetPropertyStr(ctx, o, "stepMismatch",   JS_NewBool(ctx, r.stepMismatch));
    JS_SetPropertyStr(ctx, o, "badInput",       JS_NewBool(ctx, r.badInput));
    JS_SetPropertyStr(ctx, o, "customError",    JS_NewBool(ctx, r.customError));
    JS_SetPropertyStr(ctx, o, "valid",          JS_NewBool(ctx, r.valid()));
    return o;
}

static JSValue js_element_get_validationMessage(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewString(ctx, "");
    ValidityReport r = computeValidity(ctx, el);
    if (r.valid()) return JS_NewString(ctx, "");
    return JS_NewString(ctx, defaultValidationMessage(r, el).c_str());
}

static JSValue js_element_get_willValidate(JSContext* ctx, JSValueConst this_val) {
    return JS_NewBool(ctx, willValidate(getElement(this_val)));
}

// Forward declaration so the element-level checkValidity can delegate when
// called on a <form>.
static JSValue js_form_checkValidity_impl(JSContext* ctx, bro::dom::Element* el);

static JSValue js_element_checkValidity(JSContext* ctx, JSValueConst this_val,
                                        int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_TRUE;
    if (el->tagName() == "FORM" || el->tagName() == "form") {
        return js_form_checkValidity_impl(ctx, el);
    }
    ValidityReport r = computeValidity(ctx, el);
    if (r.valid()) return JS_TRUE;
    // Fire cancelable 'invalid' event per spec.
    bro::dom::Event evt("invalid", false, true);
    evt.setIsTrusted(true);
    dispatchDomEvent(ctx, el, evt);
    return JS_FALSE;
}

static JSValue js_element_reportValidity(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    // Same as checkValidity for now — no native bubble UI. Keeping the method
    // separate so future UI can hook in without breaking callers that rely on
    // checkValidity's quieter semantics.
    return js_element_checkValidity(ctx, this_val, argc, argv);
}

static JSValue js_element_setCustomValidity(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el || argc < 1) return JS_UNDEFINED;
    el->setCustomValidity(jsToStdString(ctx, argv[0]));
    return JS_UNDEFINED;
}

// ---- HTMLFormElement ------------------------------------------------------

namespace {
// Collect form-associated controls for the given form. Controls associated
// via ancestor: the form is their nearest <form> ancestor. Controls
// associated via form="id" attribute: the attribute matches this form's id.
// Returns owners in document order (best-effort via DFS).
void collectFormElements(bro::dom::Element* form,
                         std::vector<bro::dom::Element*>& out) {
    if (!form || !form->document()) return;
    const std::string formId = form->getAttribute("id");

    auto isControl = [](const std::string& tag) {
        return tag == "INPUT" || tag == "input" ||
               tag == "SELECT" || tag == "select" ||
               tag == "TEXTAREA" || tag == "textarea" ||
               tag == "BUTTON" || tag == "button" ||
               tag == "FIELDSET" || tag == "fieldset" ||
               tag == "OBJECT" || tag == "object" ||
               tag == "OUTPUT" || tag == "output";
    };

    std::function<void(bro::dom::Element*)> walk = [&](bro::dom::Element* e) {
        if (!e) return;
        if (isControl(e->tagName())) {
            // Which form owns this control?
            const std::string& ownerAttr = e->getAttribute("form");
            bool owned = false;
            if (!ownerAttr.empty()) {
                if (ownerAttr == formId) owned = true;
            } else {
                for (auto* p = e->parentElement(); p; p = p->parentElement()) {
                    if (p == form) { owned = true; break; }
                    if (p->tagName() == "FORM" || p->tagName() == "form") break;
                }
            }
            if (owned) out.push_back(e);
        }
        for (auto* c : e->children()) walk(c);
    };
    if (auto* root = form->document()->documentElement()) walk(root);
}
} // namespace

static JSValue js_form_get_elements(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NULL;
    std::vector<bro::dom::Element*> items;
    collectFormElements(el, items);
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < items.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i),
                             DomBindings::wrapElement(ctx, items[i]));
    }
    JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, static_cast<int>(items.size())));
    // Named access: form.elements["myname"] returns the matching control.
    // A full HTMLFormControlsCollection is a Proxy — snapshot with properties
    // works for the common case.
    for (auto* c : items) {
        const std::string& n = c->getAttribute("name");
        if (!n.empty()) {
            // Only set if not already present (first control wins for name lookup).
            JSValue existing = JS_GetPropertyStr(ctx, arr, n.c_str());
            if (JS_IsUndefined(existing)) {
                JS_SetPropertyStr(ctx, arr, n.c_str(),
                                  DomBindings::wrapElement(ctx, c));
            }
            JS_FreeValue(ctx, existing);
        }
    }
    return arr;
}

static JSValue js_form_get_length(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NewInt32(ctx, 0);
    std::vector<bro::dom::Element*> items;
    collectFormElements(el, items);
    return JS_NewInt32(ctx, static_cast<int>(items.size()));
}

static JSValue js_form_submit(JSContext* ctx, JSValueConst this_val,
                              int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    // .submit() skips constraint validation and skips the submit event per
    // spec. bro has no native navigation; apps handle submission in script.
    // We intentionally do nothing else — tests that want a notification
    // should use requestSubmit() instead.
    (void)el;
    return JS_UNDEFINED;
}

void requestFormSubmit(JSContext* ctx, bro::dom::Element* form,
                       bro::dom::Element* submitter) {
    if (!form) return;
    std::vector<bro::dom::Element*> items;
    collectFormElements(form, items);
    bool anyInvalid = false;
    for (auto* c : items) {
        ValidityReport r = computeValidity(ctx, c);
        if (!r.valid()) {
            anyInvalid = true;
            bro::dom::Event invalidEvt("invalid", false, true);
            invalidEvt.setIsTrusted(true);
            dispatchDomEvent(ctx, c, invalidEvt);
        }
    }
    if (anyInvalid) return;

    bro::dom::SubmitEvent evt("submit", true, true);
    evt.setIsTrusted(true);
    evt.setSubmitter(submitter);
    dispatchDomEvent(ctx, form, evt);
    // If not cancelled, bro has no default navigation — the app owns it.
}

static JSValue js_form_requestSubmit(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    bro::dom::Element* submitter = nullptr;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        submitter = getElement(argv[0]);
    }
    requestFormSubmit(ctx, el, submitter);
    return JS_UNDEFINED;
}

static JSValue js_form_reset(JSContext* ctx, JSValueConst this_val,
                             int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;

    bro::dom::Event resetEvt("reset", true, true);
    resetEvt.setIsTrusted(true);
    dispatchDomEvent(ctx, el, resetEvt);
    if (resetEvt.defaultPrevented()) return JS_UNDEFINED;

    std::vector<bro::dom::Element*> items;
    collectFormElements(el, items);
    for (auto* c : items) {
        const std::string& tag = c->tagName();
        if (tag == "INPUT" || tag == "input") {
            const std::string& t = c->getAttribute("type");
            if (t == "checkbox" || t == "radio") {
                // Restore the `checked` attribute to its defaultChecked.
                if (c->hasAttribute("data-default-checked")) {
                    c->setAttribute("checked", "");
                } else if (c->hasAttribute("_default_checked")) {
                    c->setAttribute("checked", "");
                } else {
                    // defaultChecked tracks the parsed 'checked' attribute;
                    // bro doesn't separately store it, so we preserve the
                    // current attribute state (no change on reset for now).
                }
            } else {
                // Restore value to defaultValue (parsed 'value' attribute).
                // bro stores current value in the 'value' attribute and has
                // no separate defaultValue slot, so for now reset is a no-op
                // on non-checkable inputs. Document the limitation and ship.
                (void)c;
            }
        } else if (tag == "TEXTAREA" || tag == "textarea") {
            // Similar limitation: no stored defaultValue.
        } else if (tag == "SELECT" || tag == "select") {
            // Restore to the option with the `selected` attribute (or index 0
            // if none). ElSelect itself doesn't track a separate default, so
            // we read the DOM directly.
            int def = 0, idx = 0, found = -1;
            for (auto* child : c->children()) {
                if (child->tagName() != "OPTION" && child->tagName() != "option") continue;
                if (child->hasAttribute("selected") && found < 0) found = idx;
                ++idx;
            }
            def = (found >= 0) ? found : 0;
            if (auto* sel = c->selectControl()) sel->setSelectedIndex(def);
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_form_checkValidity_impl(JSContext* ctx, bro::dom::Element* el) {
    if (!el) return JS_TRUE;
    std::vector<bro::dom::Element*> items;
    collectFormElements(el, items);
    bool allValid = true;
    for (auto* c : items) {
        ValidityReport r = computeValidity(ctx, c);
        if (!r.valid()) {
            allValid = false;
            bro::dom::Event evt("invalid", false, true);
            evt.setIsTrusted(true);
            dispatchDomEvent(ctx, c, evt);
        }
    }
    return JS_NewBool(ctx, allValid);
}

// files: FileList for <input type=file>. bro has no native file picker in
// headless and no engine-driven file drop has been wired to <input> yet, so
// this always returns null (spec: null for non-file inputs) or an empty
// array-like for type=file. Extending to a live FileList when file drop
// wiring lands is a focused follow-up.
static JSValue js_element_get_files(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_NULL;
    const std::string& t = el->getAttribute("type");
    if (t != "file") return JS_NULL;
    // Empty FileList — array with length=0, array-like indexing returns undefined.
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, 0));
    return arr;
}



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
    // VIDEO src change: trigger load through the controller if attached.
    // If the control isn't present yet, ensureReplacedElements picks the
    // src attribute up when it runs, so the value-stored-on-attribute
    // path alone suffices in that case.
    if (name == "src" && (el->tagName() == "VIDEO" || el->tagName() == "video")) {
        if (auto* v = el->videoControl()) {
            if (!newVal.empty()) v->load(newVal);
        }
    }
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

// A DOM mutation belongs to the element whose child list moved. Attributing it
// lets the layout tree rebuild just that node's children from the DOM and hand
// back every other subtree's cached geometry — the document-wide mark throws the
// whole tree away, which on a few-thousand-element document is ~150ms of layout
// for one appended chip. Falls back to the document-wide form only when there is
// no element to pin the change on.
static void markChildListChanged(bro::dom::Document* doc, bro::dom::Node* parent) {
    if (parent && parent->nodeType() == bro::dom::NodeType::Element)
        static_cast<bro::dom::Element*>(parent)->markStructureDirty();
    else if (doc)
        doc->markStructureDirty();
}

// DOM "pre-insert" step 2: adopt `node` into `parent`'s document when their
// owner documents differ. Without this, a node built in a DOMParser document
// and appended into the live tree stayed owned by the parser document — it
// rendered until that document was collected, then its holder destroyed a node
// still sitting in the live tree. Spec says pre-insertion adopts; so do we.
static void adoptIntoParentDocument(bro::dom::Node* parent, bro::dom::Node* node)
{
    if (!parent || !node) return;
    auto* target = parent->document();
    if (!target || node->document() == target) return;
    // adoptNode detaches from the old parent, which is also what insertion
    // would do — the caller's insertBefore/appendChild then re-parents here.
    target->adoptNode(node);
}

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
            // Adopt each child individually: the fragment itself is discarded,
            // only its children enter the tree.
            for (auto* kid : kids) adoptIntoParentDocument(el, kid);
            // Build addedNodes array for MutationObserver
            JSValue addedArr = JS_NewArray(ctx);
            uint32_t addedIdx = 0;
            for (auto* kid : kids) {
                el->appendChild(kid);
                markChildListChanged(doc, el);
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
            adoptIntoParentDocument(el, child);
            el->appendChild(child);
            markChildListChanged(doc, el);
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
        }
        markChildListChanged(doc, el);
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
        adoptIntoParentDocument(el, newChild);
        el->insertBefore(newChild, refChild);
        auto* doc = getDocumentForCtx(ctx);
        markChildListChanged(doc, el);
        if (newChild->nodeType() == bro::dom::NodeType::Element) {
            auto* newElem = static_cast<bro::dom::Element*>(newChild);
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
        adoptIntoParentDocument(el, newChild);
        el->insertBefore(newChild, oldChild);
        markChildListChanged(doc, el);
        if (oldChild->nodeType() == bro::dom::NodeType::Element) {
            auto* oldElem = static_cast<bro::dom::Element*>(oldChild);
            if (doc && !oldElem->id().empty())
                doc->unregisterElementId(oldElem->id());
        }
        el->removeChild(oldChild);
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
    // "style" lives in StyleProxy, not attributes_ (see Element::setAttribute).
    if (el->hasAttribute("style")) {
        clone->setAttribute("style", el->style().cssText());
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
        markChildListChanged(doc, parent);
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
        // NB: JS_SetProperty does NOT take ownership of the atom — cf.
        // JS_SetPropertyStr, which interns one and frees it right back. Duping
        // `key` into it leaked a ref, once per element the first time it got a
        // listener, which is what kept "__bro_listeners" in the runtime's atom
        // table at shutdown. It DOES take the value ref, hence the JS_DupValue.
        JS_SetProperty(ctx, this_val, key, JS_DupValue(ctx, arr));
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
    // Detached (DOMParser) nodes answer with THEIR Document wrapper, not the
    // realm's global document.
    JSValue detached = detachedDocumentWrapper(ctx, el->document());
    if (!JS_IsNull(detached)) return detached;
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
    if (type != "2d" && type != "webgl" && type != "webgl2" && type != "scene") return JS_NULL;

    // Return cached context if one already exists for this element. Per the
    // HTML spec getContext must return the SAME context object for the life of
    // the canvas — including canvases that were never inserted into the DOM
    // (off-screen scratch canvases for decode/readback are idiomatic; handing
    // back a fresh context per call silently makes draw-then-getImageData read
    // a blank surface).
    auto cacheIt = s_canvas_contexts.find(el);
    if (cacheIt != s_canvas_contexts.end()) {
        // Staleness: a freed canvas Element*'s address can be reused by a
        // brand-new canvas before this entry is evicted — eviction is tied to
        // GC of the old wrapper (js_element_finalizer), but the engine frees
        // the Element — and its CanvasScene — deterministically at
        // drainPendingFrees, much earlier. Returning the cached wrapper would
        // hand back a CanvasScene freed with the old element — a
        // use-after-free on the next draw. A live context always leaves its
        // backing on the element (setCanvasScene for 2d/scene, setWebglContext
        // for webgl) from the moment of creation, DOM-connected or not, and
        // the backing is cleared when the element is destroyed — so a cached
        // entry with no live backing is the one-and-only stale case.
        // (Deliberately NOT a DOM-connectedness check: off-DOM canvases keep
        // their context.)
        bool hasBacking = (el->canvasScene() != nullptr) || (el->webglContext() != nullptr);
        if (!hasBacking) {
            // Stale entry — the element's address was reused by a new canvas
            // whose own context hasn't been created yet.
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
    // HTML5 canvas: setting width is the bitmap size, not just an attribute.
    // Push it straight to the CanvasScene so the surface resizes/clears
    // synchronously rather than waiting on the threaded layout pipeline —
    // otherwise draw commands recorded after this can race the layout
    // update and land on a surface still sized for the previous content.
    if (auto* cs = static_cast<bro::canvas::CanvasScene*>(el->canvasScene())) {
        cs->setIntrinsicWidth(w);
        cs->reset();
    }
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
    if (auto* cs = static_cast<bro::canvas::CanvasScene*>(el->canvasScene())) {
        cs->setIntrinsicHeight(h);
        cs->reset();
    }
    return JS_UNDEFINED;
}

// ---- Layout measurement ---------------------------------------------------

static const htmlayout::layout::LayoutBox& getLayoutBox(bro::dom::Element* el) {
    static const htmlayout::layout::LayoutBox empty{};
    if (!el) return empty;
    return el->layoutBox();
}

// Per CSSOM spec, inline non-replaced elements return 0 for client/scroll dimensions.
static bool isInlineDisplay(bro::dom::Element* el) {
    if (!el) return false;
    auto& style = el->computedStyle();
    auto it = style.find("display");
    return it != style.end() && it->second == "inline";
}

static JSValue js_element_get_clientWidth(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (isInlineDisplay(el)) return JS_NewInt32(ctx, 0);
    auto& box = getLayoutBox(el);
    float cw = box.contentRect.width + box.padding.left + box.padding.right;
    if (cw > 0) return JS_NewInt32(ctx, static_cast<int>(cw));
    return js_element_get_width(ctx, this_val);
}

static JSValue js_element_get_clientHeight(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (isInlineDisplay(el)) return JS_NewInt32(ctx, 0);
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

static JSValue js_element_get_clientTop(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (isInlineDisplay(el)) return JS_NewInt32(ctx, 0);
    auto& box = getLayoutBox(el);
    return JS_NewInt32(ctx, static_cast<int>(box.border.top));
}

static JSValue js_element_get_clientLeft(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (isInlineDisplay(el)) return JS_NewInt32(ctx, 0);
    auto& box = getLayoutBox(el);
    return JS_NewInt32(ctx, static_cast<int>(box.border.left));
}

static JSValue js_element_get_hidden(JSContext* ctx, JSValueConst this_val) {
    auto* el = getElement(this_val);
    if (!el) return JS_FALSE;
    return JS_NewBool(ctx, el->hasAttribute("hidden"));
}

static JSValue js_element_set_hidden(JSContext* ctx, JSValueConst this_val,
                                     JSValueConst val) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    if (JS_ToBool(ctx, val))
        el->setAttribute("hidden", "");
    else
        el->removeAttribute("hidden");
    return JS_UNDEFINED;
}

static JSValue js_element_get_scrollWidth(JSContext* ctx, JSValueConst this_val) {
    // Per spec: scrollWidth = max(clientWidth, scrollable content width including padding)
    // Inline non-replaced elements return 0.
    auto* el = getElement(this_val);
    if (isInlineDisplay(el)) return JS_NewInt32(ctx, 0);
    auto& box = getLayoutBox(el);
    float clientW = box.contentRect.width + box.padding.left + box.padding.right;
    if (clientW > 0) return JS_NewInt32(ctx, static_cast<int>(clientW));
    return js_element_get_offsetWidth(ctx, this_val);
}

static JSValue js_element_get_scrollHeight(JSContext* ctx, JSValueConst this_val) {
    // Per spec: scrollHeight = max(clientHeight, scrollable content height including padding)
    // Inline non-replaced elements return 0.
    auto* el = getElement(this_val);
    if (!el) return JS_NewInt32(ctx, 0);
    if (isInlineDisplay(el)) return JS_NewInt32(ctx, 0);
    auto& box = el->layoutBox();
    float clientH = box.contentRect.height + box.padding.top + box.padding.bottom;
    // naturalHeight is the unclamped content height (before min/max-height).
    // scrollHeight includes padding around that content.
    float scrollH = box.naturalHeight + box.padding.top + box.padding.bottom;
    float h = scrollH > clientH ? scrollH : clientH;
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
    // Clamp to [0, maxScroll] per spec
    auto& box = el->layoutBox();
    float maxScroll = std::max(0.0f, box.naturalHeight - box.contentRect.height);
    float clamped = std::clamp(static_cast<float>(v), 0.0f, maxScroll);
    float prev = el->scrollTopValue();
    el->setScrollTopValue(clamped);
    // Layout is async: when JS appends content and then does the classic
    // `el.scrollTop = el.scrollHeight` in the SAME turn, the just-appended nodes
    // aren't laid out yet, so both scrollHeight and maxScroll above are STALE and
    // the clamp lands short of the real bottom — the container never follows the
    // new content until a second scroll (a log/transcript that "stops updating").
    // If the request is at or beyond the current (stale) max and a layout is
    // still pending, defer to the post-layout scroll-to-bottom pass so it snaps
    // to the true bottom once the append is laid out. A definite mid-position
    // (v < maxScroll) cancels any such pending intent.
    if (el->document()) {
        // "Wants the end" = a positive request at or beyond the current max. That
        // excludes scrollTop = 0 (scroll to top), which must never be read as a
        // scroll-to-bottom even when the element isn't scrollable yet (max == 0).
        bool wantsEnd = static_cast<float>(v) > 0.0f &&
                        static_cast<float>(v) >= maxScroll;
        if (wantsEnd && el->document()->isDirty())
            el->setScrollToBottom(true);   // defer to the true bottom post-layout
        else
            el->setScrollToBottom(false);  // explicit position: cancel any pending jump
        el->document()->markDirty();
    }
    if (static_cast<float>(v) != prev) {
        bro::dom::Event evt("scroll", false, false);
        evt.setIsTrusted(true);
        dispatchDomEvent(ctx, el, evt);
    }
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
            // Flow-collapsed content (closed <details> body) is not rendered,
            // so it doesn't contribute to innerText (matching Chromium).
            auto fcIt = style.find("-x-flow-collapse");
            if (fcIt != style.end() && fcIt->second == "collapse") continue;
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

    // display:none elements have no box at all, and a null element has no
    // ancestor chain to walk — bro::dom::absoluteBorderBox() returns
    // {0,0,0,0} for both, matching Chromium rather than inheriting the
    // parent's accumulated position.
    //
    // Elements inside an <svg> subtree have no layout boxes (the svg is a
    // replaced element); their rect is computed from the SVG geometry
    // itself — shape fill bounds through the transform/viewBox chain, or
    // all-zeros for non-rendered elements (defs, gradients, stops, ...).
    // SVG <text>/<tspan> bounds need font measurement; hand the binding the
    // engine's renderer (the same backend used to paint the text).
    bro::render::Renderer* rr = nullptr;
    if (auto it = s_ctx_engines.find(ctx); it != s_ctx_engines.end() && it->second)
        rr = static_cast<bro::engine::Engine*>(it->second)->renderer();
    bro::dom::AbsoluteRect r;
    if (!bro::layout::svgChildBoundingClientRect(el, r, rr))
        r = bro::dom::absoluteBorderBox(el);

    JSValue rect = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rect, "x",      JS_NewFloat64(ctx, r.x));
    JS_SetPropertyStr(ctx, rect, "y",      JS_NewFloat64(ctx, r.y));
    JS_SetPropertyStr(ctx, rect, "width",  JS_NewFloat64(ctx, r.width));
    JS_SetPropertyStr(ctx, rect, "height", JS_NewFloat64(ctx, r.height));
    JS_SetPropertyStr(ctx, rect, "top",    JS_NewFloat64(ctx, r.y));
    JS_SetPropertyStr(ctx, rect, "left",   JS_NewFloat64(ctx, r.x));
    JS_SetPropertyStr(ctx, rect, "bottom", JS_NewFloat64(ctx, r.y + r.height));
    JS_SetPropertyStr(ctx, rect, "right",  JS_NewFloat64(ctx, r.x + r.width));
    return rect;
}

static JSValue js_element_getClientRects(JSContext* ctx, JSValueConst this_val,
                                         int /*argc*/, JSValueConst* /*argv*/) {
    JSValue rect = js_element_getBoundingClientRect(ctx, this_val, 0, nullptr);
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, rect);
    return arr;
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

    // Default actions — button form submit/reset and checkbox/radio
    // activation (toggle + change/input events). Mirrors the hit-tested
    // click path in replaced_elements.cpp so scripted clicks via
    // element.click() exercise the same behavior.
    if (!event.defaultPrevented()) {
        const auto& tag = el->tagName();
        const bool isButton = (tag == "BUTTON" || tag == "button");
        const bool isInput = (tag == "INPUT" || tag == "input");
        const std::string inputType = isInput ? el->getAttribute("type") : "";
        const bool isActionInput =
            isInput && (inputType == "submit" || inputType == "reset" || inputType == "image");
        if (isButton || isActionInput) {
            std::string btnType = el->getAttribute("type");
            if (btnType.empty() && isButton) btnType = "submit";
            if (btnType == "image") btnType = "submit"; // image = implicit submit
            if (btnType == "submit") {
                bro::dom::Element* owner = nullptr;
                const std::string& attrForm = el->getAttribute("form");
                if (!attrForm.empty() && el->document()) {
                    auto* o = el->document()->getElementById(attrForm);
                    if (o && (o->tagName() == "FORM" || o->tagName() == "form")) owner = o;
                } else {
                    for (auto* p = el->parentElement(); p; p = p->parentElement()) {
                        if (p->tagName() == "FORM" || p->tagName() == "form") { owner = p; break; }
                    }
                }
                if (owner) requestFormSubmit(ctx, owner, el);
            } else if (btnType == "reset") {
                for (auto* p = el->parentElement(); p; p = p->parentElement()) {
                    if (p->tagName() == "FORM" || p->tagName() == "form") {
                        bro::dom::Event resetEvt("reset", true, true);
                        resetEvt.setIsTrusted(true);
                        dispatchDomEvent(ctx, p, resetEvt);
                        break;
                    }
                }
            }
        } else if (isInput && (inputType == "checkbox" || inputType == "radio")) {
            if (inputType == "checkbox") {
                if (el->hasAttribute("checked")) el->removeAttribute("checked");
                else el->setAttribute("checked", "");
            } else {
                // Radio: checking one unchecks the rest of its name group.
                const std::string nameStr = el->getAttribute("name");
                if (!nameStr.empty() && el->document() && el->document()->body()) {
                    auto radios =
                        el->document()->body()->querySelectorAll("input[type=\"radio\"]");
                    for (auto* other : radios) {
                        if (other != el && other->getAttribute("name") == nameStr)
                            other->removeAttribute("checked");
                    }
                }
                el->setAttribute("checked", "");
            }
            dom::Event changeEvt("change");
            dispatchDomEvent(ctx, el, changeEvt);
            dom::InputEvent inputEvt("input");
            inputEvt.setIsTrusted(true);
            dispatchDomEvent(ctx, el, inputEvt);
        } else {
            // <summary> activation: toggle [open] on the parent <details>.
            // Walk up from the clicked element so a click on content inside
            // the summary counts too; only the first <summary> child is the
            // disclosure handle (HTML spec). Mirrors replaced_elements.cpp.
            for (auto* s = el; s; s = s->parentElement()) {
                const auto& stag = s->tagName();
                if (stag != "SUMMARY" && stag != "summary") continue;
                auto* parent = s->parentElement();
                if (!parent) break;
                const auto& ptag = parent->tagName();
                if (ptag != "DETAILS" && ptag != "details") break;
                dom::Element* firstSummary = nullptr;
                for (auto* c : parent->children()) {
                    const auto& ct = c->tagName();
                    if (ct == "SUMMARY" || ct == "summary") { firstSummary = c; break; }
                }
                if (firstSummary != s) break;
                if (parent->hasAttribute("open")) parent->removeAttribute("open");
                else parent->setAttribute("open", "");
                dom::Event toggleEvt("toggle", false, false);
                toggleEvt.setIsTrusted(true);
                dispatchDomEvent(ctx, parent, toggleEvt);
                break;
            }
        }
    }
    return JS_UNDEFINED;
}

// Engine-side focus sync for programmatic .focus()/.blur(): commits any
// in-progress IME composition, mirrors the input/textarea control focused
// flags (so typing works after .focus(), as in a browser), and starts/stops
// SDL text input. No-op when the ctx has no engine (iframe sub-documents) or
// the document isn't the engine's app document.
static void syncEngineFocus(JSContext* ctx, bro::dom::Document* doc,
                            bro::dom::Element* oldEl, bro::dom::Element* newEl) {
    auto it = s_ctx_engines.find(ctx);
    if (it == s_ctx_engines.end() || !it->second) return;
    static_cast<bro::engine::Engine*>(it->second)
        ->handleProgrammaticFocus(doc, oldEl, newEl);
}

static JSValue js_element_focus(JSContext* ctx, JSValueConst this_val,
                                int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto* doc = getDocumentForCtx(ctx);
    if (!doc) return JS_UNDEFINED;
    auto* prev = doc->activeElement();
    if (prev == el) return JS_UNDEFINED; // already focused
    syncEngineFocus(ctx, doc, prev, el);
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
    syncEngineFocus(ctx, doc, el, nullptr);
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
    // "style" is never stored in attributes_ (Element::setAttribute routes
    // it into StyleProxy instead) — append it separately.
    if (el->hasAttribute("style")) {
        result += " style=\"" + el->style().cssText() + "\"";
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
        markChildListChanged(doc, el);
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
        markChildListChanged(doc, el);
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
        markChildListChanged(doc, parent);
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
        markChildListChanged(doc, parent);
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
        markChildListChanged(doc, parent);
        fireConnectedIfElement(ctx, node);
    }

    // Remove the old element
    if (doc && !el->id().empty())
        doc->unregisterElementId(el->id());
    markChildListChanged(doc, parent);
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
        if (child->nodeType() == bro::dom::NodeType::Element) {
            invalidateWrapper(ctx, static_cast<bro::dom::Element*>(child));
        } else {
            if (doc) doc->freeNode(child);
        }
    }
    // Append new children
    for (int i = 0; i < argc; ++i) {
        auto* node = nodeOrTextFromArg(ctx, argv[i]);
        if (!node) continue;
        el->appendChild(node);
        markChildListChanged(doc, el);
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
    markChildListChanged(doc, newEl->parentNode());
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
    // "style" lives in StyleProxy, not attributes_ (see Element::setAttribute).
    if (el->hasAttribute("style")) {
        JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, "style"));
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

// requestPointerLock() — captures mouse with relative mode and freezes hit-test
// position to this element until document.exitPointerLock() is called.
static JSValue js_element_requestPointerLock(JSContext* ctx, JSValueConst this_val,
                                              int /*argc*/, JSValueConst* /*argv*/) {
    auto* el = getElement(this_val);
    if (!el) return JS_UNDEFINED;
    auto it = s_ctx_engines.find(ctx);
    auto* engine = (it == s_ctx_engines.end())
                       ? nullptr
                       : static_cast<bro::engine::Engine*>(it->second);
    // v1 cut: pointer lock is main-window only. lockedElement_ and SDL's
    // relative-mouse-mode toggle are both bound to the PRIMARY window, so a
    // lock requested from a secondary window (bro.window.open) or an iframe
    // would capture the wrong window's pointer. Only the main app realm
    // registers an engine for its context, so "no engine here" and "this
    // element is not in the app document" both mean the same thing: a sub-doc
    // realm. Say so out loud instead of no-oping silently.
    if (!engine || el->document() != engine->document()) {
        return JS_ThrowTypeError(ctx,
            "requestPointerLock is only available in the main window's document "
            "(secondary windows opened with bro.window.open, and iframes, do "
            "not support pointer lock)");
    }
    engine->requestPointerLock(el);
    return JS_UNDEFINED;
}

// Pointer capture — setPointerCapture(pointerId) / releasePointerCapture(
// pointerId) / hasPointerCapture(pointerId). Capture is per pointerId: the
// mouse pointer is id 1 (the default when the argument is omitted), touch
// contacts carry their PointerEvent.pointerId (≥ 2). While captured,
// pointermove/up/cancel for that pointer route to this element and the
// capture auto-releases after pointerup/pointercancel (see
// Engine::setPointerCapture). Unknown/inactive ids are a silent no-op.
static int pointerIdArg(JSContext* ctx, int argc, JSValueConst* argv) {
    int id = bro::engine::Engine::kMousePointerId;
    if (argc >= 1) JS_ToInt32(ctx, &id, argv[0]);
    return id;
}

static JSValue js_element_setPointerCapture(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    auto it = s_ctx_engines.find(ctx);
    if (!el || it == s_ctx_engines.end() || !it->second) return JS_UNDEFINED;
    static_cast<bro::engine::Engine*>(it->second)
        ->setPointerCapture(el, pointerIdArg(ctx, argc, argv));
    return JS_UNDEFINED;
}

static JSValue js_element_releasePointerCapture(JSContext* ctx, JSValueConst this_val,
                                                int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    auto it = s_ctx_engines.find(ctx);
    if (!el || it == s_ctx_engines.end() || !it->second) return JS_UNDEFINED;
    static_cast<bro::engine::Engine*>(it->second)
        ->releasePointerCapture(el, pointerIdArg(ctx, argc, argv));
    return JS_UNDEFINED;
}

static JSValue js_element_hasPointerCapture(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    auto it = s_ctx_engines.find(ctx);
    if (!el || it == s_ctx_engines.end() || !it->second) return JS_FALSE;
    return JS_NewBool(ctx, static_cast<bro::engine::Engine*>(it->second)
                               ->hasPointerCapture(el, pointerIdArg(ctx, argc, argv)));
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
    JS_CGETSET_DEF("clientTop",     js_element_get_clientTop, nullptr),
    JS_CGETSET_DEF("clientLeft",    js_element_get_clientLeft, nullptr),
    JS_CGETSET_DEF("hidden",        js_element_get_hidden, js_element_set_hidden),
    JS_CGETSET_DEF("scrollWidth",   js_element_get_scrollWidth, nullptr),
    JS_CGETSET_DEF("scrollHeight",  js_element_get_scrollHeight, nullptr),
    JS_CGETSET_DEF("scrollLeft",    js_element_get_scrollLeft, js_element_set_scrollLeft),
    JS_CGETSET_DEF("scrollTop",     js_element_get_scrollTop, js_element_set_scrollTop),
    JS_CGETSET_DEF("outerHTML",     js_element_get_outerHTML, js_element_set_outerHTML),
    JS_CGETSET_DEF("innerText",     js_element_get_innerText, nullptr),
    JS_CGETSET_DEF("dataset",       js_element_get_dataset, nullptr),
    JS_CGETSET_DEF("ownerDocument", js_element_get_ownerDocument, nullptr),
    JS_CGETSET_DEF("content",      js_element_get_content, nullptr),
    // HTMLMediaElement / HTMLVideoElement
    JS_CGETSET_DEF("currentTime", js_element_video_get_currentTime, js_element_video_set_currentTime),
    JS_CGETSET_DEF("duration",    js_element_video_get_duration, nullptr),
    JS_CGETSET_DEF("paused",      js_element_video_get_paused, nullptr),
    JS_CGETSET_DEF("ended",       js_element_video_get_ended, nullptr),
    JS_CGETSET_DEF("seeking",     js_element_video_get_seeking, nullptr),
    JS_CGETSET_DEF("readyState",  js_element_video_get_readyState, nullptr),
    JS_CGETSET_DEF("networkState", js_element_video_get_networkState, nullptr),
    JS_CGETSET_DEF("currentSrc",  js_element_video_get_currentSrc, nullptr),
    JS_CGETSET_DEF("videoWidth",  js_element_video_get_videoWidth, nullptr),
    JS_CGETSET_DEF("videoHeight", js_element_video_get_videoHeight, nullptr),
    JS_CGETSET_DEF("src",         js_element_video_get_src, js_element_video_set_src),
    JS_CGETSET_DEF("autoplay",    js_element_video_get_autoplay, js_element_video_set_autoplay),
    JS_CGETSET_DEF("controls",    js_element_video_get_controls, js_element_video_set_controls),
    JS_CGETSET_DEF("loop",        js_element_video_get_loop, js_element_video_set_loop),
    JS_CGETSET_DEF("preload",     js_element_video_get_preload, js_element_video_set_preload),
    JS_CGETSET_DEF("defaultMuted",js_element_video_get_defaultMuted, js_element_video_set_defaultMuted),
    JS_CGETSET_DEF("volume",      js_element_video_get_volume, js_element_video_set_volume),
    JS_CGETSET_DEF("muted",       js_element_video_get_muted, js_element_video_set_muted),
    JS_CGETSET_DEF("playbackRate", js_element_video_get_playbackRate, js_element_video_set_playbackRate),
    JS_CGETSET_DEF("defaultPlaybackRate", js_element_video_get_defaultPlaybackRate, js_element_video_set_defaultPlaybackRate),
    JS_CGETSET_DEF("buffered",    js_element_video_get_buffered, nullptr),
    JS_CGETSET_DEF("seekable",    js_element_video_get_seekable, nullptr),
    JS_CGETSET_DEF("played",      js_element_video_get_played, nullptr),
    JS_CFUNC_DEF("play",  0, js_element_video_play),
    JS_CFUNC_DEF("pause", 0, js_element_video_pause),
    JS_CFUNC_DEF("load",  0, js_element_video_load),
    JS_CFUNC_DEF("canPlayType", 1, js_element_video_canPlayType),
    // Iframe control
    JS_CFUNC_DEF("reload", 0, js_element_iframe_reload),
    JS_CFUNC_DEF("capture", 0, js_element_iframe_capture),
    // Form control properties
    JS_CGETSET_DEF("value",       js_element_get_value,       js_element_set_value),
    JS_CGETSET_DEF("checked",     js_element_get_checked,     js_element_set_checked),
    JS_CGETSET_DEF("selected",      js_element_get_selected,      js_element_set_selected),
    JS_CGETSET_DEF("selectedIndex", js_element_get_selectedIndex, js_element_set_selectedIndex),
    JS_CGETSET_DEF("options",       js_element_get_options,       nullptr),
    JS_CGETSET_DEF("type",        js_element_get_type,        js_element_set_type),
    JS_CGETSET_DEF("disabled",    js_element_get_disabled,    js_element_set_disabled),
    JS_CGETSET_DEF("placeholder", js_element_get_placeholder, js_element_set_placeholder),
    JS_CGETSET_DEF("name",        js_element_get_name,        js_element_set_name),
    JS_CGETSET_DEF("required",    js_element_get_required,    js_element_set_required),
    JS_CGETSET_DEF("readOnly",    js_element_get_readOnly,    js_element_set_readOnly),
    JS_CGETSET_DEF("multiple",    js_element_get_multiple,    js_element_set_multiple),
    JS_CGETSET_DEF("pattern",     js_element_get_pattern,     js_element_set_pattern),
    JS_CGETSET_DEF("min",         js_element_get_min,         js_element_set_min),
    JS_CGETSET_DEF("max",         js_element_get_max,         js_element_set_max),
    JS_CGETSET_DEF("step",        js_element_get_step,        js_element_set_step),
    JS_CGETSET_DEF("minLength",   js_element_get_minLength,   js_element_set_minLength),
    JS_CGETSET_DEF("maxLength",   js_element_get_maxLength,   js_element_set_maxLength),
    JS_CGETSET_DEF("size",        js_element_get_size,        js_element_set_size),
    JS_CGETSET_DEF("autocomplete",js_element_get_autocomplete,js_element_set_autocomplete),
    JS_CGETSET_DEF("form",        js_element_get_form,        nullptr),
    JS_CGETSET_DEF("selectionStart", js_element_get_selectionStart, js_element_set_selectionStart),
    JS_CGETSET_DEF("selectionEnd",   js_element_get_selectionEnd,   js_element_set_selectionEnd),
    JS_CGETSET_DEF("files",       js_element_get_files, nullptr),
    JS_CGETSET_DEF("validity",        js_element_get_validity, nullptr),
    JS_CGETSET_DEF("validationMessage", js_element_get_validationMessage, nullptr),
    JS_CGETSET_DEF("willValidate",    js_element_get_willValidate, nullptr),
    JS_CFUNC_DEF("setSelectionRange", 2, js_element_setSelectionRange),
    JS_CFUNC_DEF("select",            0, js_element_select),
    JS_CFUNC_DEF("checkValidity",     0, js_element_checkValidity),
    JS_CFUNC_DEF("reportValidity",    0, js_element_reportValidity),
    JS_CFUNC_DEF("setCustomValidity", 1, js_element_setCustomValidity),
    // HTMLFormElement
    JS_CGETSET_DEF("elements",        js_form_get_elements, nullptr),
    JS_CGETSET_DEF("length",          js_form_get_length, nullptr),
    JS_CFUNC_DEF("submit",            0, js_form_submit),
    JS_CFUNC_DEF("requestSubmit",     0, js_form_requestSubmit),
    JS_CFUNC_DEF("reset",             0, js_form_reset),
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
    JS_CFUNC_DEF("getClientRects",            0, js_element_getClientRects),
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
    JS_CFUNC_DEF("requestPointerLock",        0, js_element_requestPointerLock),
    JS_CFUNC_DEF("setPointerCapture",         1, js_element_setPointerCapture),
    JS_CFUNC_DEF("releasePointerCapture",     1, js_element_releasePointerCapture),
    JS_CFUNC_DEF("hasPointerCapture",         1, js_element_hasPointerCapture),
};

// ===========================================================================
// Registration
// ===========================================================================

void installElementBindings(JSContext* ctx) {
    using Elem = bro::dom::Element;
    qjsbind::Class<Elem>(ctx, "Element", qjsbind::NoGlobal,
                          js_element_finalizer)
        .function_list(js_element_proto_funcs,
                       sizeof(js_element_proto_funcs) / sizeof(js_element_proto_funcs[0]));

    js_element_class_id = qjsbind::class_id<Elem>();
}

} // namespace bro::js
