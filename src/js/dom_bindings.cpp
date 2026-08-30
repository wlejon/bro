#include "js/dom_bindings.h"
#include "js/dom_bindings_internal.h"
#include "js/custom_elements.h"
#include "js/html_interfaces.h"
#include "js/event_dispatch.h"
#include "js/web_animation_bindings.h"
#include "js/matchmedia_bindings.h"
#include "dom/document.h"
#include "dom/event.h"
#include "util/log.h"
#include "util/object_url.h"

#include <api/api.h>  // brokit::api::blobBytes

#include <qjsbind/qjsbind.h>

#include "dom_polyfills.js.h"
#include "observer_polyfills.js.h"
#include "file_polyfills.js.h"

#include <atomic>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

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

// Elements the orphan sweep has taken back out of __bro_elem_map because they
// are no longer in a tree — see the rooting rule above sweepOrphanedWrappers().
// Raw pointers, kept correct by fireNodeFreed(), which erases a node here
// before its memory can be reused. The set is bounded by how many detached
// elements are alive at once, which for a list being rebuilt is one frame's
// worth.
std::unordered_map<JSContext*, std::unordered_set<bro::dom::Element*>> s_unrooted;

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

static void noteDetachedElementWrapper(JSContext* ctx, bro::dom::Element* el,
                                       JSValue wrapper);

