// The WebIDL attribute kinds of the host's globals (host_global_kinds.h).

#include "bronze_host/host_global_kinds.h"

#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace bro::bronze_host {

namespace {

// Per thread: a worker's realm is installed on its own thread, and its
// `self` is readonly where a window's is [Replaceable].
std::unordered_map<std::string, ev::GlobalKind>& kinds() {
    thread_local std::unordered_map<std::string, ev::GlobalKind> map;
    return map;
}

std::unordered_set<std::string>& accessors() {
    thread_local std::unordered_set<std::string> set;
    return set;
}

}  // namespace

void registerHostGlobal(const char* name, Value value, ev::GlobalKind kind) {
    const ev::Persistent v(value);
    accessors().erase(name);
    if (kind == ev::GlobalKind::Replaceable) {
        kinds().erase(name);
    } else {
        kinds()[name] = kind;
    }
    ev::registerGlobal(name, v.get(), kind);
}

ev::GlobalKind hostGlobalKind(const std::string& name) {
    auto it = kinds().find(name);
    return it == kinds().end() ? ev::GlobalKind::Replaceable : it->second;
}

bool isHostGlobalAccessor(const std::string& name) { return accessors().count(name) != 0; }

void registerHostGlobalAccessor(const char* name, ev::NativeFn getter, ev::NativeFn setter) {
    const std::string getName = "get " + std::string(name);
    const std::string setName = "set " + std::string(name);
    const ev::Persistent g(ev::makeFunction(std::move(getter), 0, getName));
    const ev::Persistent s(setter ? ev::makeFunction(std::move(setter), 1, setName) : ev::undefined());
    kinds().erase(name);
    accessors().insert(name);
    ev::registerGlobalAccessor(name, g.get(), s.get());
}

void defineReplaceableAttribute(ObjectBuilder& global, const char* name, ev::NativeFn getter) {
    const std::string key = name;
    global.accessor(name, std::move(getter), [key](Value self, std::span<const Value> a) -> Value {
        const ev::Persistent v(argAt(a, 0));
        const ev::Persistent target(ev::isObject(self) ? self : ev::globalValue("globalThis").value);
        if (!ev::isObject(target.get())) return ev::undefined();
        ev::deleteProperty(target.get(), key);
        ev::setProperty(target.get(), key, v.get());
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
