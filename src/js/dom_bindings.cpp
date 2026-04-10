#include "js/dom_bindings.h"
#include "js/dom_bindings_internal.h"
#include "js/custom_elements.h"
#include "dom/document.h"

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

// ===========================================================================
// Per-context state
// ===========================================================================

std::unordered_map<JSContext*, bro::dom::Document*> s_ctx_documents;
std::unordered_map<JSContext*, DomBindings::GetContextFactory> s_ctx_factories;
std::unordered_map<JSContext*, void*> s_ctx_sdl_windows;

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

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (JS_IsUndefined(elemMap)) {
        elemMap = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__bro_elem_map", JS_DupValue(ctx, elemMap));
    }

    std::string key = std::to_string(elem->nodeId());
    JSValue existing = JS_GetPropertyStr(ctx, elemMap, key.c_str());
    if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) {
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

    JS_SetPropertyStr(ctx, elemMap, key.c_str(), JS_DupValue(ctx, obj));

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

void DomBindings::cleanup(JSContext* ctx) {
    s_ctx_documents.erase(ctx);
    s_ctx_factories.erase(ctx);
    s_ctx_sdl_windows.erase(ctx);
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