JSValue DomBindings::wrapElement(JSContext* ctx, void* element_ptr)
{
    if (!element_ptr) return JS_NULL;

    auto* elem = static_cast<bro::dom::Element*>(element_ptr);

    // Fast path: the element already knows its wrapper. A non-null pointer is
    // always a live object, because every path that ends a wrapper's life nulls
    // this slot first — the element finalizer, fireNodeFreed, fireNodeDestroying
    // and invalidateWrapper. That is the whole guarantee, and it is *not* "the
    // map holds a strong ref to it": the map roots only elements that are in a
    // tree (see sweepOrphanedWrappers), so a detached element's wrapper is held
    // by whatever JS still points at it and by nothing else. This skips the
    // global fetch + "__bro_elem_map" atom intern + itoa + hash lookup below,
    // which is the bulk of the per-crossing cost on DOM-heavy code.
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

    // A registered custom element wins: its class already extends HTMLElement,
    // so it carries the built-in chain anyway. Otherwise give the wrapper the
    // interface prototype for its tag (HTMLCanvasElement, HTMLImageElement,
    // ...), which is what makes `instanceof` answer the way the web says.
    if (!upgradeCustomElementPrototype(ctx, obj, elem->tagName())) {
        JSValue ifaceProto = htmlInterfaceProto(ctx, elem->tagName());
        if (JS_IsObject(ifaceProto)) JS_SetPrototype(ctx, obj, ifaceProto);
    }

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
    } else if (ctxDoc->isNodeLive(elem)) {
        // Doomed but not yet destroyed: queued in pendingFrees_. Deliberately
        // NOT added to __bro_elem_map (see above) — but it must still be
        // reachable from the element, because fireNodeFreed has already run for
        // this node and can never come back for a wrapper made after the fact.
        // The jsWrapper_ slot is what fireNodeDestroying reads at
        // drainPendingFrees() to null this opaque while the storage is still
        // valid; without it the wrapper outlives the Element and every later
        // touch reads freed memory. Safe to cache: if the wrapper is collected
        // first, its finalizer clears the slot (the Element is alive until the
        // drain, so that clear is a valid write).
        elem->setJsWrapper(JS_VALUE_GET_PTR(obj));
    } else if (auto* ddoc = elem->document();
               ddoc && isDetachedDocument(ddoc) && ddoc->ownsNode(elem)) {
        // Detached-document node (DOMParser). Cache via the element's wrapper
        // pointer only — NOT the strong __bro_elem_map, so an unreferenced
        // parsed tree stays collectable — and record it in the weak registry +
        // pin the Document wrapper so a held child keeps its document alive.
        // The element finalizer undoes both.
        elem->setJsWrapper(JS_VALUE_GET_PTR(obj));
        noteDetachedElementWrapper(ctx, elem, obj);
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

JSContext* getCtxForDocument(bro::dom::Document* doc) {
    auto it = s_doc_to_ctx.find(doc);
    return it != s_doc_to_ctx.end() ? it->second : nullptr;
}

static void fireNodeFreed(bro::dom::Document* doc, bro::dom::Node* node);
static void fireNodeDestroying(bro::dom::Document* doc, bro::dom::Node* node);
static void fireNodeAdopted(bro::dom::Document* newDoc,
                            bro::dom::Document* oldDoc, bro::dom::Node* node);

// ===========================================================================
// Detached documents (DOMParser) — JS-owned bro::dom::Document instances
// ===========================================================================
//
// DOMParser().parseFromString returns a REAL detached Document: parsed by the
// same gumbo path as the app document, exposed through the same Document /
// Element wrapper classes, but owned by JS. Ownership lives in a hidden
// "holder" object (non-configurable property of the Document wrapper) whose
// finalizer deletes the Document when the wrapper is collected.
//
// Wrapper liveness differs from the main document on purpose:
//   * element wrappers are cached ONLY via Element::jsWrapper() (the element
//     finalizer clears it) — never in the strong __bro_elem_map, so a parsed
//     tree nobody references stays collectable;
//   * each element wrapper pins the Document wrapper through a hidden
//     "__bro_owner_doc" property, so holding any child keeps the whole
//     document alive (plain GC reachability — no custom gc_mark needed);
//   * a weak per-document registry (nodeId → wrapper) lets the holder null
//     every surviving wrapper's opaque before deleting the Document, so a
//     wrapper that outlives its document goes cleanly inert instead of
//     dangling. Entries are erased by the element finalizer, so the holder
//     only ever touches wrappers whose finalizer has not yet run — their
//     JSObject memory is still valid in every teardown order (QuickJS frees
//     cycle members' memory only after the whole finalizer phase).
//
// The registry deliberately holds raw JSObject* (weak). All access is on the
// JS thread.
struct DetachedDocState {
    JSContext* ctx = nullptr;
    JSObject* docWrapper = nullptr;                       // weak
    std::unordered_map<uint32_t, JSObject*> elemWrappers; // weak, nodeId-keyed
};
static std::unordered_map<bro::dom::Document*, DetachedDocState> s_detached_docs;

static JSClassID s_detached_holder_class_id = 0;

bool isDetachedDocument(bro::dom::Document* doc) {
    return doc && s_detached_docs.count(doc) != 0;
}

JSValue detachedDocumentWrapper(JSContext* ctx, bro::dom::Document* doc) {
    auto it = s_detached_docs.find(doc);
    if (it == s_detached_docs.end() || !it->second.docWrapper) return JS_NULL;
    return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, it->second.docWrapper));
}

// Called from wrapElement for a freshly created detached-document wrapper:
// record it in the weak registry and pin the Document wrapper from it.
static void noteDetachedElementWrapper(JSContext* ctx, bro::dom::Element* el,
                                       JSValue wrapper) {
    auto it = s_detached_docs.find(el->document());
    if (it == s_detached_docs.end()) return;
    it->second.elemWrappers[el->nodeId()] =
        static_cast<JSObject*>(JS_VALUE_GET_PTR(wrapper));
    JSValue dw = JS_MKPTR(JS_TAG_OBJECT, it->second.docWrapper);
    // Non-configurable/non-writable so script can't sever the lifetime pin.
    JS_DefinePropertyValueStr(ctx, wrapper, "__bro_owner_doc",
                              JS_DupValue(ctx, dw), 0);
}

