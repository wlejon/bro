#include "dom/event_dispatch.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event_target.h"
#include "dom/event.h"
#include "dom/shadow_root.h"

namespace bro::dom {

namespace {

constexpr int CAPTURING_PHASE = 1;
constexpr int AT_TARGET = 2;
constexpr int BUBBLING_PHASE = 3;

struct EventPathEntry {
    Element* element = nullptr;
    Element* retargetedTarget = nullptr;
};

std::vector<EventPathEntry> buildEventPath(Element* target) {
    std::vector<EventPathEntry> path;
    Element* current = target;
    Element* effectiveTarget = target;

    while (current) {
        path.push_back({current, effectiveTarget});
        auto* parentNode = current->parentNode();
        if (parentNode && parentNode->nodeType() == NodeType::DocumentFragment) {
            auto* sr = dynamic_cast<ShadowRoot*>(parentNode);
            if (sr && sr->host()) {
                effectiveTarget = sr->host();
                current = sr->host();
                continue;
            }
        }
        current = current->parentElement();
    }
    return path;
}

void invokeElementListeners(Element* el, Event& event, int phase) {
    if (!el || !el->nativeListeners()) return;
    auto& list = *el->nativeListeners();
    auto snapshot = list.snapshot(event.type());
    for (auto& entry : snapshot) {
        if (event.immediatePropagationStopped()) break;
        if (!entry || entry->removed || !entry->cb) continue;
        const bool capture = entry->opts.capture;
        const bool runs = (phase == AT_TARGET) ||
                          (phase == CAPTURING_PHASE && capture) ||
                          (phase == BUBBLING_PHASE && !capture);
        if (!runs) continue;
        if (entry->opts.once) list.remove(ListenerHandle{entry->id});
        entry->cb(event);
    }
}

void dispatchToWindow(Document* doc, Event& event, bool isCapture) {
    if (!doc) return;
    auto& list = doc->windowListeners();
    auto snapshot = list.snapshot(event.type());
    for (auto& entry : snapshot) {
        if (event.immediatePropagationStopped()) break;
        if (!entry || entry->removed || !entry->cb) continue;
        if (entry->opts.capture != isCapture) continue;
        if (entry->opts.once) list.remove(ListenerHandle{entry->id});
        entry->cb(event);
    }
}

} // namespace

void dispatchDomEvent(Element* target, Event& event) {
    if (!target) return;

    event.setTarget(target);
    auto path = buildEventPath(target);
    if (path.empty()) return;

    Document* doc = target->document();

    // What `event.target` reads as at each step of the walk. Shadow
    // encapsulation is exactly this: a listener OUTSIDE the shadow tree is
    // told the HOST was clicked, because the component's internals are not
    // its host page's business — a page that wrote
    // `if (e.target === myWidget)` must not be defeated by the widget having
    // grown a shadow root. buildEventPath already computed it per entry; this
    // is where it is applied, and the real target is restored afterwards so a
    // caller reading the event once the dispatch is over still sees it.
    Element* const realTarget = target;
    Element* const outermost = path.back().retargetedTarget;

    // 1. Capture phase: window -> root -> target (exclusive)
    if (!event.propagationStopped()) {
        event.setTarget(outermost);
        dispatchToWindow(doc, event, /*isCapture=*/true);
    }
    for (int i = static_cast<int>(path.size()) - 1; i > 0; --i) {
        if (event.propagationStopped()) break;
        event.setTarget(path[i].retargetedTarget);
        event.setCurrentTarget(path[i].element);
        event.setEventPhase(CAPTURING_PHASE);
        invokeElementListeners(path[i].element, event, CAPTURING_PHASE);
    }

    // 2. At-target phase
    if (!event.propagationStopped()) {
        event.setTarget(path[0].retargetedTarget);
        event.setCurrentTarget(path[0].element);
        event.setEventPhase(AT_TARGET);
        invokeElementListeners(path[0].element, event, AT_TARGET);
    }

    // 3. Bubble phase: target parent -> root -> window
    if (event.bubbles()) {
        for (size_t i = 1; i < path.size(); ++i) {
            if (event.propagationStopped()) break;
            event.setTarget(path[i].retargetedTarget);
            event.setCurrentTarget(path[i].element);
            event.setEventPhase(BUBBLING_PHASE);
            invokeElementListeners(path[i].element, event, BUBBLING_PHASE);
        }
        if (!event.propagationStopped()) {
            event.setTarget(outermost);
            dispatchToWindow(doc, event, /*isCapture=*/false);
        }
    }

    event.setTarget(realTarget);
}

void dispatchWindowEvent(Document* doc, Event& event) {
    if (!doc) return;
    auto& list = doc->windowListeners();
    auto snapshot = list.snapshot(event.type());
    for (auto& entry : snapshot) {
        if (event.immediatePropagationStopped()) break;
        if (!entry || entry->removed || !entry->cb) continue;
        if (entry->opts.once) list.remove(ListenerHandle{entry->id});
        entry->cb(event);
    }
}

} // namespace bro::dom
