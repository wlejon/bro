// Iteration for the DOM's list objects (host_list_iteration.h).
//
// The embed API names properties by string, so the symbol-keyed member a
// list needs (`[Symbol.iterator]`) is defined, and read, through the realm's
// own Object.defineProperty and Reflect.get. The members themselves are
// Array.prototype's, which are generic over anything with a `length` and
// indexed elements: a live collection answers both, so its iterator walks
// what it holds at each step, as the web's does.

#include "bronze_host/host_list_iteration.h"
#include "bronze_host/host_builder.h"

namespace bro::bronze_host {

namespace {

// globalThis[a][b], or undefined.
Value globalMember(const char* a, const char* b) {
    ev::GlobalValue g = ev::globalValue(a);
    if (!g.found || !ev::isObject(g.value)) return ev::undefined();
    return ev::getProperty(g.value, b);
}

Value arrayProtoMethod(const char* name) {
    ev::GlobalValue arr = ev::globalValue("Array");
    if (!arr.found || !ev::isObject(arr.value)) return ev::undefined();
    ev::Persistent proto(ev::getProperty(arr.value, "prototype"));
    return ev::isObject(proto.get()) ? ev::getProperty(proto.get(), name) : ev::undefined();
}

// Object.defineProperty(obj, key, {value, writable, configurable}), not
// enumerable, as a WebIDL operation is.
void defineMethod(Value objIn, Value keyIn, Value fnIn) {
    ev::Persistent obj(objIn), key(keyIn), fn(fnIn);
    if (!ev::isObject(obj.get()) || ev::isUndefined(fn.get())) return;
    ev::Persistent define(globalMember("Object", "defineProperty"));
    if (!ev::isFunction(define.get())) return;
    ObjectBuilder desc;
    desc.set("value", fn.get());
    desc.set("writable", ev::fromBool(true));
    desc.set("configurable", ev::fromBool(true));
    ev::Persistent descP(desc.get());
    const Value args[3] = {obj.get(), key.get(), descP.get()};
    ev::call(define.get(), ev::undefined(), std::span<const Value>(args, 3));
}

}  // namespace

Value symbolIterator() {
    return globalMember("Symbol", "iterator");
}

Value getSymbolKeyed(Value objIn, Value keyIn) {
    ev::Persistent obj(objIn), key(keyIn);
    if (!ev::isObject(obj.get())) return ev::undefined();
    ev::Persistent get(globalMember("Reflect", "get"));
    if (!ev::isFunction(get.get())) return ev::undefined();
    const Value args[2] = {obj.get(), key.get()};
    ev::CallResult r = ev::call(get.get(), ev::undefined(), std::span<const Value>(args, 2));
    return r.thrown ? ev::undefined() : r.value;
}

void setToStringTag(Value objIn, const std::string& name) {
    ev::Persistent obj(objIn);
    ev::Persistent define(globalMember("Object", "defineProperty"));
    ev::Persistent key(globalMember("Symbol", "toStringTag"));
    if (!ev::isFunction(define.get()) || !ev::isObject(obj.get())) return;
    ev::Persistent value(ev::fromUtf8(name));
    ev::Persistent desc(ev::createObject());
    desc.set(ev::setProperty(desc.get(), "value", value.get()));
    desc.set(ev::setProperty(desc.get(), "configurable", ev::fromBool(true)));
    const Value args[3] = {obj.get(), key.get(), desc.get()};
    ev::call(define.get(), ev::undefined(), std::span<const Value>(args, 3));
}

Value reflectGet(Value objIn, Value keyIn, Value receiverIn) {
    ev::Persistent obj(objIn), key(keyIn), receiver(receiverIn);
    if (!ev::isObject(obj.get())) return ev::undefined();
    ev::Persistent get(globalMember("Reflect", "get"));
    if (!ev::isFunction(get.get())) return ev::undefined();
    const Value args[3] = {obj.get(), key.get(), receiver.get()};
    ev::CallResult r = ev::call(get.get(), ev::undefined(), std::span<const Value>(args, 3));
    if (r.thrown) ev::throwValue(r.value);
    return r.value;
}

void installArrayLikeIteration(Value objIn, bool withForEach) {
    ev::Persistent obj(objIn);
    ev::Persistent iter(symbolIterator());
    ev::Persistent values(arrayProtoMethod("values"));
    defineMethod(obj.get(), iter.get(), values.get());
    if (!withForEach) return;
    for (const char* name : {"forEach", "entries", "keys", "values"}) {
        ev::Persistent fn(arrayProtoMethod(name));
        ev::Persistent key(ev::fromUtf8(name));
        defineMethod(obj.get(), key.get(), fn.get());
    }
}

void installSnapshotIteration(Value objIn, std::function<Value()> snapshot) {
    ev::Persistent obj(objIn);
    auto delegate = [snapshot](const char* method) {
        std::string name = method;
        return ev::makeFunction(
            [snapshot, name](Value, std::span<const Value> a) -> Value {
                std::vector<ev::Persistent> args(a.begin(), a.end());
                ev::Persistent list(snapshot());
                ev::Persistent fn(arrayProtoMethod(name.c_str()));
                if (!ev::isFunction(fn.get())) return ev::undefined();
                std::vector<Value> argv;
                for (const auto& p : args) argv.push_back(p.get());
                ev::CallResult r = ev::call(fn.get(), list.get(), argv);
                return r.thrown ? ev::throwValue(r.value) : r.value;
            },
            0);
    };
    ev::Persistent iter(symbolIterator());
    ev::Persistent valuesFn(delegate("values"));
    defineMethod(obj.get(), iter.get(), valuesFn.get());
    for (const char* name : {"forEach", "entries", "keys", "values"}) {
        ev::Persistent fn(delegate(name));
        ev::Persistent key(ev::fromUtf8(name));
        defineMethod(obj.get(), key.get(), fn.get());
    }
}

}  // namespace bro::bronze_host
