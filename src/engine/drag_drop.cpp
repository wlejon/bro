#include "engine/drag_drop.h"

#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "engine/replaced_elements.h"

#include <cmath>
#include <string>

namespace bro::engine {

namespace {

constexpr float kDragThreshold = 4.0f;

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

bool fireDrag(dom::Element* target, const char* type, float x, float y) {
    if (!target) return false;
    dom::DragEvent evt(type, /*bubbles=*/true, /*cancelable=*/true);
    evt.setSessionDrag(true);
    evt.setIsTrusted(true);
    evt.setClientX(x);   evt.setClientY(y);
    evt.setScreenX(x);   evt.setScreenY(y);
    evt.setPageX(x);     evt.setPageY(y);
    applyMouseOffset(evt, target);
    dom::dispatchDomEvent(target, evt);
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

bool DragDrop::update(dom::Element* under, float x, float y, int buttons) {
    if (active_ && (buttons & 1) == 0) {
        cancel();
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

        if (fireDrag(src, "dragstart", x, y)) {
            return false;
        }
        source_.assign(src->document(), src);
        target_.reset();
        dropAllowed_ = false;
        active_ = true;
    }

    if (dom::Element* src = source_.get())
        fireDrag(src, "drag", x, y);

    dom::Element* prev = target_.get();
    if (under != prev) {
        if (prev) fireDrag(prev, "dragleave", x, y);
        if (under) {
            dropAllowed_ = fireDrag(under, "dragenter", x, y);
            target_.assign(under->document(), under);
        } else {
            target_.reset();
            dropAllowed_ = false;
        }
    }

    if (dom::Element* t = target_.get()) {
        if (fireDrag(t, "dragover", x, y)) dropAllowed_ = true;
    }
    return true;
}

bool DragDrop::finish(dom::Element* under, float x, float y) {
    armed_ = false;
    candidate_.reset();
    if (!active_) return false;

    dom::Element* t = under ? under : target_.get();
    if (dropAllowed_ && t) fireDrag(t, "drop", x, y);

    if (dom::Element* src = source_.get()) fireDrag(src, "dragend", x, y);

    source_.reset();
    target_.reset();
    active_ = false;
    dropAllowed_ = false;
    return true;
}

void DragDrop::cancel() {
    armed_ = false;
    candidate_.reset();
    if (!active_) return;
    if (dom::Element* prev = target_.get()) fireDrag(prev, "dragleave", 0, 0);
    if (dom::Element* src = source_.get()) fireDrag(src, "dragend", 0, 0);
    source_.reset();
    target_.reset();
    active_ = false;
    dropAllowed_ = false;
}

} // namespace bro::engine
