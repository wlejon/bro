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

    {
        // Reading mints it. ObjectBuilder's own Persistent is what holds it
        // across the decorating allocations.
        ObjectBuilder proto(ev::getProperty(ctor.get(), "prototype"));
        if (decorate) decorate(proto);
        // Re-read after decoration: setProperty may have moved the object.
        proto_ = new ev::Persistent(proto.get());
    }

    ev::registerGlobal(name, ctor.get());
    ctor_ = new ev::Persistent(ctor.get());
}

void HostClass::alias(const char* name) const {
    if (!ctor_) return;
    ev::registerGlobal(name, ctor_->get());
}

void HostClass::inherit(const HostClass& base) const {
    if (!proto_ || !base.proto_) return;
    // Reached through the program's own Object.setPrototypeOf, the way
    // host_proxy.cpp reaches Proxy: embed has no prototype-chaining call, and
    // `Object` resolves off the builtin ladder, which a program cannot shadow.
    // Both operands are PLAIN objects here — a class prototype, never an
    // instance — so none of the handle-cell caveats apply.
    ev::GlobalValue objectCtor = ev::globalValue("Object");
    if (!objectCtor.found) return;
    ev::Persistent objectNs(objectCtor.value);
    ev::Persistent setProto(ev::getProperty(objectNs.get(), "setPrototypeOf"));
    if (!ev::isFunction(setProto.get())) return;
    const Value args[2] = {proto_->get(), base.proto_->get()};
    ev::CallResult r = ev::call(setProto.get(), ev::undefined(),
                                std::span<const Value>(args, 2));
    // A throw here would mean the chain is not what the caller declared, which
    // is worth naming rather than leaving to a mystifying `instanceof` false.
    if (r.thrown) {
        LOG_WARN("bronze_host: HostClass::inherit failed to chain a prototype");
    }
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
