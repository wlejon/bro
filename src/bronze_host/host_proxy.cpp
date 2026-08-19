// makeHostProxy: the property trap this layer spent its whole life without.
//
// WHY IT MATTERS MORE THAN ITS SIZE SUGGESTS. Three of the DOM's most-used
// objects are LIVE VIEWS whose key set is not known when the object is built:
// `el.dataset` (whatever data-* attributes exist, plus whatever the app adds),
// `el.style` (363 CSS properties, plus custom `--*` ones an app invents), and
// the computed declaration. The only shape available before was an accessor
// pair per NAME per OBJECT, which forced two compromises this file exists to
// undo: a curated ~110-property list, because 363 accessors on every element
// of a thousand-element UI is not a cost anyone would pay; and no `dataset` at
// all, because a dataset that silently drops `dataset.k = 'v'` is worse than no
// dataset (src/bronze_host/README.md argued exactly that).
//
// A proxy over an EMPTY target constrains nothing. bronze's Proxy implements
// the 10.5 essential invariants, and every one of those checks reads the
// TARGET — never a trap — so an extensible target with no own properties makes
// them all vacuous, and the traps below may answer however the DOM answers.
// That is why `methods` is not on the target: keeping the target genuinely
// empty is what makes `Object.keys(el.style)` exactly the set properties
// rather than the set properties plus setProperty/getPropertyValue/cssText.
//
// SYMBOL KEYS. A get trap receives every key the program uses, and property
// keys are strings or SYMBOLS. `toUtf8` of a symbol is a TypeError by spec
// (rt_convert.cpp says so in as many words), so a trap that stringified its key
// first would turn `style[Symbol.toPrimitive]` — a question any library may
// ask — into a thrown error. `embed::isSymbol` is the guard. Symbol keys then
// answer undefined, which is the truthful answer for all three of these
// objects, since none has a symbol-keyed member; there is no forwarding them to
// the target either, because getProperty takes a string_view key.
//
// LIFETIME. The traps live in a shared_ptr captured by each handler function;
// embed::makeFunction parks closure state in the function's environment slot
// and destroys it with the function object, so the pack dies when the last
// trap does. `methods` is held in a Persistent inside that pack because it is
// a heap value the collector may move.

#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"  // ObjectBuilder, argAt

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

struct TrapPack {
    HostProxyTraps t;
    ev::Persistent methods;
};

}  // namespace

