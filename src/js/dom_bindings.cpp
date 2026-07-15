#include "js/dom_bindings.h"
#include "js/dom_bindings_internal.h"
#include "js/custom_elements.h"
#include "js/event_dispatch.h"
#include "dom/document.h"
#include "dom/event.h"
#include "util/log.h"

#include <qjsbind/qjsbind.h>

#include "dom_polyfills.js.h"
#include "observer_polyfills.js.h"

#include <cstring>

namespace bro::js {

// ===========================================================================
// Class IDs
// ===========================================================================

JSClassID js_document_class_id = 0;
JSClassID js_element_class_id  = 0;
JSClassID js_node_class_id    = 0;
JSClassID js_event_class_id    = 0;
JSClassID js_nodelist_class_id = 0;
JSClassID js_cssstyle_class_id = 0;
JSClassID js_computed_class_id = 0;
JSClassID js_tokenlist_class_id = 0;
JSClassID js_shadowroot_class_id = 0;
JSClassID js_htmlcollection_class_id = 0;
JSClassID js_range_class_id = 0;
JSClassID js_selection_class_id = 0;

// ===========================================================================
// Per-context state
// ===========================================================================

std::unordered_map<JSContext*, bro::dom::Document*> s_ctx_documents;
std::unordered_map<JSContext*, DomBindings::GetContextFactory> s_ctx_factories;
std::unordered_map<JSContext*, void*> s_ctx_sdl_windows;
std::unordered_map<JSContext*, void*> s_ctx_engines;

bro::dom::Document* getDocumentForCtx(JSContext* ctx) {
    auto it = s_ctx_documents.find(ctx);
    return it != s_ctx_documents.end() ? it->second : nullptr;
}

void DomBindings::setGetContextFactory(JSContext* ctx, GetContextFactory factory) {
    s_ctx_factories[ctx] = std::move(factory);
}

// ===========================================================================
// Wrap / unwrap helpers (public API)
// ===========================================================================

JSValue DomBindings::wrapElement(JSContext* ctx, void* element_ptr)
{
    if (!element_ptr) return JS_NULL;

    auto* elem = static_cast<bro::dom::Element*>(element_ptr);

    // Fast path: the element already knows its wrapper. The map keeps a strong
    // ref to every cached wrapper, so a non-null pointer is always a live object
    // (the finalizer / detach paths null it otherwise). This skips the global
    // fetch + "__bro_elem_map" atom intern + itoa + hash lookup below, which is
    // the bulk of the per-crossing cost on DOM-heavy code.
    if (void* w = elem->jsWrapper()) {
        return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, w));
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (JS_IsUndefined(elemMap)) {
        elemMap = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__bro_elem_map", JS_DupValue(ctx, elemMap));
    }

    std::string key = std::to_string(elem->nodeId());
    JSValue existing = JS_GetPropertyStr(ctx, elemMap, key.c_str());
    if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) {
        // Seed the pointer cache from a map entry another path created (custom
        // element upgrade, event dispatch), so the next crossing takes the fast
        // path above.
        elem->setJsWrapper(JS_VALUE_GET_PTR(existing));
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

    upgradeCustomElementPrototype(ctx, obj, elem->tagName());

    // Do not resurrect a doomed element into the map. If the document no longer
    // owns this node it was already freed (or is queued in pendingFrees_) — a
    // node removed mid-event-dispatch, then re-wrapped as the dispatch unwinds
    // the propagation path (target/currentTarget of the very handler that
    // removed it). Caching it here re-adds a dangling __bro_elem_map entry that
    // outlives the node and faults the next sweepOrphanedWrappers(). The caller
    // still gets a usable (transient) wrapper for the tail of this dispatch; it
    // just isn't persisted. ownsNode() is a pointer-hash lookup, never a deref.
    auto* ctxDoc = getDocumentForCtx(ctx);
    if (!ctxDoc || ctxDoc->ownsNode(elem)) {
        JS_SetPropertyStr(ctx, elemMap, key.c_str(), JS_DupValue(ctx, obj));
        // Cache the wrapper on the element only when it is also rooted in the
        // map — a transient wrapper for a doomed node must not be cached, or the
        // raw pointer would outlive it.
        elem->setJsWrapper(JS_VALUE_GET_PTR(obj));
    }

    JS_FreeValue(ctx, elemMap);
    JS_FreeValue(ctx, global);
    return obj;
}

void* DomBindings::unwrapElement(JSContext* /*ctx*/, JSValueConst val)
{
    return JS_GetOpaque(val, js_element_class_id);
}