// Called from the element wrapper finalizer (never under shutdown): drop the
// weak registry entry so the holder never touches a freed wrapper object.
void dropDetachedElementWrapper(bro::dom::Element* el, void* wrapperPtr) {
    auto* doc = el->document();
    if (!doc) return;
    auto it = s_detached_docs.find(doc);
    if (it == s_detached_docs.end()) return;
    auto wit = it->second.elemWrappers.find(el->nodeId());
    if (wit != it->second.elemWrappers.end() && wit->second == wrapperPtr)
        it->second.elemWrappers.erase(wit);
}

// Holder finalizer — the Document wrapper is being collected. Null every
// surviving element wrapper's opaque (they go inert; getElement() then
// returns nullptr and every binding takes its safe-default path), then free
// the Document. Under engine shutdown the wrapper objects may already be
// gone (their finalizers skip bookkeeping), so only the delete runs.
static void js_detached_doc_holder_finalizer(JSRuntime* /*rt*/, JSValue val) {
    auto* doc = static_cast<bro::dom::Document*>(
        JS_GetOpaque(val, s_detached_holder_class_id));
    if (!doc) return;
    auto it = s_detached_docs.find(doc);
    if (it != s_detached_docs.end()) {
        if (!isElementFinalizerShutdown()) {
            for (auto& [id, obj] : it->second.elemWrappers) {
                (void)id;
                JS_SetOpaque(JS_MKPTR(JS_TAG_OBJECT, obj), nullptr);
            }
            if (it->second.docWrapper) {
                JS_SetOpaque(JS_MKPTR(JS_TAG_OBJECT, it->second.docWrapper),
                             nullptr);
            }
        }
        s_detached_docs.erase(it);
    }
    s_doc_to_ctx.erase(doc);
    delete doc;
}

static const JSClassDef js_detached_doc_holder_class = {
    "DetachedDocumentHolder",
    js_detached_doc_holder_finalizer,
    nullptr, nullptr, nullptr
};

JSValue wrapDetachedDocument(JSContext* ctx, bro::dom::Document* doc) {
    if (!doc) return JS_NULL;
    JSValue docObj = DomBindings::wrapDocument(ctx, doc);
    if (JS_IsException(docObj)) {
        delete doc;
        return docObj;
    }
    JSValue holder = JS_NewObjectClass(
        ctx, static_cast<int>(s_detached_holder_class_id));
    if (JS_IsException(holder)) {
        JS_FreeValue(ctx, docObj);
        delete doc;
        return holder;
    }
    JS_SetOpaque(holder, doc);
    // Non-configurable: deleting this property would leak/UAF the Document.
    JS_DefinePropertyValueStr(ctx, docObj, "__bro_doc_holder", holder, 0);
    s_detached_docs[doc] = DetachedDocState{
        ctx, static_cast<JSObject*>(JS_VALUE_GET_PTR(docObj)), {}};
    // Register for freed-node wrapper invalidation, same as the main document.
    s_doc_to_ctx[doc] = ctx;
    doc->setNodeFreedCallback(&fireNodeFreed);
    doc->setNodeDestroyingCallback(&fireNodeDestroying);
    doc->setNodeAdoptedCallback(&fireNodeAdopted);
    doc->setElementClonedCallback(&fireElementCloned);
    return docObj;
}

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
    if (!node) return;
    if (node->nodeType() != bro::dom::NodeType::Element) {
        // Text/comment wrappers live in __bro_node_map, not __bro_elem_map, and
        // are cached strongly — so without this they outlived the node and the
        // next `.data`/`.length` access read destroyed memory. Their opaque is a
        // generation-checked handle so they would go inert on their own, but the
        // map entry still has to go or the cache grows forever.
        auto it = s_doc_to_ctx.find(doc);
        if (it != s_doc_to_ctx.end() && it->second)
            invalidateNodeWrapper(it->second, node);
        return;
    }
    // Drop the element's cached wrapper pointer before the node is destroyed so
    // no fast-path wrap can hand back a wrapper that's about to be invalidated.
    static_cast<bro::dom::Element*>(node)->setJsWrapper(nullptr);
    auto it = s_doc_to_ctx.find(doc);
    if (it == s_doc_to_ctx.end() || !it->second) return;
    JSContext* ctx = it->second;

    // Out of the sweep's detached set before the memory can be reused. That set
    // holds raw pointers and asks them for a parent on the next pass, so a freed
    // Element left in it would be answered by whatever is allocated at that
    // address next.
    auto sit = s_unrooted.find(ctx);
    if (sit != s_unrooted.end())
        sit->second.erase(static_cast<bro::dom::Element*>(node));

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

    // Detached-document nodes are cached outside __bro_elem_map — null the
    // wrapper's opaque via the weak registry so it goes inert now, before the
    // node memory is eventually freed.
    auto dit = s_detached_docs.find(doc);
    if (dit != s_detached_docs.end()) {
        auto wit = dit->second.elemWrappers.find(node->nodeId());
        if (wit != dit->second.elemWrappers.end()) {
            JS_SetOpaque(JS_MKPTR(JS_TAG_OBJECT, wit->second), nullptr);
            dit->second.elemWrappers.erase(wit);
        }
    }
}

