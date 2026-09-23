// HTMLDialogElement: `open`, `returnValue`, `show()`, `showModal()`,
// `close(returnValue)`, `requestClose(returnValue)` and the `close` / `cancel`
// events (HTML §4.11.4).
//
// `open` reflects the attribute, and the UA stylesheet (default_styles.h)
// hides a dialog without it, so opening and closing is attribute work. The
// `close` event is queued as a task, not fired inside close(): on the web a
// listener runs after the code that closed the dialog has finished.
//
// What is not here: the top layer, ::backdrop, inertness of the rest of the
// page and Escape-to-cancel. A modal dialog is tracked as modal (show() on it
// throws, as the spec says) but renders and hit-tests like a non-modal one.

#include "bronze_host/host_template.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "engine/engine.h"

namespace bro::bronze_host {

namespace {

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

void closeDialog(Value self, HostNodeState* st, std::span<const Value> a, size_t resultIndex) {
    if (!st->el->hasAttribute("open")) return;
    ev::Persistent wrapper(self);
    Value result = argAt(a, resultIndex);
    if (!ev::isUndefined(result)) st->dialogReturnValue = ev::toUtf8(result);
    st->el->removeAttribute("open");
    st->dialogModal = false;
    postHostTask([wrapper]() { fireSimple(wrapper, "close", false); });
}

Value invalidState(const std::string& message) {
    return ev::throwValue(hostMakeDomError("InvalidStateError", message));
}

}  // namespace

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
            if (!st->dialogModal) return ev::undefined();
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
            if (st->dialogModal) return ev::undefined();
            return invalidState(
                "HTMLDialogElement.showModal: the dialog is already open as a non-modal dialog");
        }
        if (!isConnected(st->el)) {
            return invalidState("HTMLDialogElement.showModal: the dialog is not connected");
        }
        st->el->setAttribute("open", "");
        st->dialogModal = true;
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
