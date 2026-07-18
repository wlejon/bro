#pragma once

#include "js/dom_bindings.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/text_node.h"
#include "dom/comment_node.h"
#include "dom/shadow_root.h"

namespace bro::dom { class Selection; class Range; }

#include <string>
#include <vector>
#include <unordered_map>
#include <cctype>
#include <cstring>

extern "C" {
#include "quickjs.h"
}

#include <qjsbind/qjsbind.h>

namespace bro::js {

// ===========================================================================
// Class IDs (defined in dom_bindings.cpp)
// ===========================================================================

extern JSClassID js_document_class_id;
extern JSClassID js_element_class_id;
extern JSClassID js_node_class_id;
extern JSClassID js_event_class_id;
extern JSClassID js_nodelist_class_id;
extern JSClassID js_cssstyle_class_id;
extern JSClassID js_computed_class_id;
extern JSClassID js_tokenlist_class_id;
extern JSClassID js_shadowroot_class_id;
extern JSClassID js_htmlcollection_class_id;
extern JSClassID js_range_class_id;
extern JSClassID js_selection_class_id;

// ===========================================================================
// Per-context / per-runtime state (defined in dom_bindings.cpp)
// ===========================================================================

extern std::unordered_map<JSContext*, bro::dom::Document*> s_ctx_documents;
extern std::unordered_map<JSContext*, DomBindings::GetContextFactory> s_ctx_factories;

// SDL window pointer for pointer lock support (stored as void* to avoid SDL header dependency)
extern std::unordered_map<JSContext*, void*> s_ctx_sdl_windows;

// Engine pointer (stored as void* to avoid including engine/engine.h here).
extern std::unordered_map<JSContext*, void*> s_ctx_engines;

bro::dom::Document* getDocumentForCtx(JSContext* ctx);

// ===========================================================================
// String conversion helpers (inline)
// ===========================================================================

/// Convert camelCase to kebab-case: "backgroundColor" -> "background-color"
inline std::string camelToKebab(const std::string& name)
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
inline std::string kebabToCamel(const std::string& name)
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

/// Convert a JSValue to a C++ std::string
inline std::string jsToStdString(JSContext* ctx, JSValueConst val)
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
// Cross-module wrapper functions
// ===========================================================================

// node_bindings.cpp
JSValue wrapNodeList(JSContext* ctx, const std::vector<bro::dom::Element*>& elems);
JSValue wrapLiveHTMLCollection(JSContext* ctx, bro::dom::Element* root,
                               bro::dom::Document* doc,
                               const std::string& selector);
JSValue wrapAnyNode(JSContext* ctx, bro::dom::Node* node);
bro::dom::Node* unwrapNode(JSContext* ctx, JSValueConst val);
// Make a freed Text/Comment node's cached wrapper inert and drop it from
// __bro_node_map. Called from fireNodeFreed for every non-element node.
void invalidateNodeWrapper(JSContext* ctx, bro::dom::Node* node);
// Re-point a cached Text/Comment wrapper's handle after adoption moved the
// node to another document. Called from fireNodeAdopted.
void repointNodeWrapper(JSContext* ctx, bro::dom::Node* node,
                        bro::dom::Document* newDoc);

// event_bindings.cpp
JSValue wrapEvent(JSContext* ctx, const std::string& type, bro::dom::Element* target);

// style_bindings.cpp
JSValue wrapStyleProxy(JSContext* ctx, bro::dom::StyleProxy* style);
JSValue wrapTokenList(JSContext* ctx, bro::dom::Element* elem);
JSValue js_window_getComputedStyle(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv);

// shadowroot_bindings.cpp
JSValue wrapShadowRoot(JSContext* ctx, bro::dom::ShadowRoot* sr);

// dom_bindings.cpp — realm that owns a document, or nullptr. Inverse of
// getDocumentForCtx; needed by hooks the DOM layer fires with only a Document*.
JSContext* getCtxForDocument(bro::dom::Document* doc);

// element_bindings.cpp
void invalidateWrapper(JSContext* ctx, bro::dom::Element* elem);
// Document::ElementClonedCallback — carries over the clone state that lives
// above the DOM layer (canvas backing store, <select> selection).
void fireElementCloned(bro::dom::Document* doc, bro::dom::Element* src,
                       bro::dom::Element* clone);
bro::dom::Element* getElement(JSValueConst val);
void cleanupCanvasContextCache(JSRuntime* rt);
// True while engine teardown is running (see setElementFinalizerShutdown) —
// finalizers must not dereference DOM pointers or touch other wrappers then.
bool isElementFinalizerShutdown();

// document_bindings.cpp
bro::dom::Document* getDocument(JSValueConst val);

// dom_bindings.cpp — detached (JS-owned) documents backing DOMParser.
// wrapDetachedDocument takes ownership of `doc` (deletes it on failure; on
// success a hidden holder on the wrapper deletes it when the wrapper is GC'd).
JSValue wrapDetachedDocument(JSContext* ctx, bro::dom::Document* doc);
bool isDetachedDocument(bro::dom::Document* doc);
// Dup'd Document wrapper for a registered detached doc, or JS_NULL.
JSValue detachedDocumentWrapper(JSContext* ctx, bro::dom::Document* doc);
// Element-wrapper-finalizer hook: drop the weak registry entry.
void dropDetachedElementWrapper(bro::dom::Element* el, void* wrapperPtr);

// mutation_observer.cpp — notify MutationObservers of DOM changes
void notifyMutationObservers(JSContext* ctx, JSValueConst target,
                             const char* type,         // "childList", "attributes", "characterData"
                             const char* attributeName, // for "attributes" type, else nullptr
                             const char* oldValue,      // previous value or nullptr
                             JSValueConst addedNodes,   // JS array or JS_NULL
                             JSValueConst removedNodes); // JS array or JS_NULL

// ===========================================================================
// Per-module binding installation (called by DomBindings::install())
// qjsbind-managed classes — handle IDs, class registration, and prototypes
// ===========================================================================

void installEventBindings(JSContext* ctx);
void installNodeBindings(JSContext* ctx);
void installDocumentBindings(JSContext* ctx);
void installShadowRootBindings(JSContext* ctx);
void installRangeBindings(JSContext* ctx);
void installSelectionBindings(JSContext* ctx);

// selection_bindings.cpp
JSValue wrapSelection(JSContext* ctx, bro::dom::Selection* s);

// ===========================================================================
// qjsbind-managed classes — element and style
// ===========================================================================

void installElementBindings(JSContext* ctx);
void installStyleBindings(JSContext* ctx);

} // namespace bro::js