// Last-chance wrapper invalidation. Fired from Document::drainPendingFrees()
// for every node in a doomed subtree, at the last instant its storage is valid.
//
// fireNodeFreed() already ran for these nodes when freeNode() queued them, so
// jsWrapper_ is normally null here and this does nothing. The case it exists
// for is a wrapper created AFTER the node was doomed: wrapElement() hands out a
// transient wrapper when an event dispatch, still unwinding its propagation
// path, re-wraps the target its own handler just removed. Such a wrapper is
// rooted in neither __bro_elem_map nor the detached-document registry, so
// fireNodeFreed cannot reach it — but the element's jsWrapper_ slot can, and
// that is exactly what wrapElement records it in.
//
// Nulling the opaque here keeps the invariant every other free path maintains:
// a wrapper is invalidated eagerly, while the memory behind it is still valid.
// Afterwards getElement() sees a null opaque and the wrapper is simply inert —
// no liveness probing of freed storage anywhere.
static void fireNodeDestroying(bro::dom::Document* doc, bro::dom::Node* node) {
    if (!node || node->nodeType() != bro::dom::NodeType::Element) return;
    auto* elem = static_cast<bro::dom::Element*>(node);
    void* w = elem->jsWrapper();
    if (!w) return;
    elem->setJsWrapper(nullptr);
    // Under engine shutdown the wrapper objects may already be gone (their
    // finalizers skip the bookkeeping that would have cleared the slot above),
    // so the pointer is only trustworthy while the runtime is up. That is also
    // exactly when js_element_finalizer skips its own dereference, so nothing
    // reads the Element we are declining to detach from.
    //
    // Deliberately NOT gated on s_doc_to_ctx: DomBindings::cleanup() drops that
    // mapping before the caller destroys the Document, and ~Document invoking
    // this hook is the last chance to null these opaques. The wrapper objects
    // outlive the mapping — they are not freed until JS_FreeContext, after.
    (void)doc;
    if (isElementFinalizerShutdown()) return;
    JS_SetOpaque(JS_MKPTR(JS_TAG_OBJECT, w), nullptr);
}

