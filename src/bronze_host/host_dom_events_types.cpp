#include "bronze_host/host_dom_events_types.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_html_interfaces.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

HostClass g_mouseEventClass;
HostClass g_keyboardEventClass;
HostClass g_wheelEventClass;
HostClass g_focusEventClass;
HostClass g_customEventClass;

}  // namespace

const HostClass& mouseEventHostClass() { return g_mouseEventClass; }
const HostClass& keyboardEventHostClass() { return g_keyboardEventClass; }
const HostClass& wheelEventHostClass() { return g_wheelEventClass; }
const HostClass& focusEventHostClass() { return g_focusEventClass; }
const HostClass& customEventHostClass() { return g_customEventClass; }

void installDomEventTypes() {
    // Bind to the constructor classes from js/events.js and brokit
    g_mouseEventClass.bind("MouseEvent");
    g_keyboardEventClass.bind("KeyboardEvent");
    g_wheelEventClass.bind("WheelEvent");
    g_focusEventClass.bind("FocusEvent");
    g_customEventClass.bind("CustomEvent");

    // If CustomEvent is not yet branded, install it with proper prototype chaining
    if (!g_customEventClass.prototype().isObject()) {
        g_customEventClass.install(
            "CustomEvent", 1,
            [](Value, std::span<const Value> a) -> Value {
                if (a.empty()) return ev::throwTypeError("Failed to construct 'CustomEvent': 1 argument required");
                std::string type = ev::toUtf8(a[0]);
                Value inst = g_customEventClass.make(nullptr, [](void*) {});
                ObjectBuilder b(inst);
                b.set("type", ev::fromUtf8(type));
                b.set("bubbles", ev::fromBool(false));
                b.set("cancelable", ev::fromBool(false));
                b.set("detail", ev::null());
                if (a.size() > 1 && ev::isObject(a[1])) {
                    // Read through the argument slot each time: b.set
                    // allocates, and a raw copy of a[1] would go stale.
                    Value bub = ev::getProperty(a[1], "bubbles");
                    if (!ev::isUndefined(bub)) b.set("bubbles", ev::fromBool(ev::toBool(bub)));
                    Value canc = ev::getProperty(a[1], "cancelable");
                    if (!ev::isUndefined(canc)) b.set("cancelable", ev::fromBool(ev::toBool(canc)));
                    Value det = ev::getProperty(a[1], "detail");
                    if (!ev::isUndefined(det)) b.set("detail", det);
                }
                return b.get();
            },
            nullptr);
        ev::GlobalValue evt = ev::globalValue("Event");
        if (evt.found && ev::isFunction(evt.value)) {
            Value evtProto = ev::getProperty(evt.value, "prototype");
            if (ev::isObject(evtProto)) {
                ev::setPrototype(g_customEventClass.prototype(), evtProto);
            }
        }
    }
}

int legacyKeyCodeFor(const std::string& code, const std::string& key) {
    if (key == "Enter") return 13;
    if (key == "Backspace") return 8;
    if (key == "Tab") return 9;
    if (key == "Escape") return 27;
    if (key == " " || key == "Space") return 32;
    if (key == "ArrowLeft") return 37;
    if (key == "ArrowUp") return 38;
    if (key == "ArrowRight") return 39;
    if (key == "ArrowDown") return 40;
    if (key == "Delete") return 46;
    if (key == "Home") return 36;
    if (key == "End") return 35;
    if (key == "PageUp") return 33;
    if (key == "PageDown") return 34;
    if (key == "Insert") return 45;
    if (key == "Shift") return 16;
    if (key == "Control") return 17;
    if (key == "Alt") return 18;
    if (key == "Meta") return code == "MetaRight" ? 93 : 91;
    if (code.size() == 7 && code.rfind("Numpad", 0) == 0) {
        char c = code[6];
        if (c >= '0' && c <= '9') return 96 + (c - '0');
    }
    if (key == "CapsLock") return 20;
    if (key == "NumLock") return 144;
    if (key == "ScrollLock") return 145;
    if (code.size() == 4 && code.rfind("Key", 0) == 0) {
        char c = code[3];
        if (c >= 'A' && c <= 'Z') return c;
        if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    }
    if (code.size() == 6 && code.rfind("Digit", 0) == 0) {
        char c = code[5];
        if (c >= '0' && c <= '9') return c;
    }
    if (code.size() >= 2 && code[0] == 'F') {
        try {
            int n = std::stoi(code.substr(1));
            if (n >= 1 && n <= 24) return 111 + n;
        } catch (...) {}
    }
    if (key.size() == 1) {
        char c = key[0];
        if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
        if (c >= 'A' && c <= 'Z') return c;
        if (c >= '0' && c <= '9') return c;
    }
    return 0;
}