Value makeHostProxy(HostProxyTraps traps) {
    auto pack = std::make_shared<TrapPack>();
    pack->t = std::move(traps);
    pack->methods.set(pack->t.methods);

    ObjectBuilder h;

    // get(target, key, receiver)
    h.def("get", 3, [pack](Value, std::span<const Value> a) -> Value {
        Value key = argAt(a, 1);
        if (ev::isSymbol(key)) return ev::undefined();
        const std::string k = ev::toUtf8(key);
        // Methods first: they are the object's fixed surface, and no CSS
        // property or data-* key exists named `setProperty` to collide.
        if (!ev::isUndefined(pack->methods.get())) {
            Value m = ev::getProperty(pack->methods.get(), k.c_str());
            if (!ev::isUndefined(m)) return m;
        }
        Value out = ev::undefined();
        if (pack->t.get && pack->t.get(k, out)) return out;
        return ev::undefined();
    });

    // set(target, key, value, receiver) — the boolean is what decides between
    // a silent no-op and a TypeError, and compiled code is always strict.
    // Answering true even for a dropped write is deliberate: the web's style
    // object ignores an unknown property rather than throwing.
    h.def("set", 4, [pack](Value, std::span<const Value> a) -> Value {
        Value key = argAt(a, 1);
        if (ev::isSymbol(key)) return ev::fromBool(true);
        const std::string k = ev::toUtf8(key);
        if (pack->t.set) pack->t.set(k, argAt(a, 2));
        return ev::fromBool(true);
    });

    h.def("has", 2, [pack](Value, std::span<const Value> a) -> Value {
        Value key = argAt(a, 1);
        if (ev::isSymbol(key)) return ev::fromBool(false);
        const std::string k = ev::toUtf8(key);
        if (!ev::isUndefined(pack->methods.get()) &&
            !ev::isUndefined(ev::getProperty(pack->methods.get(), k.c_str()))) {
            return ev::fromBool(true);
        }
        return ev::fromBool(pack->t.has && pack->t.has(k));
    });

    h.def("deleteProperty", 2, [pack](Value, std::span<const Value> a) -> Value {
        Value key = argAt(a, 1);
        if (ev::isSymbol(key)) return ev::fromBool(true);
        const std::string k = ev::toUtf8(key);
        if (pack->t.remove) pack->t.remove(k);
        return ev::fromBool(true);
    });

    // ownKeys(target). Methods are deliberately absent — see the header: they
    // are the object's API, not its data, and on the web they live on a
    // prototype, where enumeration does not reach them either.
    h.def("ownKeys", 1, [pack](Value, std::span<const Value>) -> Value {
        std::vector<std::string> keys;
        if (pack->t.ownKeys) keys = pack->t.ownKeys();
        return hostArrayOf(keys.size(),
                           [&keys](size_t i) { return ev::fromUtf8(keys[i]); });
    });

    // getOwnPropertyDescriptor(target, key). Object.keys walks ownKeys and
    // then asks this for each name, keeping only the enumerable ones — so
    // without this trap the list above would be filtered away to nothing
    // against an empty target. `configurable: true` is not a detail: a
    // descriptor reported non-configurable for a property the target does not
    // have is exactly what invariant 10.5.5 rejects.
    h.def("getOwnPropertyDescriptor", 2, [pack](Value, std::span<const Value> a) -> Value {
        Value key = argAt(a, 1);
        if (ev::isSymbol(key)) return ev::undefined();
        const std::string k = ev::toUtf8(key);
        if (!(pack->t.has && pack->t.has(k))) return ev::undefined();
        Value out = ev::undefined();
        if (pack->t.get) pack->t.get(k, out);
        ObjectBuilder d;
        d.set("value", out);
        d.set("writable", ev::fromBool(true));
        d.set("enumerable", ev::fromBool(true));
        d.set("configurable", ev::fromBool(true));
        return d.get();
    });

    // apply(target, thisArg, argsArray) / construct(target, argsArray,
    // newTarget) — present only for a callable view, because a trap defined on
    // a non-callable target is a trap the language will never consult.
    if (pack->t.apply) {
        h.def("apply", 3, [pack](Value, std::span<const Value> a) -> Value {
            Value list = argAt(a, 2);
            const uint32_t n = ev::isObject(list)
                                   ? static_cast<uint32_t>(
                                         ev::toDouble(ev::getProperty(list, "length")))
                                   : 0;
            std::vector<Value> args;
            args.reserve(n);
            for (uint32_t i = 0; i < n; ++i) args.push_back(ev::getElement(list, i));
            return pack->t.apply(argAt(a, 1), std::span<const Value>(args));
        });
    }
    if (pack->t.construct) {
        h.def("construct", 3, [pack](Value, std::span<const Value> a) -> Value {
            Value list = argAt(a, 1);
            const uint32_t n = ev::isObject(list)
                                   ? static_cast<uint32_t>(
                                         ev::toDouble(ev::getProperty(list, "length")))
                                   : 0;
            std::vector<Value> args;
            args.reserve(n);
            for (uint32_t i = 0; i < n; ++i) args.push_back(ev::getElement(list, i));
            return pack->t.construct(std::span<const Value>(args));
        });
    }

    ev::Persistent handler(h.get());
    ev::Persistent target(ev::isUndefined(pack->t.target) ? ev::createObject()
                                                          : pack->t.target);

    ev::GlobalValue proxyCtor = ev::globalValue("Proxy");
    if (!proxyCtor.found) {
        // Nothing sane to degrade to: every caller here is a live view whose
        // whole point is the trap. Answering the bare target at least keeps
        // reads and writes from throwing, which is what a build with a
        // Proxy-less runtime would want if one could exist.
        return target.get();
    }
    ev::Persistent ctor(proxyCtor.value);

    const Value args[2] = {target.get(), handler.get()};
    ev::CallResult r = ev::construct(ctor.get(), std::span<const Value>(args, 2));
    if (r.thrown) return target.get();
    return r.value;
}

}  // namespace bro::bronze_host