// A node's owner document changed (Document::adoptNode). Cached wrappers are
// keyed and lifetime-checked per document, so both caches need fixing up:
// text/comment handles get re-pointed at the new document, and an element that
// moved out of a detached (DOMParser) document leaves that document's weak
// registry and joins the strong __bro_elem_map of the document it now belongs
// to — otherwise the parser document's holder would still null its opaque, and
// a node living in the live tree would go inert.
static void fireNodeAdopted(bro::dom::Document* newDoc,
                            bro::dom::Document* oldDoc,
                            bro::dom::Node* node) {
    if (!node || !newDoc) return;
    auto it = s_doc_to_ctx.find(newDoc);
    if (it == s_doc_to_ctx.end() || !it->second) {
        it = s_doc_to_ctx.find(oldDoc);
        if (it == s_doc_to_ctx.end() || !it->second) return;
    }
    JSContext* ctx = it->second;

    if (node->nodeType() != bro::dom::NodeType::Element) {
        repointNodeWrapper(ctx, node, newDoc);
        return;
    }

    auto* elem = static_cast<bro::dom::Element*>(node);

    // Leaving a detached document: drop the weak registry entry so its holder
    // never nulls this wrapper's opaque.
    auto dit = s_detached_docs.find(oldDoc);
    if (dit != s_detached_docs.end())
        dit->second.elemWrappers.erase(elem->nodeId());

    void* w = elem->jsWrapper();
    if (!w) return;  // never wrapped — nothing cached to fix up

    // Joining a non-detached document that this context owns: the wrapper now
    // belongs in the strong map, same as one wrapElement would have made.
    if (isDetachedDocument(newDoc)) {
        noteDetachedElementWrapper(ctx, elem, JS_MKPTR(JS_TAG_OBJECT, w));
        return;
    }
    if (getDocumentForCtx(ctx) != newDoc) return;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (JS_IsUndefined(elemMap)) {
        elemMap = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__bro_elem_map", JS_DupValue(ctx, elemMap));
    }
    if (!JS_IsNull(elemMap)) {
        std::string key = std::to_string(elem->nodeId());
        JS_SetPropertyStr(ctx, elemMap, key.c_str(),
                          JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, w)));
    }
    JS_FreeValue(ctx, elemMap);
    JS_FreeValue(ctx, global);
}

// ===========================================================================
// Object URLs — the native half of URL.createObjectURL
// ===========================================================================
//
// The JS half is in js/file_polyfills.js: it keeps the Blob alive for `fetch`
// and calls in here so the bytes also reach the C++ consumers of a URL — the
// <img> size probe, the draw path's image cache — which have no way to read a
// JS Blob, and in the draw path's case no JSContext to read it with.
//
// Minting the URL here rather than reusing brokit's JS-side counter keeps ids
// unique across realms: an iframe and its parent each run their own copy of
// the polyfill against this one process-global table.
static JSValue js_createObjectURL(JSContext* ctx, JSValueConst /*this_val*/,
                                  int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    const uint8_t* data = nullptr;
    size_t len = 0;
    std::string type;
    if (!brokit::api::blobBytes(ctx, argv[0], &data, &len, &type))
        return JS_NULL;   // not a Blob — the JS side falls back

    static std::atomic<uint64_t> counter{1};
    std::string url = "blob:bro/" +
                      std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    util::registerObjectURL(url, std::vector<uint8_t>(data, data + len),
                            std::move(type));
    return JS_NewString(ctx, url.c_str());
}

static JSValue js_revokeObjectURL(JSContext* ctx, JSValueConst /*this_val*/,
                                  int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char* url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_UNDEFINED;
    util::revokeObjectURL(url);
    JS_FreeCString(ctx, url);
    return JS_UNDEFINED;
}

