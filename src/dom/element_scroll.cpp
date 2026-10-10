#include "dom/element_scroll.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"

#include <algorithm>

namespace bro::dom {

namespace {

std::string declaredOverflow(const htmlayout::css::ComputedStyle& style, const char* axisProp) {
    auto it = style.find(axisProp);
    if (it != style.end() && !it->second.empty()) return it->second;
    auto o = style.find("overflow");
    return (o != style.end() && !o->second.empty()) ? o->second : std::string("visible");
}

bool spills(const std::string& ov) {
    return ov == "visible" || ov == "clip" || ov == "initial";
}

}  // namespace

std::string usedOverflow(const htmlayout::css::ComputedStyle& style, bool horizontal) {
    std::string ov = declaredOverflow(style, horizontal ? "overflow-x" : "overflow-y");
    // CSS Overflow 3 §3: when one axis scrolls, the other cannot stay
    // visible/clip — visible computes to auto and clip to hidden.
    if (spills(ov)) {
        const std::string other =
            declaredOverflow(style, horizontal ? "overflow-y" : "overflow-x");
        if (!spills(other)) ov = (ov == "clip") ? "hidden" : "auto";
    }
    return ov;
}

float maxScrollTopOf(const Element* el) {
    if (!el) return 0.0f;
    const auto& box = el->layoutBox();
    return std::max(0.0f, box.naturalHeight - box.contentRect.height);
}

float maxScrollLeftOf(const Element* el) {
    if (!el) return 0.0f;
    const auto& box = el->layoutBox();
    const float overflow = box.naturalWidth - box.contentRect.width;
    if (overflow <= 0.0f) return 0.0f;
    // Only a scroll container scrolls. Content spilling out of an
    // overflow-x:visible box (or a clip box, which forbids even programmatic
    // scrolling) is not scrolled through, so its scrollLeft stays 0. The style
    // check sits behind the cheap one above because paint asks every element.
    if (spills(usedOverflow(el->computedStyle(), true))) return 0.0f;
    return overflow;
}

// Every painted and hit-tested element asks, and almost none has scrolled:
// answer those without looking at the box or the style.
float clampedScrollTopOf(const Element* el) {
    if (!el || el->scrollTopValue() <= 0.0f) return 0.0f;
    return std::clamp(el->scrollTopValue(), 0.0f, maxScrollTopOf(el));
}

float clampedScrollLeftOf(const Element* el) {
    if (!el || el->scrollLeftValue() <= 0.0f) return 0.0f;
    return std::clamp(el->scrollLeftValue(), 0.0f, maxScrollLeftOf(el));
}

bool elementClipsOverflow(const Element* el) {
    if (!el) return false;
    const std::string ov = usedOverflow(el->computedStyle(), false);
    return ov != "visible" && ov != "initial";
}

bool elementClipsOverflowX(const Element* el) {
    if (!el) return false;
    const std::string ov = usedOverflow(el->computedStyle(), true);
    return ov != "visible" && ov != "initial";
}

void setElementScrollTop(Element* el, double v) {
    if (!el) return;
    const float maxScroll = maxScrollTopOf(el);
    const float requested = static_cast<float>(v);
    const float clamped = std::clamp(requested, 0.0f, maxScroll);
    const float prev = el->scrollTopValue();
    el->setScrollTopValue(clamped);

    if (Document* doc = el->document()) {
        // Layout is async: when script appends content and then writes the
        // classic `el.scrollTop = el.scrollHeight` in the SAME turn, the
        // just-appended nodes are not laid out yet, so both scrollHeight and
        // maxScroll above are STALE and the clamp lands short of the real
        // bottom — the container never follows the new content until a second
        // scroll (a log or transcript that "stops updating").
        //
        // A positive request at or beyond the current (stale) max means "the
        // end", so it defers to the post-layout scroll-to-bottom pass instead.
        // scrollTop = 0 is excluded deliberately: it is "scroll to top", and it
        // would otherwise read as the end on an element that is not scrollable
        // yet (max == 0). A definite mid-position cancels any pending jump.
        const bool wantsEnd = requested > 0.0f && requested >= maxScroll;
        el->setScrollToBottom(wantsEnd && doc->isDirty());
        doc->markDirty();
    }

    if (requested != prev) {
        Event evt("scroll", false, false);
        evt.setIsTrusted(true);
        dispatchDomEvent(el, evt);
    }
}

void setElementScrollLeft(Element* el, double v) {
    if (!el) return;
    const float requested = static_cast<float>(v);
    const float clamped = std::clamp(requested, 0.0f, maxScrollLeftOf(el));
    const float prev = el->scrollLeftValue();
    el->setScrollLeftValue(clamped);

    if (Document* doc = el->document()) {
        doc->markDirty();
    }

    if (clamped != prev) {
        Event evt("scroll", false, false);
        evt.setIsTrusted(true);
        dispatchDomEvent(el, evt);
    }
}

void scrollElementBy(Element* el, double delta) {
    if (!el) return;
    setElementScrollTop(el, static_cast<double>(el->scrollTopValue()) + delta);
}

void scrollElementLeftBy(Element* el, double delta) {
    if (!el) return;
    setElementScrollLeft(el, static_cast<double>(el->scrollLeftValue()) + delta);
}

}  // namespace bro::dom
