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
// PER REALM, PER THREAD. The constructor and prototype live in a table keyed
// by (class, realm), held per thread because a realm belongs to one thread. A
// prototype is mutable and app code can reach it, so a realm sharing another's
// would let one app change what another's `div.appendChild` does; and a Worker
// installing the same file-scope class used to overwrite the main thread's
// pointers with its own heap's. A destroyed realm's entries are dropped
// (ev::addRealmDestroyHook), so its classes are collectable with it.
//
// THE LEAKED PERSISTENTS. The table is heap-allocated and never freed: a
// static or thread_local destructor would release roots after the engine has
// torn the runtime down. The Persistents are not what keeps the prototype
// alive in any case — registerGlobal roots the constructor in its realm and
// the prototype hangs off it. What they buy is skipping a global lookup and a
// property read per instance, and a Value that is safe to hold across the
// allocation makeHandle performs.
//
// A CLASS THIS REALM NEVER INSTALLED still works: make() falls back to the bare
// 3-argument handle. That is the honest degrade for a value built before its
// install ran — no methods, but no fatal either.

#include "bronze_host/host_class.h"
#include "bronze_host/host_builder.h"  // ObjectBuilder
#include "util/log.h"

#include <span>
#include <unordered_map>
#include <utility>

namespace bro::bronze_host {

struct HostClass::Slots {
    ev::Persistent* ctor = nullptr;
    ev::Persistent* proto = nullptr;
};

namespace {

struct SlotKey {
    const HostClass* cls;
    const ev::Realm* realm;
    bool operator==(const SlotKey& o) const noexcept { return cls == o.cls && realm == o.realm; }
};
struct SlotKeyHash {
    size_t operator()(const SlotKey& k) const noexcept {
        return std::hash<const void*>()(k.cls) * 31u ^ std::hash<const void*>()(k.realm);
    }
};
using SlotTable = std::unordered_map<SlotKey, HostClass::Slots, SlotKeyHash>;

SlotTable& threadSlots() {
    static thread_local auto* t = new SlotTable();
    return *t;
}

void releaseSlots(HostClass::Slots& s) {
    delete s.ctor;
    delete s.proto;
    s.ctor = nullptr;
    s.proto = nullptr;
}

void dropRealmSlots(ev::Realm* realm) {
    SlotTable& t = threadSlots();
    for (auto it = t.begin(); it != t.end();) {
        if (it->first.realm == realm) {
            releaseSlots(it->second);
            it = t.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace

HostClass::Slots& HostClass::slots() const {
    return threadSlots()[SlotKey{this, ev::currentRealm()}];
}

HostClass::Slots* HostClass::slotsIfAny() const {
    auto& t = threadSlots();
    auto it = t.find(SlotKey{this, ev::currentRealm()});
    return it == t.end() ? nullptr : &it->second;
}

void HostClass::install(const char* name, uint32_t arity, ev::NativeFn body,
                        const std::function<void(ObjectBuilder&)>& decorate) {
    ev::addRealmDestroyHook(&dropRealmSlots);
    Slots& s = slots();
    releaseSlots(s);
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
    s.ctor = new ev::Persistent(ctor.get());

    {
        // Reading mints it. ObjectBuilder's own Persistent is what holds it
        // across the decorating allocations.
        ObjectBuilder proto(ev::getProperty(ctor.get(), "prototype"));
        proto.set("constructor", ctor.get());
        if (decorate) decorate(proto);
        // Re-read after decoration: setProperty may have moved the object.
        s.proto = new ev::Persistent(proto.get());
    }

    ev::registerGlobal(name, s.ctor->get());
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && !gt.value.isUndefined() && ev::isObject(gt.value)) {
        ev::Persistent gtP(gt.value);
        ev::setProperty(gtP.get(), name, s.ctor->get());
    }
}

void HostClass::alias(const char* name) const {
    const Slots* s = slotsIfAny();
    if (!s || !s->ctor) return;
    ev::registerGlobal(name, s->ctor->get());
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && !gt.value.isUndefined() && ev::isObject(gt.value)) {
        ev::Persistent gtP(gt.value);
        ev::setProperty(gtP.get(), name, s->ctor->get());
    }
}

void HostClass::bind(const char* name) {
    ev::GlobalValue g = ev::globalValue(name);
    if (!g.found || !ev::isFunction(g.value)) {
        ev::GlobalValue gt = ev::globalValue("globalThis");
        if (gt.found && !gt.value.isUndefined() && ev::isObject(gt.value)) {
            ev::Persistent gtP(gt.value);
            Value c = ev::getProperty(gtP.get(), name);
            if (ev::isFunction(c)) {
                g.found = true;
                g.value = c;
            }
        }
    }
    if (g.found && ev::isFunction(g.value)) {
        ev::addRealmDestroyHook(&dropRealmSlots);
        ev::Persistent ctor(g.value);
        Slots& s = slots();
        releaseSlots(s);
        s.ctor = new ev::Persistent(ctor.get());
        Value proto = ev::getProperty(ctor.get(), "prototype");
        if (ev::isObject(proto)) {
            s.proto = new ev::Persistent(proto);
        }
    }
}

}  // namespace bro::bronze_host

namespace bronze::embed {

// Same return contract as setProperty: the receiver's POST-call address.
// Object.setPrototypeOf converts the receiver to dictionary mode, which
// allocates, and the getProperty that finds it allocates before that — so
// both arguments are rooted first and the caller must use what comes back,
// not what it passed in.
Value setPrototype(Value obj, Value proto) {
    Persistent objP(obj);
    Persistent protoP(proto);
    GlobalValue objectCtor = globalValue("Object");
    if (!objectCtor.found || !isObject(objectCtor.value)) return objP.get();
    Persistent objectNs(objectCtor.value);
    Persistent setProto(getProperty(objectNs.get(), "setPrototypeOf"));
    if (!isFunction(setProto.get())) return objP.get();
    const Value args[2] = {objP.get(), protoP.get()};
    call(setProto.get(), undefined(), std::span<const Value>(args, 2));
    return objP.get();
}

}  // namespace bronze::embed

namespace bro::bronze_host {

void HostClass::inherit(const HostClass& base) const {
    const Slots* s = slotsIfAny();
    const Slots* b = base.slotsIfAny();
    if (!s || !s->proto || !b || !b->proto) return;
    s->proto->set(ev::setPrototype(s->proto->get(), b->proto->get()));
}

Value HostClass::make(void* data, ev::HandleDestructor dtor, ev::Finalize when) const {
    const Slots* s = slotsIfAny();
    if (!s || !s->proto) return ev::makeHandle(data, dtor, when);
    return ev::makeHandle(data, dtor, when, s->proto->get());
}

void HostClass::setStatic(const char* name, Value v) const {
    const Slots* s = slotsIfAny();
    if (!s || !s->ctor) return;
    // setProperty may move the function object and answers its new address;
    // the registered global is a ROOT, so the registry follows the move too.
    ev::Persistent valP(v);
    s->ctor->set(ev::setProperty(s->ctor->get(), name, valP.get()));
}

Value HostClass::prototype() const {
    const Slots* s = slotsIfAny();
    return s && s->proto ? s->proto->get() : ev::undefined();
}

Value HostClass::constructor() const {
    const Slots* s = slotsIfAny();
    return s && s->ctor ? s->ctor->get() : ev::undefined();
}

}  // namespace bro::bronze_host