void populateMouseEvent(ObjectBuilder& b, dom::Event& e) {
    if (auto* m = dynamic_cast<dom::MouseEvent*>(&e)) {
        b.set("clientX", ev::fromDouble(m->clientX()));
        b.set("clientY", ev::fromDouble(m->clientY()));
        b.set("pageX", ev::fromDouble(m->pageX()));
        b.set("pageY", ev::fromDouble(m->pageY()));
        b.set("screenX", ev::fromDouble(m->screenX()));
        b.set("screenY", ev::fromDouble(m->screenY()));
        b.set("offsetX", ev::fromDouble(m->offsetX()));
        b.set("offsetY", ev::fromDouble(m->offsetY()));
        b.set("movementX", ev::fromDouble(m->movementX()));
        b.set("movementY", ev::fromDouble(m->movementY()));
        b.set("button", ev::fromDouble(m->button()));
        b.set("buttons", ev::fromDouble(m->buttons()));
        b.set("which", ev::fromDouble(m->button() >= 0 ? m->button() + 1 : 0));
        b.set("detail", ev::fromDouble(m->detail()));
        b.set("ctrlKey", ev::fromBool(m->ctrlKey()));
        b.set("shiftKey", ev::fromBool(m->shiftKey()));
        b.set("altKey", ev::fromBool(m->altKey()));
        b.set("metaKey", ev::fromBool(m->metaKey()));
        // Pointer events ride on MouseEvent in this DOM (dom/event.h), and a
        // compiled handler duck-types them exactly as a JS one does.
        if (e.type().rfind("pointer", 0) == 0) {
            b.set("pointerId", ev::fromDouble(m->pointerId()));
            Value ptype = ev::fromUtf8(m->pointerType());
            b.set("pointerType", ptype);
            b.set("isPrimary", ev::fromBool(m->isPrimaryPointer()));
            b.set("width", ev::fromDouble(1.0));
            b.set("height", ev::fromDouble(1.0));
            double pressure = m->pressure();
            if (pressure < 0.0) pressure = m->buttons() != 0 ? 0.5 : 0.0;
            b.set("pressure", ev::fromDouble(pressure));
            b.set("tangentialPressure", ev::fromDouble(0.0));
            b.set("tiltX", ev::fromDouble(0.0));
            b.set("tiltY", ev::fromDouble(0.0));
            b.set("twist", ev::fromDouble(0.0));
        }
        if (auto* w = dynamic_cast<dom::WheelEvent*>(&e)) {
            b.set("deltaX", ev::fromDouble(w->deltaX()));
            b.set("deltaY", ev::fromDouble(w->deltaY()));
            b.set("deltaZ", ev::fromDouble(w->deltaZ()));
            b.set("deltaMode", ev::fromDouble(w->deltaMode()));
        }
    }
}

void populateKeyboardEvent(ObjectBuilder& b, dom::Event& e) {
    if (auto* k = dynamic_cast<dom::KeyboardEvent*>(&e)) {
        Value key = ev::fromUtf8(k->key());
        b.set("key", key);
        Value code = ev::fromUtf8(k->code());
        b.set("code", code);
        b.set("repeat", ev::fromBool(k->repeat()));
        b.set("location", ev::fromDouble(k->location()));
        b.set("ctrlKey", ev::fromBool(k->ctrlKey()));
        b.set("shiftKey", ev::fromBool(k->shiftKey()));
        b.set("altKey", ev::fromBool(k->altKey()));
        b.set("metaKey", ev::fromBool(k->metaKey()));
        int kc = legacyKeyCodeFor(k->code(), k->key());
        b.set("keyCode", ev::fromDouble(kc));
        b.set("which", ev::fromDouble(kc));
        b.set("charCode", ev::fromDouble(0.0));
    }
}