void DomBindings::install(JSContext* ctx, void* document_ptr)
{
    JSRuntime* rt = JS_GetRuntime(ctx);

    // Class backing detached-document ownership (see wrapDetachedDocument).
    JS_NewClassID(rt, &s_detached_holder_class_id);
    if (!JS_IsRegisteredClass(rt, s_detached_holder_class_id))
        JS_NewClass(rt, s_detached_holder_class_id, &js_detached_doc_holder_class);

    // ----- qjsbind-managed classes (handle IDs, class registration, prototypes) -----
    installEventBindings(ctx);
    installNodeBindings(ctx);
    installDocumentBindings(ctx);
    installShadowRootBindings(ctx);
    installElementBindings(ctx);
    installStyleBindings(ctx);
    installRangeBindings(ctx);
    installSelectionBindings(ctx);
    installWebAnimationBindings(ctx);
    installMatchMediaBindings(ctx);

    // Put Element, Document and ShadowRoot underneath Node.prototype.
    //
    // qjsbind gives each class a prototype whose parent is Object.prototype, so
    // without this the DOM's inheritance chain is flat and `el instanceof Node`
    // is false — for an element. Code that guards an append with that test then
    // silently rejects every element it is given. This has to run after all
    // three classes are registered, which is why it is here and not at the
    // registration sites.
    linkNodePrototype(ctx, qjsbind::class_id<bro::dom::Element>());
    linkNodePrototype(ctx, js_document_class_id);
    linkNodePrototype(ctx, js_shadowroot_class_id);

    // Wire Document's selectionchange callback through JS event dispatch.
    auto* doc = static_cast<bro::dom::Document*>(document_ptr);
    if (doc) {
        s_doc_to_ctx[doc] = ctx;
        doc->setSelectionChangeCallback(&fireSelectionChangeOnDocument);
        doc->setNodeFreedCallback(&fireNodeFreed);
        doc->setNodeDestroyingCallback(&fireNodeDestroying);
        doc->setNodeAdoptedCallback(&fireNodeAdopted);
        doc->setElementClonedCallback(&fireElementCloned);
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

    // ----- Object URLs: the native half of URL.createObjectURL -----
    {
        JSValue g = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, g, "__bro_createObjectURL",
            JS_NewCFunction(ctx, js_createObjectURL, "__bro_createObjectURL", 1));
        JS_SetPropertyStr(ctx, g, "__bro_revokeObjectURL",
            JS_NewCFunction(ctx, js_revokeObjectURL, "__bro_revokeObjectURL", 1));
        JS_FreeValue(ctx, g);
    }

    // ----- File API: FileReader, object URLs, dropped paths -> File -----
    JSValue r3 = JS_Eval(ctx, js_file_polyfills, strlen(js_file_polyfills),
                         "<file-polyfills>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, r3);

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

void* DomBindings::engineFor(JSContext* ctx) {
    auto it = s_ctx_engines.find(ctx);
    return it == s_ctx_engines.end() ? nullptr : it->second;
}

void DomBindings::cleanup(JSContext* ctx) {
    // Release the strong pins that keep running Animation wrappers alive for
    // finish delivery — they must not survive into JS_FreeRuntime.
    cleanupWebAnimationBindings(ctx);

    // Release matchMedia's strong pins (listening MediaQueryLists are kept
    // alive for change delivery) and its per-realm delivery bookkeeping.
    cleanupMatchMediaBindings(ctx);

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
    s_unrooted.erase(ctx);
}

void DomBindings::cleanupRuntime(JSRuntime* rt) {
    cleanupCanvasContextCache(rt);
}

// ── What the strong element map is allowed to keep alive ───────────────────
//
// **__bro_elem_map roots the elements that are in a tree, and nothing else.**
// That sentence is the whole of this pass, and it used to be missing — with the
// consequence that a bro application could not run for an afternoon.
//
// The map holds a strong reference to every element wrapper it caches, and the
// only thing that ever calls Document::freeNode for an ordinary removal is the
// element finalizer's parentless branch. So while the map held a detached
// element's wrapper, that wrapper could never be collected, its finalizer could
// never run, and the Element behind it — with its computed style, its attribute
// maps and its interned map key — was retained for the life of the process.
// removeChild(), textContent=, innerHTML= and replaceChildren() all detach
// without destroying, deliberately and per spec, so **every one of them leaked**;
// only el.remove(), which calls freeNode outright, did not.
//
// Measured, because the size of it is the reason this is not a tidy-up: a list
// of ten rows rebuilt a hundred times the way `put()` in an app's DOM helper
// rebuilds one — clear with removeChild, append the replacement — retained all
// two thousand elements it built, at **1.5 kB apiece**. Both halves of the
// once-a-second pass below are linear in what is retained, so at 1.2 M held
// elements the enumeration cost 121 ms and JS_RunGC 441 ms: **56% of every
// second**, climbing, until the window stops answering. An application whose
// panels redraw off a progress number reached that in an afternoon.
//
// **Why demotion rather than destruction.** A detached element stays
// re-insertable per spec, and this engine leans on that — a live <video> or a
// canvas kept across a rebuild is the documented reason replaceChildren() stopped
// invalidating the wrappers of the children it removes. So this does not free
// anything. It takes the map's reference away and leaves Element::jsWrapper()
// pointing at the wrapper, exactly as the detached-document (DOMParser) path
// already does: JS still holding the element keeps it alive and re-insertable,
// and JS holding nothing lets the wrapper collect, at which point the finalizer's
// existing parentless branch frees the node. The GC decides, which is what should
// have been deciding all along.
//
// **Why the second pass exists.** JS listeners live *on the wrapper*
// (`__bro_listeners`), so an element that is in a tree must stay rooted or a
// collection would silently take its handlers with it. An element demoted while
// detached and put back afterwards has to be rooted again, and there is no one
// place to hook that: appendChild, insertBefore, replaceChild, append, prepend,
// before, after, replaceWith and replaceChildren all insert, and textContent=
// and innerHTML= reach into the child list directly. So re-rooting is asked here,
// of the small set of elements this pass has taken out, rather than asserted at
// nine call sites where the tenth is the bug. The window in which a re-attached
// element is unrooted is bounded by one sweep, and throughout it the element is
// held by whatever JS reference performed the re-insertion.
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
    auto& detached = s_unrooted[ctx];
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
            !el->isTemplateContent() &&
            el->tagName() == "#DOCUMENT-FRAGMENT") {
            auto* doc = el->document();
            JS_SetOpaque(val, nullptr);
            JS_DeleteProperty(ctx, elemMap, props[i].atom, 0);
            if (doc) doc->freeNode(el);
            JS_FreeValue(ctx, val);
            continue;
        }

        // Detached: stop rooting it. The opaque is deliberately left alone —
        // this is not a free and the element goes on working for anyone still
        // holding it. `val` keeps the wrapper alive until the end of this
        // iteration, so the collection happens in the JS_RunGC that follows
        // this sweep rather than under it.
        //
        // The document element is exempt because it is parentless by
        // construction and is the root of everything; template content because
        // it hangs off its template rather than off a parent, which is the same
        // exemption the branch above and the element finalizer both make.
        if (el && el->isAlive() && !el->parentNode() && !el->isTemplateContent() &&
            (!ctxDoc || ctxDoc->documentElement() != el)) {
            detached.insert(el);
            JS_DeleteProperty(ctx, elemMap, props[i].atom, 0);
            JS_FreeValue(ctx, val);
            continue;
        }
        JS_FreeValue(ctx, val);
    }

    // Back in a tree since the last sweep: root it again, before a collection
    // can take the listeners registered on its wrapper with it. A wrapper that
    // has already gone (jsWrapper_ null) means nothing was holding the element
    // and nothing re-inserted it from JS; there is nothing left to root and the
    // entry simply goes.
    for (auto it = detached.begin(); it != detached.end(); ) {
        bro::dom::Element* el = *it;
        if (!ctxDoc || !ctxDoc->isNodeLive(el)) { it = detached.erase(it); continue; }
        if (!el->parentNode()) { ++it; continue; }
        if (void* w = el->jsWrapper()) {
            const std::string key = std::to_string(el->nodeId());
            JS_SetPropertyStr(ctx, elemMap, key.c_str(),
                              JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, w)));
        }
        it = detached.erase(it);
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
