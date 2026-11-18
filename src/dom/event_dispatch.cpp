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

bool isInsideShadowRoot(const Element* el, const ShadowRoot* sr) {
    const Node* n = el;
    while (n && n->parentNode()) n = n->parentNode();
    return n == sr;
}

std::vector<EventPathEntry> buildEventPath(Element* target, const Event& event,
                                           bool& stoppedAtShadowBoundary) {
    std::vector<EventPathEntry> path;
    Element* current = target;
    Element* effectiveTarget = target;
    stoppedAtShadowBoundary = false;

    while (current) {
        path.push_back({current, effectiveTarget});
        auto* parentNode = current->parentNode();
        if (parentNode && parentNode->nodeType() == NodeType::Element) {
            auto* parentElem = static_cast<Element*>(parentNode);
            if (parentElem->hasShadow()) {
                if (auto* sr = parentElem->shadowRoot()) {
                    if (auto* slot = sr->assignedSlot(current)) {
                        current = slot;
                        continue;
                    }
                }
            }
        }
        if (parentNode && parentNode->nodeType() == NodeType::DocumentFragment) {
            auto* sr = dynamic_cast<ShadowRoot*>(parentNode);
            if (sr && sr->host()) {
                if (isInsideShadowRoot(effectiveTarget, sr)) {
                    if (!event.composed()) {
                        stoppedAtShadowBoundary = true;
                        break;
                    }
                    effectiveTarget = sr->host();
                }
                current = sr->host();
                continue;
            }
        }
        current = current->parentElement();
    }
    return path;
}

AttributeHandlerHook g_attributeHandlerHook = nullptr;

void invokeAttributeHandler(Element* el, Event& event, int phase) {
    if (!g_attributeHandlerHook || phase == CAPTURING_PHASE || el->attributes().empty()) return;
    const std::string& type = event.type();
    for (const auto& kv : el->attributes()) {
        const std::string& name = kv.first;
        if (name.size() == type.size() + 2 && name[0] == 'o' && name[1] == 'n' &&
            name.compare(2, std::string::npos, type) == 0) {
            g_attributeHandlerHook(el, event);
            return;
        }
    }
}

void invokeElementListeners(Element* el, Event& event, int phase) {
    if (!el) return;
    invokeAttributeHandler(el, event, phase);
    if (!el->nativeListeners() || event.immediatePropagationStopped()) return;
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
    // The window is the outermost entry of the path: it is the current
    // target for its own listeners, in the phase the walk is in.
    event.setCurrentTargetToWindow();
    event.setEventPhase(isCapture ? CAPTURING_PHASE : BUBBLING_PHASE);
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

void setAttributeHandlerHook(AttributeHandlerHook hook) {
    g_attributeHandlerHook = hook;
}

void dispatchDomEvent(Element* target, Event& event) {
    if (!target) return;

    event.setTarget(target);
    bool stoppedAtShadowBoundary = false;
    auto path = buildEventPath(target, event, stoppedAtShadowBoundary);
    if (path.empty()) return;

    Document* doc = stoppedAtShadowBoundary ? nullptr : target->document();

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

    // After the walk: target stays, currentTarget and the phase are cleared
    // (DOM 2.9 dispatch, steps 12-14).
    event.setTarget(realTarget);
    event.setCurrentTarget(nullptr);
    event.setEventPhase(0);
}

void dispatchWindowEvent(Document* doc, Event& event) {
    if (!doc) return;
    // An event dispatched AT the window: the window is its target and its
    // current target, and every listener, capture or not, runs at-target.
    event.setTargetToWindow();
    event.setCurrentTargetToWindow();
    event.setEventPhase(AT_TARGET);
    auto& list = doc->windowListeners();
    auto snapshot = list.snapshot(event.type());
    for (auto& entry : snapshot) {
        if (event.immediatePropagationStopped()) break;
        if (!entry || entry->removed || !entry->cb) continue;
        if (entry->opts.once) list.remove(ListenerHandle{entry->id});
        entry->cb(event);
    }
    event.setCurrentTarget(nullptr);
    event.setEventPhase(0);
}

void advanceDocumentReadiness(Document* doc, const char* state) {
    if (!doc || doc->readyState() == state) return;
    doc->setReadyState(state);
    // The document's own listeners are held by its root element (the host's
    // `document` event target), so the event is dispatched there; it does
    // not bubble, so it reaches the window by no path.
    if (Element* root = doc->documentElement()) {
        Event change("readystatechange", /*bubbles=*/false, /*cancelable=*/false);
        dispatchDomEvent(root, change);
    }
}

void completeDocumentLoad(Document* doc) {
    if (!doc || doc->readyState() == "complete") return;
    advanceDocumentReadiness(doc, "interactive");
    if (Element* root = doc->documentElement()) {
        Event loaded("DOMContentLoaded", /*bubbles=*/true, /*cancelable=*/false);
        dispatchDomEvent(root, loaded);
    }
    advanceDocumentReadiness(doc, "complete");
    // `load` is fired at the window only: no listener on the document or an
    // element hears it, as on the web.
    Event load("load", /*bubbles=*/false, /*cancelable=*/false);
    dispatchWindowEvent(doc, load);
}

} // namespace bro::dom
