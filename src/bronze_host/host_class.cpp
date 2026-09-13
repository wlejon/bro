// HostClass: the three-call class shape from host_image.cpp, factored so the
// rest of the layer can take it a family at a time.
//
// WHAT IT REPLACES. Every wrapper here used to be a bare handle cell with its
// methods closed over PER INSTANCE — a fresh copy of every method object for
// every AudioParam, every WebGL buffer, every Blob — and `instanceof` answering
// false for all of them, because the registered name was either a namespace
// object or a makeBrandConstructor stub that had no prototype to brand with.
//
// THE SHAPE, and why each step is what it is:
//
//   1. makeFunction for the constructor, then READ `prototype` off it. The
//      read MINTS the slot-backed object 10.2.4 describes, as an ordinary
//      plain object. (Assigning `prototype` is still refused by name; it is
//      the read that hands you one.)
//   2. Decorate that prototype once — one copy of each method for the whole
//      class, where the web also puts them.
//   3. Birth each instance with makeHandle's 4-argument form.
//
// Born on, not swapped on: instances share the memoized per-prototype root
// shape, so their property writes keep their inline caches. An
// Object.setPrototypeOf after the fact also preserves the payload, but puts
// the cell in dictionary mode for the rest of its life.
//
// THE LEAKED PERSISTENT. The prototype must outlive every instance, which
// means the life of the process. A file-scope or function-local Persistent
// would be destroyed during static destruction — after the engine has torn the
// runtime down — freeing a root out of a registry that no longer exists. So
// the Persistent is heap-allocated and never deleted, deliberately. It is not
// what keeps the prototype alive in any case: registerGlobal roots the
// constructor for the process and the prototype hangs off it. What the
// Persistent buys is skipping a global lookup and a property read per
// instance, and a Value that is safe to hold across the allocation makeHandle
// performs.
//
// A CLASS THAT WAS NEVER INSTALLED still works: make() falls back to the bare
// 3-argument handle. That is the honest degrade for a value built before its
// install ran — no methods, but no fatal either.

#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"  // ObjectBuilder
#include "util/log.h"

#include <span>
#include <utility>

namespace bro::bronze_host {

void HostClass::install(const char* name, uint32_t arity, ev::NativeFn body,
                        const std::function<void(ObjectBuilder&)>& decorate) {
    // A class the program may name but not construct — what makeBrandConstructor
    // used to be, except that this one can actually brand, because it has a
    // prototype its instances are born on.
    ev::NativeFn ctorBody = body;
    if (!ctorBody) {
        std::string msg = std::string("bronze host ") + name + ": not constructible";
        ctorBody = [msg](Value, std::span<const Value>) { return ev::throwTypeError(msg); };
    }

    // Named, like every host method reached through ObjectBuilder::def: a
    // constructor standing in for a web-platform one answers for its `.name`
    // too, and `Element.name` reading as a diagnosed absence rather than
    // "Element" was the last place a host object could be told from a real one.
    ev::Persistent ctor(ev::makeFunction(std::move(ctorBody), arity, name));
    ctor_ = new ev::Persistent(ctor.get());

    {
        // Reading mints it. ObjectBuilder's own Persistent is what holds it
        // across the decorating allocations.
        ObjectBuilder proto(ev::getProperty(ctor.get(), "prototype"));
        proto.set("constructor", ctor.get());
        if (decorate) decorate(proto);
        // Re-read after decoration: setProperty may have moved the object.
        proto_ = new ev::Persistent(proto.get());
    }

    ev::registerGlobal(name, ctor_->get());
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && !gt.value.isUndefined() && ev::isObject(gt.value)) {
        ev::setProperty(gt.value, name, ctor_->get());
    }
}

void HostClass::alias(const char* name) const {
    if (!ctor_) return;
    ev::registerGlobal(name, ctor_->get());
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && !gt.value.isUndefined() && ev::isObject(gt.value)) {
        ev::setProperty(gt.value, name, ctor_->get());
    }
}

}  // namespace bro::bronze_host

namespace bronze::embed {

Value setPrototype(Value obj, Value proto) {
    GlobalValue objectCtor = globalValue("Object");
    if (!objectCtor.found || !isObject(objectCtor.value)) return obj;
    Persistent objectNs(objectCtor.value);
    Persistent setProto(getProperty(objectNs.get(), "setPrototypeOf"));
    if (!isFunction(setProto.get())) return obj;
    const Value args[2] = {obj, proto};
    call(setProto.get(), undefined(), std::span<const Value>(args, 2));
    return obj;
}

}  // namespace bronze::embed

namespace bro::bronze_host {

void HostClass::inherit(const HostClass& base) const {
    if (!proto_ || !base.proto_) return;
    ev::setPrototype(proto_->get(), base.proto_->get());
}

Value HostClass::make(void* data, ev::HandleDestructor dtor, ev::Finalize when) const {
    if (!proto_) return ev::makeHandle(data, dtor, when);
    return ev::makeHandle(data, dtor, when, proto_->get());
}

void HostClass::setStatic(const char* name, Value v) const {
    if (!ctor_) return;
    // setProperty may move the function object and answers its new address;
    // the registered global is a ROOT, so the registry follows the move too.
    ctor_->set(ev::setProperty(ctor_->get(), name, v));
}

Value HostClass::prototype() const {
    return proto_ ? proto_->get() : ev::undefined();
}

Value HostClass::constructor() const {
    return ctor_ ? ctor_->get() : ev::undefined();
}

}  // namespace bro::bronze_host
