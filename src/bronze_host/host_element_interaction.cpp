#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_realm_scope.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "engine/engine.h"
#include "platform/sdl_window.h"

namespace bro::bronze_host {

void decorateElementInteraction(ObjectBuilder& b) {
    // ---- pointer lock -----------------------------------------------------
    b.def("requestPointerLock", 0, [](Value self_, std::span<const Value>) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (st && st->el) {
            auto* e = hostEngine();
            if (isChildRealm() || (e && st->el->document() != e->document())) {
                return ev::throwTypeError("requestPointerLock is only available in the main window");
            }
            if (e) e->requestPointerLock(st->el);
        }
        return ev::undefined();
    });

    // ---- fullscreen -------------------------------------------------------
    b.def("requestFullscreen", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (st && st->el) {
            setHostFullscreenElement(st->el);
        }
        if (auto* e = hostEngine()) {
            e->setFullscreenState(true);
            if (auto* win = e->window()) {
                win->setFullscreen(true);
            }
        }
        ev::Persistent p{ev::createPromise()};
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    // ---- focus & blur -----------------------------------------------------
    b.def("focus", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        dom::Document* doc = st->el->document();
        if (!doc) return ev::undefined();
        dom::Element* prev = doc->activeElement();
        if (prev == st->el) return ev::undefined();
        if (auto* eng = hostEngine()) eng->handleProgrammaticFocus(doc, prev, st->el);
        doc->setActiveElement(st->el);

        if (prev) {
            dom::FocusEvent blurEvt("blur", false, false);
            blurEvt.setRelatedTarget(st->el);
            dom::dispatchDomEvent(prev, blurEvt);
        }
        if (!st->el) return ev::undefined();
        {
            dom::FocusEvent focusEvt("focus", false, false);
            focusEvt.setRelatedTarget(prev);
            dom::dispatchDomEvent(st->el, focusEvt);
        }
        if (!st->el) return ev::undefined();
        if (prev) {
            dom::FocusEvent focusoutEvt("focusout", true, false);
            focusoutEvt.setRelatedTarget(st->el);
            dom::dispatchDomEvent(prev, focusoutEvt);
        }
        if (!st->el) return ev::undefined();
        {
            dom::FocusEvent focusinEvt("focusin", true, false);
            focusinEvt.setRelatedTarget(prev);
            dom::dispatchDomEvent(st->el, focusinEvt);
        }
        return ev::undefined();
    });

    b.def("blur", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        dom::Document* doc = st->el->document();
        if (!doc || doc->activeElement() != st->el) return ev::undefined();
        if (auto* eng = hostEngine()) eng->handleProgrammaticFocus(doc, st->el, nullptr);
        doc->setActiveElement(nullptr);

        {
            dom::FocusEvent blurEvt("blur", false, false);
            blurEvt.setRelatedTarget(nullptr);
            dom::dispatchDomEvent(st->el, blurEvt);
        }
        {
            dom::FocusEvent focusoutEvt("focusout", true, false);
            focusoutEvt.setRelatedTarget(nullptr);
            dom::dispatchDomEvent(st->el, focusoutEvt);
        }
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
