// The global object's prototype chain.
//
// bronze makes the global object with no prototype, so on it nothing from
// Object.prototype resolves: String(window) finds no toString and throws
// "Cannot convert object to primitive value", `window instanceof Window` has
// no Window to test, and `window.hasOwnProperty` is undefined. On the web the
// chain is window -> Window.prototype -> WindowProperties -> EventTarget
// .prototype -> Object.prototype. This builds the part a program can observe:
// an interface object whose prototype carries Symbol.toStringTag, chained onto
// brokit's EventTarget.prototype, and makes it the global's prototype through
// Object.setPrototypeOf, which bronze allows on the global object.
//
// Every heap value is held in a Persistent across the next call: each lookup,
// Object.create and defineProperty below may allocate (embed.h).

#include "bronze_host/host_internal.h"

namespace bro::bronze_host {

namespace {

// Object[name], raw: callers root it in a Persistent on the spot.
Value objectStatic(const ev::Persistent& objectCtor, const char* name) {
    return ev::getProperty(objectCtor.get(), name);
}

}  // namespace

void installGlobalPrototype(const char* ctorName) {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return;
    ev::Persistent global(gt.value);

    ev::GlobalValue objG = ev::globalValue("Object");
    if (!objG.found || !ev::isObject(objG.value)) return;
    ev::Persistent objectCtor(objG.value);

    // The base: EventTarget.prototype when brokit installed it (the page and
    // workers both have it), else Object.prototype.
    ev::Persistent base(ev::getProperty(objectCtor.get(), "prototype"));
    ev::GlobalValue etG = ev::globalValue("EventTarget");
    if (etG.found && ev::isFunction(etG.value)) {
        ev::Persistent et(etG.value);
        ev::Persistent etProto(ev::getProperty(et.get(), "prototype"));
        if (ev::isObject(etProto.get())) base.set(etProto.get());
    }

    // The interface object: `new Window()` is an Illegal constructor, as on
    // the web. A function's `prototype` is slot-backed and embed refuses to
    // assign it (embed.h, setProperty), so the prototype is the one the
    // function was born with — read off it, as HostClass does
    // (host_class.cpp) — re-parented onto `base`.
    const std::string illegal = std::string("Illegal constructor: ") + ctorName;
    ev::Persistent ctor(ev::makeFunction(
        [illegal](Value, std::span<const Value>) -> Value { return ev::throwTypeError(illegal); },
        0, ctorName));
    ev::Persistent proto(ev::getProperty(ctor.get(), "prototype"));
    if (!ev::isObject(proto.get())) return;
    ev::setPrototype(proto.get(), base.get());

    // proto[Symbol.toStringTag] = ctorName, non-enumerable, as WebIDL does.
    ev::GlobalValue symG = ev::globalValue("Symbol");
    ev::Persistent defineProperty(objectStatic(objectCtor, "defineProperty"));
    if (symG.found && ev::isObject(symG.value) && ev::isFunction(defineProperty.get())) {
        ev::Persistent symbolCtor(symG.value);
        ev::Persistent tagKey(ev::getProperty(symbolCtor.get(), "toStringTag"));
        ev::Persistent tagValue(ev::fromUtf8(ctorName));
        ev::Persistent desc(ev::createObject());
        desc.set(ev::setProperty(desc.get(), "value", tagValue.get()));
        desc.set(ev::setProperty(desc.get(), "configurable", ev::fromBool(true)));
        const Value args[3] = {proto.get(), tagKey.get(), desc.get()};
        ev::call(defineProperty.get(), objectCtor.get(), std::span<const Value>(args, 3));
    }

    // proto.constructor links back to the interface object (Window.prototype
    // already names proto).
    if (ev::isFunction(defineProperty.get())) {
        // constructor: non-enumerable, so for-in over window stays as it was.
        ev::Persistent ctorKey(ev::fromUtf8("constructor"));
        ev::Persistent desc(ev::createObject());
        desc.set(ev::setProperty(desc.get(), "value", ctor.get()));
        desc.set(ev::setProperty(desc.get(), "writable", ev::fromBool(true)));
        desc.set(ev::setProperty(desc.get(), "configurable", ev::fromBool(true)));
        const Value args[3] = {proto.get(), ctorKey.get(), desc.get()};
        ev::call(defineProperty.get(), objectCtor.get(), std::span<const Value>(args, 3));
    }

    ev::setPrototype(global.get(), proto.get());

    ev::registerGlobal(ctorName, ctor.get());
    ev::GlobalValue gt2 = ev::globalValue("globalThis");
    if (gt2.found && ev::isObject(gt2.value)) {
        ev::Persistent g2(gt2.value);
        ev::setProperty(g2.get(), ctorName, ctor.get());
    }
}

}  // namespace bro::bronze_host
