#pragma once

#include "dom/element.h"
#include "dom/event.h"
#include "engine/overlay.h"
#include "render/renderer.h"
#include "layout/el_select.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"

extern "C" { typedef struct JSContext JSContext; }

namespace bro::dom { class Document; }
namespace bro::js { class Runtime; }
namespace bro::platform { class Window; }
namespace broaudio { class Engine; }

namespace bro::engine {

// ---------------------------------------------------------------------------
// Shared replaced-element initialization
// ---------------------------------------------------------------------------

/// Walk a DOM subtree and attach ElSelect / ElInput / ElTextarea / ElSvg
/// controllers to any replaced elements that don't already have one.
/// Used by the Engine for both the app document and system panels.
void ensureReplacedElements(dom::Element* elem, render::Renderer* renderer,
                            JSContext* jsCtx = nullptr,
                            broaudio::Engine* audioEngine = nullptr);

// ---------------------------------------------------------------------------
// Shared replaced-element interaction context
// ---------------------------------------------------------------------------

/// Lightweight context for replaced-element mouse interaction.
/// Abstracts away the differences between Engine (single document, scrollY,
/// window) and system panels (per-panel document, no scroll).
struct ControlContext {
    dom::Document* document;
    JSContext* jsCtx;
    render::Renderer* renderer;
    platform::Window* window;   // may be nullptr (headless / overlay)
    bool* dirtyFlag;            // points to uiDirty_ or renderDirty_
    OverlayManager* overlays = nullptr;   // engine-wide overlay manager
    OverlayContext overlayContext = OverlayContext::App; // which pass draws overlays
    int viewportW = 0;         // for overlay anchoring (keep popups on-screen)
    int viewportH = 0;
};

/// Result of handling a click on a previously-active control.
enum class ClickDisposition {
    PassThrough,    // click was not consumed — continue processing
    Consumed,       // click was consumed (e.g. dropdown option selected)
};

// ---------------------------------------------------------------------------
// Per-document mouse dispatch state
// ---------------------------------------------------------------------------

/// Bookkeeping for a single document's mouse event stream: tracks the
/// mousedown target (so click only fires on matching mouseup) and rolling
/// double-click / contextmenu state. One instance per document participating
/// in input (one for the app doc, one per system panel doc).
struct MouseDispatchState {
    dom::Element* mouseDownTarget = nullptr;
    dom::Element* lastClickTarget = nullptr;
    double lastClickTimeMs = 0.0;
    float lastClickX = 0.0f;
    float lastClickY = 0.0f;
    int clickCount = 0;
};

/// Populate event.offsetX/Y as clientX/Y relative to the element's padding
/// edge (DOM spec). Walks up the layout tree adjusting for scroll.
void applyMouseOffset(dom::MouseEvent& evt, dom::Element* target);

/// Dispatch mousedown to `target` with full focus-transition semantics
/// (unfocusPreviousControl → setActiveElement + focus events → focusNewControl
/// → mousedown). Updates state.mouseDownTarget. Returns true if the press was
/// consumed by unfocusPreviousControl and callers should stop further work.
/// Caller populates `evt`'s coordinates/button fields before calling.
bool dispatchDocMousePress(
    const ControlContext& ctx,
    MouseDispatchState& state,
    dom::Element* target,
    dom::MouseEvent& evt,
    float focusX, float focusY);

/// Dispatch mouseup to `target` and, if it matches state.mouseDownTarget,
/// follow up with click / dblclick / contextmenu per DOM semantics. Clears
/// state.mouseDownTarget. Uses provided thresholds for double-click detection.
/// Caller populates `upEvt` coords/button; coords are reused for follow-ups.
void dispatchDocMouseRelease(
    const ControlContext& ctx,
    MouseDispatchState& state,
    dom::Element* target,
    dom::MouseEvent& upEvt,
    float clientX, float clientY,
    int button, int buttons, int mod,
    float movementX, float movementY,
    float pageX, float pageY,
    double nowMs,
    double dblThresholdMs,
    float dblDistPx);

/// Unfocus the previously-active replaced element control (unfocus inputs
/// and textareas). Dropdowns and color pickers now live in the overlay
/// manager, which handles click-outside dismissal itself.
ClickDisposition unfocusPreviousControl(
    const ControlContext& ctx,
    dom::Element* prevActive);

/// Activate a newly-clicked replaced element (toggle select dropdown,
/// focus input/textarea, handle checkbox/radio/range/color/number).
void focusNewControl(
    const ControlContext& ctx,
    dom::Element* target,
    float x, float y);

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
