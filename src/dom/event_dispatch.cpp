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

// One step of the event path: an element or a shadow root, and the target
// as a listener there sees it (retargeted to a host outside its tree).
struct EventPathEntry {
    Node* node = nullptr;
    Node* retargetedTarget = nullptr;
};

bool isInsideShadowRoot(const Node* n, const ShadowRoot* sr) {
    while (n && n->parentNode()) n = n->parentNode();
    return n == sr;
}

// DOM "get the parent" from the target up: an assigned node's parent is its
// slot, a shadow root's its host (unless the event is not composed and
// started inside the root, where the path ends at the root).
std::vector<EventPathEntry> buildEventPath(Node* target, const Event& event,
                                           bool& stoppedAtShadowBoundary) {
    std::vector<EventPathEntry> path;
    Node* current = target;
    Node* effectiveTarget = target;
    stoppedAtShadowBoundary = false;

    while (current) {
        path.push_back({current, effectiveTarget});
        if (auto* sr = dynamic_cast<ShadowRoot*>(current)) {
            if (!sr->host()) break;
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
        if (current->nodeType() != NodeType::Element) break;
        Node* parentNode = current->parentNode();
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
            current = parentElem;
            continue;
        }
        if (parentNode && dynamic_cast<ShadowRoot*>(parentNode)) {
            current = parentNode;
            continue;
        }
        break;
    }
    return path;
}

void setEventTarget(Event& event, Node* n) {
    if (auto* sr = dynamic_cast<ShadowRoot*>(n)) event.setTargetToRoot(sr);
    else event.setTarget(static_cast<Element*>(n));
}

void setEventCurrentTarget(Event& event, Node* n) {
    if (auto* sr = dynamic_cast<ShadowRoot*>(n)) event.setCurrentTargetToRoot(sr);
    else event.setCurrentTarget(static_cast<Element*>(n));
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

void invokeListeners(NativeListenerList* listeners, Event& event, int phase) {
    if (!listeners || event.immediatePropagationStopped()) return;
    auto& list = *listeners;
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

void invokeEntryListeners(Node* node, Event& event, int phase) {
    if (auto* sr = dynamic_cast<ShadowRoot*>(node)) {
        invokeListeners(sr->nativeListeners(), event, phase);
        return;
    }
    auto* el = static_cast<Element*>(node);
    invokeAttributeHandler(el, event, phase);
    invokeListeners(el->nativeListeners(), event, phase);
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

namespace {

void dispatchAlongPath(Node* target, Document* targetDoc, Event& event) {
    setEventTarget(event, target);
    bool stoppedAtShadowBoundary = false;
    auto path = buildEventPath(target, event, stoppedAtShadowBoundary);
    if (path.empty()) return;

    Document* doc = stoppedAtShadowBoundary ? nullptr : targetDoc;

    // What `event.target` reads as at each step of the walk. Shadow
    // encapsulation is exactly this: a listener OUTSIDE the shadow tree is
    // told the HOST was clicked, because the component's internals are not
    // its host page's business — a page that wrote
    // `if (e.target === myWidget)` must not be defeated by the widget having
    // grown a shadow root. buildEventPath already computed it per entry; this
    // is where it is applied, and the real target is restored afterwards so a
    // caller reading the event once the dispatch is over still sees it.
    Node* const outermost = path.back().retargetedTarget;

    // 1. Capture phase: window -> root -> target (exclusive)
    if (!event.propagationStopped()) {
        setEventTarget(event, outermost);
        dispatchToWindow(doc, event, /*isCapture=*/true);
    }
    for (int i = static_cast<int>(path.size()) - 1; i > 0; --i) {
        if (event.propagationStopped()) break;
        setEventTarget(event, path[i].retargetedTarget);
        setEventCurrentTarget(event, path[i].node);
        event.setEventPhase(CAPTURING_PHASE);
        invokeEntryListeners(path[i].node, event, CAPTURING_PHASE);
    }

    // 2. At-target phase
    if (!event.propagationStopped()) {
        setEventTarget(event, path[0].retargetedTarget);
        setEventCurrentTarget(event, path[0].node);
        event.setEventPhase(AT_TARGET);
        invokeEntryListeners(path[0].node, event, AT_TARGET);
    }

    // 3. Bubble phase: target parent -> root -> window
    if (event.bubbles()) {
        for (size_t i = 1; i < path.size(); ++i) {
            if (event.propagationStopped()) break;
            setEventTarget(event, path[i].retargetedTarget);
            setEventCurrentTarget(event, path[i].node);
            event.setEventPhase(BUBBLING_PHASE);
            invokeEntryListeners(path[i].node, event, BUBBLING_PHASE);
        }
        if (!event.propagationStopped()) {
            setEventTarget(event, outermost);
            dispatchToWindow(doc, event, /*isCapture=*/false);
        }
    }

    // After the walk: target stays, currentTarget and the phase are cleared
    // (DOM 2.9 dispatch, steps 12-14).
    setEventTarget(event, target);
    event.setCurrentTarget(nullptr);
    event.setEventPhase(0);
}

}  // namespace

void dispatchDomEvent(Element* target, Event& event) {
    if (!target) return;
    dispatchAlongPath(target, target->document(), event);
}

void dispatchDomEventAtRoot(ShadowRoot* root, Event& event) {
    if (!root || !root->host()) return;
    dispatchAlongPath(root, root->host()->document(), event);
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