JSClassID DomBindings::elementClassId()
{
    return js_element_class_id;
}

JSValue DomBindings::wrapDocument(JSContext* ctx, void* document_ptr)
{
    if (!document_ptr) return JS_NULL;

    return qjsbind::wrap_unowned<bro::dom::Document>(ctx,
        static_cast<bro::dom::Document*>(document_ptr));
}

// ===========================================================================
// install() – register everything
// ===========================================================================

// Thread-local map from Document -> JSContext so the Document's mutation
// notifications can find the right JS runtime to fire selectionchange into.
static std::unordered_map<bro::dom::Document*, JSContext*> s_doc_to_ctx;

// Fire `selectionchange` on the document (non-bubbling, non-cancelable per
// spec). Dispatched to the documentElement which is the closest analogue to
// "Document" as an EventTarget in our implementation.
static void fireSelectionChangeOnDocument(bro::dom::Document* doc) {
    auto it = s_doc_to_ctx.find(doc);
    if (it == s_doc_to_ctx.end() || !it->second) return;
    JSContext* ctx = it->second;
    auto* target = doc->documentElement();
    if (!target) return;
    bro::dom::Event event("selectionchange", /*bubbles=*/false, /*cancelable=*/false);
    event.setIsTrusted(true);
    dispatchDomEvent(ctx, target, event);
}

