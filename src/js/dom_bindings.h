#pragma once

#include <functional>
#include <string>

extern "C" {
#include "quickjs.h"
}

namespace bro::dom { class Element; }

namespace bro::js {

class DomBindings {
public:
    /// Callback for element.getContext(type) — lets the engine handle canvas creation.
    using GetContextFactory = std::function<JSValue(JSContext*, bro::dom::Element*, const std::string&)>;
    static void setGetContextFactory(JSContext* ctx, GetContextFactory factory);
    /// Register all DOM classes (Document, Element, Event, CSSStyleDeclaration,
    /// NodeList) and set the global `document` property wrapping the given
    /// Document pointer.
    static void install(JSContext* ctx, void* document_ptr);

    /// Set SDL window for pointer lock support.
    static void setSDLWindow(JSContext* ctx, void* sdl_window);

    /// Set engine pointer so bindings can call engine APIs (e.g. pointer lock).
    static void setEngine(JSContext* ctx, void* engine);

    // -----------------------------------------------------------------------
    // Helpers exposed so other modules can wrap / unwrap DOM objects.
    // -----------------------------------------------------------------------

    /// Wrap a bro::dom::Element* into a JS Element object.
    static JSValue wrapElement(JSContext* ctx, void* element_ptr);

    /// Unwrap a JS Element object back to bro::dom::Element*.
    static void* unwrapElement(JSContext* ctx, JSValueConst val);

    /// Wrap a bro::dom::Document* into a JS Document object.
    static JSValue wrapDocument(JSContext* ctx, void* document_ptr);

    /// Free per-context state. Call before destroying the JSContext.
    static void cleanup(JSContext* ctx);

    /// Free per-runtime state. Call before destroying the JSRuntime.
    static void cleanupRuntime(JSRuntime* rt);

    /// Get the QuickJS class ID used for Element wrappers.
    static JSClassID elementClassId();

    /// Sweep __bro_elem_map: remove entries for orphaned elements (no parent,
    /// not document root) so their JS wrappers can be GC'd.
    static void sweepOrphanedWrappers(JSContext* ctx);
};

/// Set shutdown flag so element finalizers skip dereferencing Element pointers
/// that may already be freed. Call with true before teardown GC passes.
void setElementFinalizerShutdown(bool shutting_down);

/// Run interactive form submission: validates controls, fires 'invalid' on
/// failures or 'submit' (SubmitEvent) on the form. Used by the click
/// handler when a submit button is activated. `submitter` may be null.
void requestFormSubmit(JSContext* ctx, bro::dom::Element* form,
                       bro::dom::Element* submitter);

} // namespace bro::js
