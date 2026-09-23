// HTMLDialogElement: `open`, `returnValue`, `show()`, `showModal()`,
// `close(returnValue)`, `requestClose(returnValue)` and the `close` / `cancel`
// events (HTML §4.11.4).
//
// `open` reflects the attribute, and the UA stylesheet (default_styles.h)
// hides a dialog without it, so opening and closing is attribute work. The
// `close` event is queued as a task, not fired inside close(): on the web a
// listener runs after the code that closed the dialog has finished.
//
// showModal() puts the dialog in its document's top layer as a modal entry
// (dom::Document::addToTopLayer): it paints above everything over its
// ::backdrop, matches :modal (fixed and centred by the UA sheet), and the rest
// of the page is inert — not hit-testable, not focusable. Focus moves into
// the dialog (the dialog focusing steps) and returns to where it was on
// close. Escape is a close request (Engine::requestTopLayerClose), answered
// here: a cancelable `cancel`, then close. Losing `open` by any other route
// (removeAttribute, `open = false`) or leaving the document takes the dialog
// out of the top layer too (Document::pruneTopLayer / notifyNodeRemoved).

#include "bronze_host/host_template.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/node_handle.h"
#include "engine/engine.h"

#include <unordered_map>

namespace bro::bronze_host {

namespace {

// The element focused before each modal dialog opened, restored on close.
std::unordered_map<const dom::Element*, dom::ElementHandle>& previouslyFocused() {
    static std::unordered_map<const dom::Element*, dom::ElementHandle> m;
    return m;
}

HostNodeState* dialogState(Value self) {
    HostNodeState* st = hostNodeStateOfValue(self);
    return (st && st->el) ? st : nullptr;
}

bool isConnected(dom::Element* el) {
    dom::Document* doc = el->document();
    if (!doc) return false;
    dom::Element* root = doc->documentElement();
    for (dom::Element* cur = el; cur; cur = cur->parentElement()) {
        if (cur == root) return true;
    }
    return false;
}

// Modal = opened with showModal() and still in the top layer as such.
bool isModal(HostNodeState* st) {
    dom::Document* doc = st->el->document();
    return st->dialogModal && doc && doc->isInTopLayer(st->el);
}

// Fire a simple, non-bubbling event at the dialog behind `wrapper`. The
// wrapper, not the element, is what the caller holds across the task: while
// it is rooted the element cannot be reclaimed.
bool fireSimple(const ev::Persistent& wrapper, const char* type, bool cancelable) {
    HostNodeState* st = hostNodeStateOfValue(wrapper.get());
    engine::Engine* eng = hostEngine();
    if (!st || !st->el || !eng) return true;
    dom::Event evt(type, /*bubbles=*/false, cancelable);
    evt.setIsTrusted(true);
    eng->dispatchElementEvent(st->el, evt);
    return !evt.defaultPrevented();
}

// Leave the top layer (if in it) and give focus back.
void leaveTopLayer(dom::Element* el) {
    dom::Document* doc = el->document();
    auto& prev = previouslyFocused();
    auto it = prev.find(el);
    dom::Element* restore = nullptr;
    if (it != prev.end()) {
        restore = it->second.get();
        prev.erase(it);
    }
    if (doc) doc->removeFromTopLayer(el);
    if (restore) hostFocusElement(restore);
}

void closeDialog(Value self, HostNodeState* st, std::span<const Value> a, size_t resultIndex) {
    if (!st->el->hasAttribute("open")) return;
    ev::Persistent wrapper(self);
    Value result = argAt(a, resultIndex);
    if (!ev::isUndefined(result)) st->dialogReturnValue = ev::toUtf8(result);
    st->el->removeAttribute("open");
    st->dialogModal = false;
    leaveTopLayer(st->el);
    postHostTask([wrapper]() { fireSimple(wrapper, "close", false); });
}

Value invalidState(const std::string& message) {
    return ev::throwValue(hostMakeDomError("InvalidStateError", message));
}

bool isFocusableControl(dom::Element* el) {
    const std::string& tag = el->tagName();
    if (el->hasAttribute("disabled") &&
        (tag == "INPUT" || tag == "TEXTAREA" || tag == "SELECT" || tag == "BUTTON")) {
        return false;
    }
    if (tag == "INPUT") return el->getAttribute("type") != "hidden";
    if (tag == "TEXTAREA" || tag == "SELECT" || tag == "BUTTON") return true;
    if (tag == "A" && el->hasAttribute("href")) return true;
    return el->hasAttribute("tabindex");
}

// HTML's dialog focusing steps: the first descendant with `autofocus`, else
// the first focusable descendant, else the dialog itself.
void runDialogFocusingSteps(dom::Element* dialog) {
    dom::Element* autofocus = nullptr;
    dom::Element* firstFocusable = nullptr;
    std::vector<dom::Node*> stack;
    const auto& kids = dialog->childNodes();
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back(*it);
    while (!stack.empty() && !autofocus) {
        dom::Node* n = stack.back();
        stack.pop_back();
        if (n->nodeType() != dom::NodeType::Element) continue;
        auto* el = static_cast<dom::Element*>(n);
        if (el->hasAttribute("autofocus")) autofocus = el;
        else if (!firstFocusable && isFocusableControl(el)) firstFocusable = el;
        const auto& cs = el->childNodes();
        for (auto it = cs.rbegin(); it != cs.rend(); ++it) stack.push_back(*it);
    }
    dom::Element* target = autofocus ? autofocus : firstFocusable ? firstFocusable : dialog;
    hostFocusElement(target);
}

// Escape's close request, offered to each top-layer entry from the top.
bool dialogCloseRequest(dom::Element* el) {
    if (!el || el->tagName() != "DIALOG" || !el->hasAttribute("open")) return false;
    ev::Persistent wrapper(hostElementValue(el));
    if (!fireSimple(wrapper, "cancel", true)) return true;
    HostNodeState* st = dialogState(wrapper.get());
    if (st) closeDialog(wrapper.get(), st, {}, 0);
    return true;
}

}  // namespace

void installDialogHooks(engine::Engine& engine) {
    previouslyFocused().clear();
    engine.setTopLayerCloseRequestHandler(&dialogCloseRequest);
}

void decorateDialogProto(ObjectBuilder& b) {
    b.accessor(
        "open",
        [](Value self, std::span<const Value>) -> Value {
            HostNodeState* st = dialogState(self);
            return ev::fromBool(st && st->el->hasAttribute("open"));
        },
        [](Value self, std::span<const Value> a) -> Value {
            HostNodeState* st = dialogState(self);
            if (!st) return ev::undefined();
            if (ev::toBool(argAt(a, 0))) {
                st->el->setAttribute("open", "");
            } else {
                st->el->removeAttribute("open");
                st->dialogModal = false;
                leaveTopLayer(st->el);
            }
            return ev::undefined();
        });

    b.accessor(
        "returnValue",
        [](Value self, std::span<const Value>) -> Value {
            HostNodeState* st = dialogState(self);
            return ev::fromUtf8(st ? st->dialogReturnValue : std::string());
        },
        [](Value self, std::span<const Value> a) -> Value {
            HostNodeState* st = dialogState(self);
            if (st) st->dialogReturnValue = hostNullableString(argAt(a, 0));
            return ev::undefined();
        });

    b.def("show", 0, [](Value self, std::span<const Value>) -> Value {
        HostNodeState* st = dialogState(self);
        if (!st) return ev::undefined();
        if (st->el->hasAttribute("open")) {
            if (!isModal(st)) return ev::undefined();
            return invalidState(
                "HTMLDialogElement.show: the dialog is already open as a modal dialog");
        }
        st->el->setAttribute("open", "");
        st->dialogModal = false;
        return ev::undefined();
    });

    b.def("showModal", 0, [](Value self, std::span<const Value>) -> Value {
        HostNodeState* st = dialogState(self);
        if (!st) return ev::undefined();
        if (st->el->hasAttribute("open")) {
            if (isModal(st)) return ev::undefined();
            return invalidState(
                "HTMLDialogElement.showModal: the dialog is already open as a non-modal dialog");
        }
        if (!isConnected(st->el)) {
            return invalidState("HTMLDialogElement.showModal: the dialog is not connected");
        }
        dom::Document* doc = st->el->document();
        dom::Element* focused = doc->activeElement();
        if (focused == doc->body()) focused = nullptr;   // nothing was focused
        st->el->setAttribute("open", "");
        st->dialogModal = true;
        previouslyFocused()[st->el] = dom::ElementHandle(doc, focused);
        doc->addToTopLayer(st->el, /*modal=*/true, "open");
        runDialogFocusingSteps(st->el);
        return ev::undefined();
    });

    b.def("close", 1, [](Value self, std::span<const Value> a) -> Value {
        HostNodeState* st = dialogState(self);
        if (st) closeDialog(self, st, a, 0);
        return ev::undefined();
    });

    // requestClose: a cancelable `cancel` first; closing only if nobody
    // prevented it.
    b.def("requestClose", 1, [](Value self, std::span<const Value> a) -> Value {
        HostNodeState* st = dialogState(self);
        if (!st || !st->el->hasAttribute("open")) return ev::undefined();
        ev::Persistent wrapper(self);
        ev::Persistent result(argAt(a, 0));
        if (!fireSimple(wrapper, "cancel", true)) return ev::undefined();
        st = dialogState(wrapper.get());
        if (!st) return ev::undefined();
        Value r = result.get();
        closeDialog(wrapper.get(), st, std::span<const Value>(&r, 1), 0);
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