// Drop a doomed node's JS wrapper. Fired from Document::freeNode() (see the
// NodeFreedCallback hook) for every node in a freed subtree, while its memory
// is still valid. Nulls the wrapper's opaque Element* so any lingering JS
// reference resolves to null instead of dereferencing freed memory, and
// removes the __bro_elem_map entry so the orphan sweep never sees it.
static void fireNodeFreed(bro::dom::Document* doc, bro::dom::Node* node) {
    if (!node || node->nodeType() != bro::dom::NodeType::Element) return;
    // Drop the element's cached wrapper pointer before the node is destroyed so
    // no fast-path wrap can hand back a wrapper that's about to be invalidated.
    static_cast<bro::dom::Element*>(node)->setJsWrapper(nullptr);
    auto it = s_doc_to_ctx.find(doc);
    if (it == s_doc_to_ctx.end() || !it->second) return;
    JSContext* ctx = it->second;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (!JS_IsUndefined(elemMap) && !JS_IsNull(elemMap)) {
        std::string key = std::to_string(node->nodeId());
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

void DomBindings::install(JSContext* ctx, void* document_ptr)
{
    JSRuntime* rt = JS_GetRuntime(ctx);

    // ----- qjsbind-managed classes (handle IDs, class registration, prototypes) -----
    installEventBindings(ctx);
    installNodeBindings(ctx);
    installDocumentBindings(ctx);
    installShadowRootBindings(ctx);
    installElementBindings(ctx);
    installStyleBindings(ctx);
    installRangeBindings(ctx);
    installSelectionBindings(ctx);

    // Wire Document's selectionchange callback through JS event dispatch.
    auto* doc = static_cast<bro::dom::Document*>(document_ptr);
    if (doc) {
        s_doc_to_ctx[doc] = ctx;
        doc->setSelectionChangeCallback(&fireSelectionChangeOnDocument);
        doc->setNodeFreedCallback(&fireNodeFreed);
    }

    // ----- Stash Document pointer for orphan management (per-context) -----
    s_ctx_documents[ctx] = static_cast<bro::dom::Document*>(document_ptr);

    // ----- Set global `document` -----
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue docObj = wrapDocument(ctx, document_ptr);
    JS_SetPropertyStr(ctx, global, "document", docObj);
    JS_FreeValue(ctx, global);

    // ----- Polyfills for jQuery/framework compatibility -----
    JSValue r = JS_Eval(ctx, js_dom_polyfills, strlen(js_dom_polyfills),
                        "<dom-polyfills>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, r);

    // ----- Observer polyfills -----
    JSValue r2 = JS_Eval(ctx, js_observer_polyfills, strlen(js_observer_polyfills),
                         "<observer-polyfills>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, r2);

    // Register native getComputedStyle on window (globalThis)
    {
        JSValue g = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, g, "getComputedStyle",
            JS_NewCFunction(ctx, js_window_getComputedStyle, "getComputedStyle", 1));
        JS_FreeValue(ctx, g);
    }
}

// ===========================================================================
// Cleanup
// ===========================================================================

void DomBindings::setSDLWindow(JSContext* ctx, void* sdl_window) {
    s_ctx_sdl_windows[ctx] = sdl_window;
}

void DomBindings::setEngine(JSContext* ctx, void* engine) {
    s_ctx_engines[ctx] = engine;
}

void DomBindings::cleanup(JSContext* ctx) {
    // Drop globalThis.__bro_elem_map, the element→wrapper cache. It is a strong
    // ref to every wrapper ever handed to script, so it has to go before the
    // runtime tears down or the wrappers (and the listener callbacks they hold)
    // are still reachable at JS_FreeRuntime.
    //
    // This used to also delete each wrapper's "__bro_listeners" property, on the
    // theory that the property key was what kept that atom interned at shutdown.
    // It wasn't, and the sweep never moved the leak count: the extra ref came
    // from an unbalanced JS_DupAtom in addEventListener (JS_SetProperty does not
    // take the atom, unlike the value). With that fixed the wrappers release the
    // atom themselves when they are collected, and the sweep is unnecessary.
    JSValue global = JS_GetGlobalObject(ctx);
    JSAtom mapAtom = JS_NewAtom(ctx, "__bro_elem_map");
    JS_DeleteProperty(ctx, global, mapAtom, 0);
    JS_FreeAtom(ctx, mapAtom);
    JS_FreeValue(ctx, global);

    // Drop Document→JSContext mapping for any document that used this context.
    for (auto it = s_doc_to_ctx.begin(); it != s_doc_to_ctx.end(); ) {
        if (it->second == ctx) it = s_doc_to_ctx.erase(it);
        else ++it;
    }
    s_ctx_documents.erase(ctx);
    s_ctx_factories.erase(ctx);
    s_ctx_sdl_windows.erase(ctx);
    s_ctx_engines.erase(ctx);
}

void DomBindings::cleanupRuntime(JSRuntime* rt) {
    cleanupCanvasContextCache(rt);
}

void DomBindings::sweepOrphanedWrappers(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (JS_IsUndefined(elemMap) || JS_IsNull(elemMap)) {
        JS_FreeValue(ctx, elemMap);
        JS_FreeValue(ctx, global);
        return;
    }

    // Every wrapper in this context's map backs a node owned by this context's
    // document. ownsNode() is a pointer-keyed hash lookup — it never
    // dereferences the pointer — so it is the one safe question we can ask about
    // a raw Element* that might already be freed.
    bro::dom::Document* ctxDoc = getDocumentForCtx(ctx);

    JSPropertyEnum* props = nullptr;
    uint32_t len = 0;
    JS_GetOwnPropertyNames(ctx, &props, &len, elemMap,
                           JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY);

    int dangling = 0;
    for (uint32_t i = 0; i < len; i++) {
        JSValue val = JS_GetProperty(ctx, elemMap, props[i].atom);
        auto* el = static_cast<bro::dom::Element*>(
            JS_GetOpaque(val, js_element_class_id));

        // Backstop for any free path that didn't eagerly clear its wrapper
        // (the eager path is fireNodeFreed / invalidateWrapper). If the document
        // no longer owns this node, the raw Element* dangles — it was freed, or
        // is queued in pendingFrees_ awaiting destruction. Dereferencing it,
        // even for isAlive()'s magic_ probe, is a use-after-free the instant the
        // page is unmapped (the sweepOrphanedWrappers crash). Drop the stale
        // entry without ever touching the pointer.
        if (el && ctxDoc && !ctxDoc->ownsNode(el)) {
            ++dangling;
            JS_SetOpaque(val, nullptr);
            JS_DeleteProperty(ctx, elemMap, props[i].atom, 0);
            JS_FreeValue(ctx, val);
            continue;
        }

        if (el && el->isAlive() && !el->parentNode() &&
            el->childNodes().empty() &&
            el->tagName() == "#DOCUMENT-FRAGMENT") {
            auto* doc = el->document();
            JS_SetOpaque(val, nullptr);
            JS_DeleteProperty(ctx, elemMap, props[i].atom, 0);
            if (doc) doc->freeNode(el);
        }
        JS_FreeValue(ctx, val);
    }

    // Any dangling entry means an eager-clear path was missed — should be zero
    // now that wrapElement() refuses to cache unowned nodes. Surface it once (not
    // per entry) if it ever recurs, so a new leak path is visible without spam.
    if (dangling > 0)
        LOG_WARN("sweepOrphanedWrappers: dropped %d dangling wrapper(s)", dangling);

    JS_FreePropertyEnum(ctx, props, len);
    JS_FreeValue(ctx, elemMap);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
