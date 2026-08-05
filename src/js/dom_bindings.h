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

    /// The engine pointer registered for `ctx` by setEngine, or nullptr.
    /// void* for the same reason setEngine takes one — this header must not
    /// pull in engine.h. bro::engine::engineForContext() is the typed reader.
    static void* engineFor(JSContext* ctx);

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

/// The control a <label> labels: its [for] target when that names a labelable
/// element, otherwise the label's first labelable descendant in tree order.
/// Returns null for a label that labels nothing (or a non-label element).
bro::dom::Element* labeledControlFor(bro::dom::Element* label);

/// HTML's label activation behavior. Given the element a click landed on, walk
/// up to the nearest enclosing <label> and, if the click did NOT already land on
/// that label's control (or on other interactive content inside the label),
/// activate the control: focus it and run its click default actions, so clicking
/// a label's TEXT ticks its checkbox.
///
/// Does nothing and returns false when there is no label, no control, or the
/// click already went to the control — which is what keeps a direct click on a
/// wrapped checkbox from toggling twice and cancelling itself out.
bool forwardLabelActivation(JSContext* ctx, bro::dom::Element* clickTarget);

/// Dispatch a trusted click on `el` and run its default actions (button submit,
/// checkbox/radio toggle + change/input, <summary> toggle). This is the one
/// implementation behind element.click(), the hit-tested click path's defaults,
/// and label forwarding. It deliberately does NOT forward label activation, so
/// activating a control inside a label cannot recurse back into the label.
void activateElement(JSContext* ctx, bro::dom::Element* el);

/// Uncheck every other member of `el`'s radio button group: same tree, same form
/// owner, type=radio, same non-empty name. HTML requires this whenever a radio's
/// checkedness becomes true "for whatever reason" — a click, element.click(), or
/// an assignment to .checked — since the group is what makes several radios
/// behave as one control. A nameless radio is a group of one and is left alone.
///
/// Does not touch `el` itself and fires no events; the caller sets the checked
/// state and decides whether the change was user-initiated.
void clearRadioGroup(bro::dom::Element* el);

} // namespace bro::js
