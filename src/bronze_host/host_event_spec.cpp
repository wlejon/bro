// The descriptor `dispatchEvent` takes, and the engine event it becomes.
//
// A program dispatches either a plain `{type, detail}` descriptor (the
// README's CustomEvent shape) or an instance of one of js/events.js's
// classes — `new MouseEvent('click', {clientX: 10})`. Both are objects with
// a `type`; what differs is which fields matter. For the mouse and pointer
// families the coordinates, buttons and modifiers cross into a
// dom::MouseEvent so a listener sees them, and the numeric `detail` is the
// click count UIEvent defines rather than a CustomEvent payload; for the key
// family `key` and `code` cross into a dom::KeyboardEvent; everything else
// is a CustomEvent when a detail was given and a plain Event otherwise.

#include "bronze_host/host_event_spec.h"

#include "dom/event.h"

namespace bro::bronze_host {

namespace {

bool isKeyType(const std::string& t) {
    return t == "keydown" || t == "keyup" || t == "keypress";
}

bool isWheelType(const std::string& t) { return t == "wheel"; }

bool isMouseType(const std::string& t) {
    return t == "click" || t == "dblclick" || t == "auxclick" || t == "contextmenu" ||
           t == "mousedown" || t == "mouseup" || t == "mousemove" ||
           t == "mouseenter" || t == "mouseleave" || t == "mouseover" || t == "mouseout" ||
           t == "pointerdown" || t == "pointerup" || t == "pointermove" ||
           t == "pointerenter" || t == "pointerleave" || t == "pointerover" ||
           t == "pointerout" || t == "pointercancel" || isWheelType(t);
}

double numberAt(Value obj, const char* key, double fallback) {
    Value v = ev::getProperty(obj, key);
    return ev::isNumber(v) ? ev::toDouble(v) : fallback;
}

bool boolAt(Value obj, const char* key) {
    Value v = ev::getProperty(obj, key);
    return !ev::isUndefined(v) && ev::toBool(v);
}

std::string stringAt(Value obj, const char* key) {
    Value v = ev::getProperty(obj, key);
    return ev::isString(v) ? ev::toUtf8(v) : std::string();
}

void fillMouse(dom::MouseEvent& m, const EventSpec& spec) {
    m.setClientX(spec.clientX);
    m.setClientY(spec.clientY);
    m.setPageX(spec.clientX);
    m.setPageY(spec.clientY);
    m.setScreenX(spec.screenX);
    m.setScreenY(spec.screenY);
    m.setMovementX(spec.movementX);
    m.setMovementY(spec.movementY);
    m.setButton(spec.button);
    m.setButtons(spec.buttons);
    m.setDetail(spec.clickCount);
    m.setCtrlKey(spec.ctrlKey);
    m.setShiftKey(spec.shiftKey);
    m.setAltKey(spec.altKey);
    m.setMetaKey(spec.metaKey);
    if (spec.pointerId != 0) m.setPointerId(spec.pointerId);
    if (!spec.pointerType.empty()) m.setPointerType(spec.pointerType);
    if (spec.isPrimary) m.setIsPrimaryPointer(true);
    if (spec.pressure >= 0.0) m.setPressure(spec.pressure);
}

}  // namespace

bool readEventSpec(Value descV, const char* what, EventSpec& out) {
    if (!ev::isObject(descV)) {
        ev::throwTypeError(std::string(what) +
                           ".dispatchEvent: expects an event object, e.g. "
                           "{ type: 'app:ping', detail: 'text' }");
        return false;
    }
    ev::Persistent desc(descV);
    Value typeV = ev::getProperty(desc.get(), "type");
    if (ev::isObject(typeV) || ev::isUndefined(typeV) || ev::isNull(typeV)) {
        ev::throwTypeError(std::string(what) +
                           ".dispatchEvent: the event object needs a string `type`");
        return false;
    }
    out.type = ev::toUtf8(typeV);
    if (out.type.empty()) {
        ev::throwTypeError(std::string(what) +
                           ".dispatchEvent: `type` must not be empty");
        return false;
    }

    out.bubbles = boolAt(desc.get(), "bubbles");
    out.cancelable = boolAt(desc.get(), "cancelable");
    out.ctrlKey = boolAt(desc.get(), "ctrlKey");
    out.shiftKey = boolAt(desc.get(), "shiftKey");
    out.altKey = boolAt(desc.get(), "altKey");
    out.metaKey = boolAt(desc.get(), "metaKey");

    if (isMouseType(out.type)) {
        out.clientX = numberAt(desc.get(), "clientX", 0);
        out.clientY = numberAt(desc.get(), "clientY", 0);
        out.screenX = numberAt(desc.get(), "screenX", out.clientX);
        out.screenY = numberAt(desc.get(), "screenY", out.clientY);
        out.movementX = numberAt(desc.get(), "movementX", 0);
        out.movementY = numberAt(desc.get(), "movementY", 0);
        out.button = static_cast<int>(numberAt(desc.get(), "button", 0));
        out.buttons = static_cast<int>(numberAt(desc.get(), "buttons", 0));
        out.clickCount = static_cast<int>(numberAt(desc.get(), "detail", 0));
        out.deltaX = numberAt(desc.get(), "deltaX", 0);
        out.deltaY = numberAt(desc.get(), "deltaY", 0);
        out.deltaZ = numberAt(desc.get(), "deltaZ", 0);
        out.deltaMode = static_cast<int>(numberAt(desc.get(), "deltaMode", 0));
        out.pointerId = static_cast<int>(numberAt(desc.get(), "pointerId", 0));
        out.pointerType = stringAt(desc.get(), "pointerType");
        out.isPrimary = boolAt(desc.get(), "isPrimary");
        out.pressure = numberAt(desc.get(), "pressure", -1.0);
        return true;
    }

    if (isKeyType(out.type)) {
        out.key = stringAt(desc.get(), "key");
        out.code = stringAt(desc.get(), "code");
        out.repeat = boolAt(desc.get(), "repeat");
        return true;
    }

    Value detailV = ev::getProperty(desc.get(), "detail");
    if (!ev::isUndefined(detailV) && !ev::isNull(detailV)) {
        out.hasDetail = true;
        if (ev::isObject(detailV)) {
            ev::GlobalValue g = ev::globalValue("JSON");
            if (g.found && ev::isObject(g.value)) {
                Value stringifyFn = ev::getProperty(g.value, "stringify");
                if (ev::isFunction(stringifyFn)) {
                    ev::CallResult res = ev::call(stringifyFn, g.value, std::span<const Value>(&detailV, 1));
                    if (!res.thrown && ev::isString(res.value)) {
                        out.detail = ev::toUtf8(res.value);
                    }
                }
            }
        } else {
            out.detail = ev::toUtf8(detailV);
        }
    }
    // `key` / `code` on a non-key type are still carried, as they were: a
    // program that dispatches its own `{type: 'hotkey', key: 'F5'}` reads
    // them back off the copy.
    out.key = stringAt(desc.get(), "key");
    out.code = stringAt(desc.get(), "code");
    return true;
}

bool dispatchEventSpec(const EventSpec& spec, const std::function<void(dom::Event&)>& dispatch) {
    // Runs DOM listeners, re-entering this layer for compiled listeners.
    // Single-threaded and re-entrant by construction: nothing here holds a
    // bare Value across the call.
    if (isKeyType(spec.type)) {
        dom::KeyboardEvent k(spec.type, spec.bubbles, spec.cancelable);
        k.setKey(spec.key);
        k.setCode(spec.code.empty() ? spec.key : spec.code);
        k.setCtrlKey(spec.ctrlKey);
        k.setShiftKey(spec.shiftKey);
        k.setAltKey(spec.altKey);
        k.setMetaKey(spec.metaKey);
        k.setRepeat(spec.repeat);
        dispatch(k);
        return !k.defaultPrevented();
    }
    if (isWheelType(spec.type)) {
        dom::WheelEvent w(spec.type, spec.bubbles, spec.cancelable);
        fillMouse(w, spec);
        w.setDeltaX(spec.deltaX);
        w.setDeltaY(spec.deltaY);
        w.setDeltaZ(spec.deltaZ);
        w.setDeltaMode(spec.deltaMode);
        dispatch(w);
        return !w.defaultPrevented();
    }
    if (isMouseType(spec.type)) {
        dom::MouseEvent m(spec.type, spec.bubbles, spec.cancelable);
        fillMouse(m, spec);
        dispatch(m);
        return !m.defaultPrevented();
    }
    if (spec.hasDetail) {
        dom::CustomEvent custom(spec.type, spec.bubbles, spec.cancelable);
        custom.setDetail(spec.detail);
        dispatch(custom);
        return !custom.defaultPrevented();
    }
    dom::Event plain(spec.type, spec.bubbles, spec.cancelable);
    dispatch(plain);
    return !plain.defaultPrevented();
}

}  // namespace bro::bronze_host
