#include "engine/drag_drop.h"

#include "dom/element.h"
#include "dom/event.h"
#include "engine/replaced_elements.h"
#include "js/event_dispatch.h"

#include <cmath>
#include <string>

namespace bro::engine {

namespace {

// How far the pointer must travel before a press becomes a drag. Below this a
// press-and-release is a click, and a hand that shakes on the button does not
// start dragging things around.
constexpr float kDragThreshold = 4.0f;

/// The nearest ancestor (self included) marked draggable. `draggable` is a
/// real attribute, not a boolean one: only "true" enables it, and "false"
/// turns it off for a subtree that would otherwise inherit nothing anyway.
dom::Element* draggableAncestor(dom::Element* el) {
    for (auto* e = el; e; e = e->parentElement()) {
        if (!e->hasAttribute("draggable")) continue;
        std::string v = e->getAttribute("draggable");
        for (char& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (v == "true" || v.empty()) return e;
        if (v == "false") return nullptr;
    }
    return nullptr;
}

/// Create the session's DataTransfer and park it where the binding layer
/// looks for it. One object for the whole gesture — see DragEvent::isSessionDrag.
void openDataTransfer(JSContext* ctx) {
    if (!ctx) return;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue factory = JS_GetPropertyStr(ctx, global, "__bro_newDataTransfer");
    JSValue dt = JS_UNDEFINED;
    if (JS_IsFunction(ctx, factory)) {
        dt = JS_Call(ctx, factory, JS_UNDEFINED, 0, nullptr);
        if (JS_IsException(dt)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            dt = JS_UNDEFINED;
        }
    }
    JS_FreeValue(ctx, factory);
    if (JS_IsUndefined(dt)) dt = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global, "__bro_dragDataTransfer", dt);
    JS_FreeValue(ctx, global);
}

void closeDataTransfer(JSContext* ctx) {
    if (!ctx) return;
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__bro_dragDataTransfer", JS_UNDEFINED);
    JS_FreeValue(ctx, global);
}

/// Dispatch one drag event and report whether its default was prevented,
/// which is how a target says "you may drop here".
bool fireDrag(JSContext* ctx, dom::Element* target, const char* type,
              float x, float y) {
    if (!ctx || !target) return false;
    dom::DragEvent evt(type, /*bubbles=*/true, /*cancelable=*/true);
    evt.setSessionDrag(true);
    evt.setIsTrusted(true);
    evt.setClientX(x);   evt.setClientY(y);
    evt.setScreenX(x);   evt.setScreenY(y);
    evt.setPageX(x);     evt.setPageY(y);
    // offsetX/offsetY relative to the target. Drop handlers lean on it hard —
    // "did this land on the top quarter, the middle, or the bottom quarter?"
    // is how a tree decides between reordering and reparenting.
    applyMouseOffset(evt, target);
    js::dispatchDomEvent(ctx, target, evt);
    return evt.defaultPrevented();
}

} // namespace

void DragDrop::arm(dom::Element* target, float x, float y) {
    candidate_.reset();
    armed_ = false;
    if (!target) return;
    dom::Element* src = draggableAncestor(target);
    if (!src) return;
    candidate_.assign(src->document(), src);
    startX_ = x;
    startY_ = y;
    armed_ = true;
}

bool DragDrop::update(JSContext* ctx, dom::Element* under, float x, float y,
                      int buttons) {
    // The button went away without a mouseup we saw (focus loss, a modal):
    // end the gesture rather than dragging forever.
    if (active_ && (buttons & 1) == 0) {
        cancel(ctx);
        return false;
    }

    if (!active_) {
        if (!armed_) return false;
        if (std::abs(x - startX_) < kDragThreshold &&
            std::abs(y - startY_) < kDragThreshold)
            return false;
        dom::Element* src = candidate_.get();
        armed_ = false;
        candidate_.reset();
        if (!src) return false;

        openDataTransfer(ctx);
        // A source that cancels dragstart refuses to be dragged.
        if (fireDrag(ctx, src, "dragstart", x, y)) {
            closeDataTransfer(ctx);
            return false;
        }
        source_.assign(src->document(), src);
        target_.reset();
        dropAllowed_ = false;
        active_ = true;
    }

    if (dom::Element* src = source_.get())
        fireDrag(ctx, src, "drag", x, y);

    dom::Element* prev = target_.get();
    if (under != prev) {
        // dragleave first, so a handler that clears its highlight runs before
        // the next element sets one.
        if (prev) fireDrag(ctx, prev, "dragleave", x, y);
        if (under) {
            // dragenter is the first chance to accept the drop, and a target
            // that accepts there without repeating it on every dragover is
            // common enough to honour.
            dropAllowed_ = fireDrag(ctx, under, "dragenter", x, y);
            target_.assign(under->document(), under);
        } else {
            target_.reset();
            dropAllowed_ = false;
        }
    }

    if (dom::Element* t = target_.get()) {
        // Each dragover re-answers the question: a target may accept only part
        // of itself, or change its mind as the pointer moves across it.
        if (fireDrag(ctx, t, "dragover", x, y)) dropAllowed_ = true;
    }
    return true;
}

bool DragDrop::finish(JSContext* ctx, dom::Element* under, float x, float y) {
    armed_ = false;
    candidate_.reset();
    if (!active_) return false;

    dom::Element* t = under ? under : target_.get();
    if (dropAllowed_ && t) fireDrag(ctx, t, "drop", x, y);

    if (dom::Element* src = source_.get()) fireDrag(ctx, src, "dragend", x, y);

    source_.reset();
    target_.reset();
    active_ = false;
    dropAllowed_ = false;
    closeDataTransfer(ctx);
    return true;
}

void DragDrop::cancel(JSContext* ctx) {
    armed_ = false;
    candidate_.reset();
    if (!active_) return;
    if (dom::Element* prev = target_.get()) fireDrag(ctx, prev, "dragleave", 0, 0);
    if (dom::Element* src = source_.get()) fireDrag(ctx, src, "dragend", 0, 0);
    source_.reset();
    target_.reset();
    active_ = false;
    dropAllowed_ = false;
    closeDataTransfer(ctx);
}

} // namespace bro::engine
