#pragma once

#include "js/dom_bindings.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/text_node.h"
#include "dom/comment_node.h"
#include "dom/shadow_root.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <cctype>
#include <cstring>

extern "C" {
#include "quickjs.h"
}

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

// ===========================================================================
// Per-context / per-runtime state (defined in dom_bindings.cpp)
// ===========================================================================

extern std::unordered_map<JSRuntime*, bool> s_classes_registered;
extern std::unordered_map<JSContext*, bro::dom::Document*> s_ctx_documents;
extern std::unordered_map<JSContext*, DomBindings::GetContextFactory> s_ctx_factories;

// SDL window pointer for pointer lock support (stored as void* to avoid SDL header dependency)
extern std::unordered_map<JSContext*, void*> s_ctx_sdl_windows;

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

// event_bindings.cpp
JSValue wrapEvent(JSContext* ctx, const std::string& type, bro::dom::Element* target);

// style_bindings.cpp
JSValue wrapStyleProxy(JSContext* ctx, bro::dom::StyleProxy* style);
JSValue wrapTokenList(JSContext* ctx, bro::dom::Element* elem);
JSValue js_window_getComputedStyle(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv);

// shadowroot_bindings.cpp
JSValue wrapShadowRoot(JSContext* ctx, bro::dom::ShadowRoot* sr);

// element_bindings.cpp
void invalidateWrapper(JSContext* ctx, bro::dom::Element* elem);
bro::dom::Element* getElement(JSValueConst val);
void cleanupCanvasContextCache(JSRuntime* rt);

// document_bindings.cpp
bro::dom::Document* getDocument(JSValueConst val);

// mutation_observer.cpp — notify MutationObservers of DOM changes
void notifyMutationObservers(JSContext* ctx, JSValueConst target,
                             const char* type,         // "childList", "attributes", "characterData"
                             const char* attributeName, // for "attributes" type, else nullptr
                             const char* oldValue,      // previous value or nullptr
                             JSValueConst addedNodes,   // JS array or JS_NULL
                             JSValueConst removedNodes); // JS array or JS_NULL

// ===========================================================================
// Per-module class registration & prototype installation
// Called by DomBindings::install() in dom_bindings.cpp
// ===========================================================================

void registerNodeClasses(JSRuntime* rt);
void installNodePrototypes(JSContext* ctx);

void registerEventClasses(JSRuntime* rt);
void installEventPrototypes(JSContext* ctx);

void registerStyleClasses(JSRuntime* rt);
void installStylePrototypes(JSContext* ctx);

void registerElementClasses(JSRuntime* rt);
void installElementPrototypes(JSContext* ctx);

void registerShadowRootClasses(JSRuntime* rt);
void installShadowRootPrototypes(JSContext* ctx);

void registerDocumentClasses(JSRuntime* rt);
void installDocumentPrototypes(JSContext* ctx);

} // namespace bro::js
