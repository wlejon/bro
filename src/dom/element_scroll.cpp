#include "dom/element_scroll.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"

#include <algorithm>

namespace bro::dom {

float maxScrollTopOf(const Element* el) {
    if (!el) return 0.0f;
    const auto& box = el->layoutBox();
    return std::max(0.0f, box.naturalHeight - box.contentRect.height);
}

bool elementClipsOverflow(const Element* el) {
    if (!el) return false;
    const auto& style = el->computedStyle();
    auto it = style.find("overflow-y");
    std::string ov = (it != style.end()) ? it->second : std::string();
    if (ov.empty()) {
        auto o = style.find("overflow");
        ov = (o != style.end()) ? o->second : "visible";
    }
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

void scrollElementBy(Element* el, double delta) {
    if (!el) return;
    setElementScrollTop(el, static_cast<double>(el->scrollTopValue()) + delta);
}

}  // namespace bro::dom