void populateInputAndFormEvents(ObjectBuilder& b, dom::Event& e) {
    if (auto* inp = dynamic_cast<dom::InputEvent*>(&e)) {
        if (inp->data().empty()) {
            b.set("data", ev::null());
        } else {
            b.set("data", ev::fromUtf8(inp->data()));
        }
        b.set("inputType", ev::fromUtf8(inp->inputType()));
        b.set("isComposing", ev::fromBool(inp->isComposing()));
    }

    if (auto* comp = dynamic_cast<dom::CompositionEvent*>(&e)) {
        b.set("data", ev::fromUtf8(comp->data()));
    }

    // SubmitEvent — which control triggered the submit.
    if (auto* sub = dynamic_cast<dom::SubmitEvent*>(&e)) {
        dom::Element* who = sub->submitter();
        Value v = who ? hostElementValue(who) : ev::null();
        b.set("submitter", v);
    }

    // CSS animation / transition events — what the event is about. The
    // engine fills these in; they never reached the page before.
    if (auto* an = dynamic_cast<dom::AnimationEvent*>(&e)) {
        b.set("animationName", ev::fromUtf8(an->animationName()));
        b.set("elapsedTime", ev::fromDouble(an->elapsedTime()));
        b.set("pseudoElement", ev::fromUtf8(an->pseudoElement()));
    }
    if (auto* tr = dynamic_cast<dom::TransitionEvent*>(&e)) {
        b.set("propertyName", ev::fromUtf8(tr->propertyName()));
        b.set("elapsedTime", ev::fromDouble(tr->elapsedTime()));
        b.set("pseudoElement", ev::fromUtf8(tr->pseudoElement()));
    }
}

void populateClipboardEvent(ObjectBuilder& b, dom::Event& e) {
    if (auto* clip = dynamic_cast<dom::ClipboardEvent*>(&e)) {
        auto textHolder = std::make_shared<std::string>(clip->clipboardText());
        ObjectBuilder dt;
        dt.def("getData", 1, [textHolder](Value, std::span<const Value> a) {
            Value fV = argAt(a, 0);
            if (ev::isObject(fV) || ev::isUndefined(fV)) return ev::fromUtf8("");
            std::string fmt = ev::toUtf8(fV);
            for (char& c : fmt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (fmt == "text" || fmt == "text/plain") return ev::fromUtf8(*textHolder);
            return ev::fromUtf8("");
        });
        dt.def("setData", 2, [textHolder](Value, std::span<const Value> a) {
            if (a.size() >= 2) {
                Value fV = argAt(a, 0);
                std::string fmt = (!ev::isObject(fV) && !ev::isUndefined(fV)) ? ev::toUtf8(fV) : "";
                for (char& c : fmt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (fmt == "text" || fmt == "text/plain") {
                    Value dataV = argAt(a, 1);
                    *textHolder = (!ev::isObject(dataV) && !ev::isUndefined(dataV)) ? ev::toUtf8(dataV) : "";
                }
            }
            return ev::undefined();
        });
        dt.def("clearData", 1, [textHolder](Value, std::span<const Value>) {
            textHolder->clear();
            return ev::undefined();
        });
        std::vector<std::string> typeList;
        if (!textHolder->empty()) typeList.push_back("text/plain");
        Value typesArr = hostArrayOf(typeList.size(), [&typeList](size_t i) {
            return ev::fromUtf8(typeList[i]);
        });
        dt.set("types", typesArr);
        b.set("clipboardData", ev::setPrototype(dt.get(), dataTransferHostClass().prototype()));
    }
}

}  // namespace bro::bronze_host
