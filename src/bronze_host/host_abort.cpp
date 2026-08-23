// AbortSignal, AbortController — bronze_host translation unit.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

HostClass g_signalClass;
HostClass g_controllerClass;

const char* const kDependentsKey = "__bronzeAbortDependents";

void hostSignalDtor(void* p) { delete static_cast<HostAbortSignal*>(p); }

HostAbortSignal* signalOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* s = static_cast<HostAbortSignal*>(ev::handleData(v));
    if (!s || s->tag != kHostSignalTag) return nullptr;
    return s;
}

void setOn(ev::Persistent& obj, const char* key, Value v) {
    obj.set(ev::setProperty(obj.get(), key, v));
}

Value signalAddListener(Value thisValue, std::span<const Value> a) {
    ev::Persistent self(thisValue);
    Value typeV = argAt(a, 0);
    if (ev::isObject(typeV)) return ev::undefined();
    const std::string type = ev::toUtf8(typeV);
    addHostListener(self, type, argAt(a, 1));
    return ev::undefined();
}

Value signalRemoveListener(Value thisValue, std::span<const Value> a) {
    ev::Persistent self(thisValue);
    Value typeV = argAt(a, 0);
    if (ev::isObject(typeV)) return ev::undefined();
    const std::string type = ev::toUtf8(typeV);
    removeHostListener(self, type, argAt(a, 1));
    return ev::undefined();
}

Value signalThrowIfAborted(Value thisValue, std::span<const Value>) {
    ev::Persistent self(thisValue);
    HostAbortSignal* s = signalOf(self.get());
    if (!s || !s->aborted) return ev::undefined();
    Value reason = ev::getProperty(thisValue, "reason");
    return ev::throwValue(reason);
}

void decorateSignalProto(ObjectBuilder& b) {
    b.def("addEventListener", 2, signalAddListener);
    b.def("removeEventListener", 2, signalRemoveListener);
    b.def("throwIfAborted", 0, signalThrowIfAborted);
}

Value makeSignal() {
    auto* s = new HostAbortSignal();
    ObjectBuilder b(g_signalClass.make(s, hostSignalDtor));
    b.set("aborted", ev::fromBool(false));
    b.set("reason", ev::undefined());
    b.set("onabort", ev::null());
    return b.get();
}

Value controllerAbort(Value thisValue, std::span<const Value> a) {
    ev::Persistent self(thisValue);
    Value reason = argAt(a, 0);
    ev::Persistent reasonP(reason);
    Value signal = ev::getProperty(self.get(), "signal");
    hostAbortSignal(signal, reasonP.get());
    return ev::undefined();
}

void decorateControllerProto(ObjectBuilder& b) {
    b.def("abort", 1, controllerAbort);
}

Value makeController() {
    ObjectBuilder b(g_controllerClass.make(nullptr, [](void*) {}));
    b.set("signal", makeSignal());
    return b.get();
}

Value signalAny(Value, std::span<const Value> a) {
    ev::Persistent composite(makeSignal());

    Value listV = argAt(a, 0);
    if (!ev::isObject(listV)) {
        return ev::throwTypeError("AbortSignal.any: argument must be a list of signals");
    }
    ev::Persistent list(listV);

    double lenD = ev::toDouble(ev::getProperty(list.get(), "length"));
    const uint32_t len = (lenD > 0.0) ? static_cast<uint32_t>(lenD) : 0;
    for (uint32_t i = 0; i < len; ++i) {
        ev::Persistent source(ev::getElement(list.get(), i));
        const HostAbortSignal* s = signalOf(source.get());
        if (!s) continue;
        if (s->aborted) {
            Value reason = ev::getProperty(source.get(), "reason");
            hostAbortSignal(composite.get(), reason);
            return composite.get();
        }
        hostListAppend(source, kDependentsKey, composite.get());
    }
    return composite.get();
}

Value signalTimeout(Value, std::span<const Value> a) {
    const double ms = numAt(a, 0);
    ev::Persistent signal(makeSignal());
    hostSetTimeout(
        [signal]() {
            ev::Persistent reason(hostMakeDomError("TimeoutError", "signal timed out"));
            hostAbortSignal(signal.get(), reason.get());
        },
        ms);
    return signal.get();
}

}  // namespace

const HostAbortSignal* hostAbortSignalOf(Value v) { return signalOf(v); }

Value makeAbortSignalValue() { return makeSignal(); }

Value hostMakeDomError(const char* name, const std::string& message) {
    ObjectBuilder b;
    b.set("name", ev::fromUtf8(name));
    b.set("message", ev::fromUtf8(message));
    return b.get();
}

void hostAbortSignal(Value signal, Value reason) {
    HostAbortSignal* s = signalOf(signal);
    if (!s || s->aborted) return;

    ev::Persistent self(signal);
    ev::Persistent reasonP(reason);
    if (ev::isUndefined(reasonP.get())) {
        reasonP.set(hostMakeDomError("AbortError", "signal is aborted without reason"));
    }

    s->aborted = true;
    setOn(self, "reason", reasonP.get());
    setOn(self, "aborted", ev::fromBool(true));

    dispatchHostEvent(ev::Persistent(self.get()), "abort");

    for (ev::Persistent& dep : hostListSnapshot(self, kDependentsKey)) {
        ev::Persistent reasonNow(ev::getProperty(self.get(), "reason"));
        hostAbortSignal(dep.get(), reasonNow.get());
    }
}

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------

void installAbortGlobals() {
    g_signalClass.install(
        "AbortSignal", 0,
        // Not constructible: the IDL declares no constructor, and
        // HostClass::install turns a null body into the TypeError the
        // web specifies for `new AbortSignal()`.
        nullptr,
        decorateSignalProto);
    g_signalClass.setStatic("abort", ev::makeFunction([](Value, std::span<const Value> a) { ev::Persistent signal(makeSignal()); hostAbortSignal(signal.get(), argAt(a, 0)); return signal.get(); }, 1));
    g_signalClass.setStatic("timeout", ev::makeFunction(signalTimeout, 1));
    g_signalClass.setStatic("any", ev::makeFunction(signalAny, 1));

    g_controllerClass.install(
        "AbortController", 0,
        [](Value, std::span<const Value> a) {
            return makeController();
        },
        decorateControllerProto);

}

}  // namespace bro::bronze_host
