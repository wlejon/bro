// AbortController and AbortSignal: the one way the web says "stop, I no longer
// want this" to work that is already in progress.
//
// It is here now because fetch is here. Every other async surface in this layer
// settles within a frame, so cancelling one is a nicety; a fetch is the first
// thing an app starts that it may genuinely need to call off — a texture load
// for a scene the user has already navigated away from — and the signal is the
// argument every library expects to be able to pass through.
//
// WHERE THE STATE LIVES. `aborted` twice: once on the payload struct and once
// as a JS property. That is not redundancy. The property is the app's — it can
// read it, and it can also overwrite it — while the payload copy is what host
// code decides on, so a fetch that must not deliver a response cannot be talked
// into delivering one by an app that assigned `signal.aborted = false`. Every
// other piece of signal state (the reason, the abort listeners, the dependents
// of an AbortSignal.any composite) is an ordinary property, for the reason the
// GC rule in host_internal.h gives: host memory holding Persistents would have
// to be freed by this object's handle finalizer, and a finalizer may not call
// into the embed API.
//
// TWO SHAPES THE WEB HAS THAT THIS DOES NOT. `AbortSignal.abort()` and
// `AbortSignal.timeout()` are statics on a constructor, and `AbortSignal` is a
// plain namespace object here instead. That was once forced — a host could not
// put a property on a function value at all — and is now merely unconverted:
// embed::setProperty takes a FUNCTION receiver, so the constructor-with-statics
// shape is buildable whenever someone wants it, here and for `URL` in
// host_file.cpp. It costs little either way: `new AbortSignal()` is illegal on
// the web anyway, so the only thing lost is `x instanceof AbortSignal` — and
// that stays false regardless, because `prototype` is the one property a host
// function still cannot be given.
//
// And `reason` is a plain `{name, message}` object where the web hands you a
// DOMException. bronze cannot build a value on a chosen prototype; `e.name ===
// 'AbortError'` is what real code tests, and that answers correctly.

#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"  // ObjectBuilder, argAt, numAt

#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// The AbortSignal.any() composites that abort when this signal does. Own and
// enumerable, like every private list in this layer, and prefixed so a
// collision with app data is a deliberate act.
const char* const kDependentsKey = "__bronzeAbortDependents";

void hostSignalDtor(void* p) { delete static_cast<HostAbortSignal*>(p); }

HostAbortSignal* signalOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* s = static_cast<HostAbortSignal*>(ev::handleData(v));
    if (!s || s->tag != kHostSignalTag) return nullptr;
    return s;
}

// Set a property on an object held in a Persistent, keeping the Persistent
// current: setProperty may MOVE the object and answers its new address.
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
    // The reason, whatever the app made it — a string, an Error, the default
    // AbortError object. throwValue rather than throwError: `throw
    // signal.reason` is the spec's own wording, and re-wrapping it would break
    // the `catch (e) { if (e.name === 'AbortError') }` that follows.
    Value reason = ev::getProperty(thisValue, "reason");
    return ev::throwValue(reason);
}

// A signal, optionally already aborted. `reason` is applied by the caller
// through hostAbortSignal so that path is written once.
Value makeSignal() {
    auto* s = new HostAbortSignal();
    ObjectBuilder b(ev::makeHandle(s, hostSignalDtor));

    // Data properties first, so the shape is fixed before an abort rewrites
    // them — the same reason host_file.cpp seeds readyState up front.
    b.set("aborted", ev::fromBool(false));
    b.set("reason", ev::undefined());
    b.set("onabort", ev::null());

    b.def("addEventListener", 2, signalAddListener);
    b.def("removeEventListener", 2, signalRemoveListener);
    b.def("throwIfAborted", 0, signalThrowIfAborted);
    return b.get();
}

