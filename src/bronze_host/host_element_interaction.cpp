#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_realm_scope.h"
#include "bronze_host/host_web_animations.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "dom/node_handle.h"
#include "engine/engine.h"
#include "platform/sdl_window.h"

namespace bro::bronze_host {

void hostFocusElement(dom::Element* target) {
    if (!target) return;
    dom::Document* doc = target->document();
    if (!doc) return;
    // Outside a modal dialog the page is inert, and an inert element is not
    // focusable: focus() on it does nothing.
    if (doc->isInert(target)) return;
    dom::Element* prev = doc->activeElement();
    if (prev == target) return;
    // Handles, not raw pointers: a listener below may remove and free either.
    dom::ElementHandle el(doc, target);
    dom::ElementHandle prevH(doc, prev);
    if (auto* eng = hostEngine()) eng->handleProgrammaticFocus(doc, prev, target);
    doc->setActiveElement(target);

    if (prevH.get()) {
        dom::FocusEvent blurEvt("blur", false, false);
        blurEvt.setRelatedTarget(el.get());
        dom::dispatchDomEvent(prevH.get(), blurEvt);
    }
    if (!el.get()) return;
    {
        dom::FocusEvent focusEvt("focus", false, false);
        focusEvt.setRelatedTarget(prevH.get());
        dom::dispatchDomEvent(el.get(), focusEvt);
    }
    if (!el.get()) return;
    if (prevH.get()) {
        dom::FocusEvent focusoutEvt("focusout", true, false);
        focusoutEvt.setRelatedTarget(el.get());
        dom::dispatchDomEvent(prevH.get(), focusoutEvt);
    }
    if (!el.get()) return;
    {
        dom::FocusEvent focusinEvt("focusin", true, false);
        focusinEvt.setRelatedTarget(prevH.get());
        dom::dispatchDomEvent(el.get(), focusinEvt);
    }
}

void decorateElementInteraction(ObjectBuilder& b) {
    decorateElementWebAnimations(b);

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
        hostFocusElement(st->el);
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
