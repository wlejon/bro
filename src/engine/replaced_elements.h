#pragma once

#include "dom/element.h"
#include "dom/event.h"
#include "render/renderer.h"
#include "layout/el_select.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"

extern "C" { typedef struct JSContext JSContext; }

namespace bro::dom { class Document; }
namespace bro::js { class Runtime; }
namespace bro::platform { class Window; }

namespace bro::engine {

// ---------------------------------------------------------------------------
// Shared replaced-element initialization
// ---------------------------------------------------------------------------

/// Walk a DOM subtree and attach ElSelect / ElInput / ElTextarea / ElSvg
/// controllers to any replaced elements that don't already have one.
/// Used by both the main Engine and the SystemOverlay.
void ensureReplacedElements(dom::Element* elem, render::Renderer* renderer);

// ---------------------------------------------------------------------------
// Shared replaced-element interaction context
// ---------------------------------------------------------------------------

/// Lightweight context for replaced-element mouse interaction.
/// Abstracts away the differences between Engine (single document, scrollY,
/// window) and SystemOverlay (per-panel document, no scroll, no window).
struct ControlContext {
    dom::Document* document;
    JSContext* jsCtx;
    render::Renderer* renderer;
    platform::Window* window;   // may be nullptr (headless / overlay)
    bool* dirtyFlag;            // points to uiDirty_ or renderDirty_
};

/// Result of handling a click on a previously-active control.
enum class ClickDisposition {
    PassThrough,    // click was not consumed — continue processing
    Consumed,       // click was consumed (e.g. dropdown option selected)
    ClosedOverlay,  // an overlay (dropdown/picker) was closed, continue
};

/// Unfocus the previously-active replaced element control (close open
/// dropdowns, unfocus inputs/textareas).  If the click lands inside an
/// open dropdown or color picker, the click is consumed and the caller
/// should stop processing.
ClickDisposition unfocusPreviousControl(
    const ControlContext& ctx,
    dom::Element* prevActive,
    float x, float y);

/// Activate a newly-clicked replaced element (toggle select dropdown,
/// focus input/textarea, handle checkbox/radio/range/color/number).
void focusNewControl(
    const ControlContext& ctx,
    dom::Element* target,
    float x, float y);

/// Update select dropdown highlight based on mouse position.
/// Returns true if a dropdown consumed the hover.
bool updateDropdownHover(
    const ControlContext& ctx,
    float x, float y);

/// Draw open select dropdowns / color pickers for the active element.
void drawActiveOverlays(dom::Document* doc);

// ---------------------------------------------------------------------------
// Inline helpers
// ---------------------------------------------------------------------------

inline layout::ElInput* getElInput(dom::Element* el) {
    return el ? el->inputControl() : nullptr;
}
inline layout::ElTextarea* getElTextarea(dom::Element* el) {
    return el ? el->textareaControl() : nullptr;
}
inline layout::ElSelect* getElSelect(dom::Element* el) {
    return el ? el->selectControl() : nullptr;
}

/// Dispatch an "input" DOM event on an element.
void dispatchInputEvent(const ControlContext& ctx, dom::Element* el,
                        const std::string& data = "",
                        const std::string& inputType = "");

/// Dispatch a generic DOM event on an element.
void dispatchControlEvent(const ControlContext& ctx, dom::Element* el,
                          dom::Event& event);

/// Dispatch focus/blur/focusin/focusout events for an active-element change.
void dispatchFocusEvents(const ControlContext& ctx,
                         dom::Element* oldTarget, dom::Element* newTarget);

} // namespace bro::engine