Value controllerAbort(Value thisValue, std::span<const Value> a) {
    // Through the property, not a captured handle: the controller and its
    // signal are two objects and the app can read the same edge with
    // `controller.signal`. A host-side pointer would be a second answer to the
    // question of which signal this controller owns.
    ev::Persistent self(thisValue);
    Value reason = argAt(a, 0);
    ev::Persistent reasonP(reason);
    Value signal = ev::getProperty(self.get(), "signal");
    hostAbortSignal(signal, reasonP.get());
    return ev::undefined();
}

Value makeController() {
    ObjectBuilder b;
    b.set("signal", makeSignal());
    b.def("abort", 1, controllerAbort);
    return b.get();
}

// AbortSignal.any([a, b]): a signal that aborts when the first of its sources
// does, carrying that source's reason.
//
// The edge is recorded on the SOURCE, pointing at the composite, and never the
// other way: a host-side dependents table would hold Persistents and would have
// to be freed by a finalizer. The consequence is worth stating — a source keeps
// its composites alive for as long as the source lives, which is the opposite
// of what the spec's garbage-collection note asks for. In exchange there is no
// finalizer touching a root slot, which is not negotiable here.
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
            // Already aborted: the composite is aborted now, with this
            // source's reason, and the remaining sources do not matter.
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
    // A host deadline rather than a JS setTimeout: the callback holds the
    // signal in a Persistent, which is safe precisely because the timer table
    // is host memory freed on the main thread and never from a finalizer.
    hostSetTimeout(
        [signal]() {
            // The error first, into a root: it allocates, and the order the
            // two arguments of a call are evaluated in is unspecified — so
            // reading signal.get() inline would be a stale Value half the time
            // and correct the other half, which is the worst kind of bug.
            ev::Persistent reason(hostMakeDomError("TimeoutError", "signal timed out"));
            hostAbortSignal(signal.get(), reason.get());
        },
        ms);
    return signal.get();
}

}  // namespace

// ---------------------------------------------------------------------------
// The pieces other files reach for
// ---------------------------------------------------------------------------

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
    // Already aborted keeps its first reason and fires nothing. That is what
    // makes abort() safe to call from a cleanup path that may run twice, which
    // is most of them.
    if (!s || s->aborted) return;

    ev::Persistent self(signal);
    ev::Persistent reasonP(reason);
    if (ev::isUndefined(reasonP.get())) {
        reasonP.set(hostMakeDomError("AbortError", "signal is aborted without reason"));
    }

    // The payload flag BEFORE anything can call back in: a listener that starts
    // a fetch on this signal must find it already aborted.
    s->aborted = true;
    setOn(self, "reason", reasonP.get());
    setOn(self, "aborted", ev::fromBool(true));

    dispatchHostEvent(ev::Persistent(self.get()), "abort");

    // Then the composites. The reason is re-read from the signal rather than
    // carried in a local, because every call above may have moved it — and a
    // composite must report the SOURCE's reason, which is what this object now
    // holds whether it was given one or defaulted to AbortError.
    for (ev::Persistent& dep : hostListSnapshot(self, kDependentsKey)) {
        ev::Persistent reasonNow(ev::getProperty(self.get(), "reason"));
        hostAbortSignal(dep.get(), reasonNow.get());
    }
}

void installAbortGlobals() {
    Value ctor = ev::makeFunction(
        [](Value, std::span<const Value>) { return makeController(); }, 0);
    ev::registerGlobal("AbortController", ctor);

    // A namespace, not a constructor — see the file header. `new AbortSignal()`
    // is a TypeError on the web too, so nothing that works there breaks here.
    ObjectBuilder ns;
    ns.def("abort", 1, [](Value, std::span<const Value> a) {
        ev::Persistent signal(makeSignal());
        hostAbortSignal(signal.get(), argAt(a, 0));
        return signal.get();
    });
    ns.def("timeout", 1, signalTimeout);
    ns.def("any", 1, signalAny);
    ev::registerGlobal("AbortSignal", ns.get());
}

}  // namespace bro::bronze_host
