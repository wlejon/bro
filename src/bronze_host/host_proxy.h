#pragma once

// makeHostProxy (host_proxy.cpp): the property trap behind `style`, computed
// style, `dataset`, and `localStorage`.

#include "embed/embed.h"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// A live view whose keys are not known when it is built: `el.style`,
// `el.dataset`, the computed declaration, `localStorage`. Each callback is
// optional; one left empty behaves as the absence of that capability (no keys,
// no membership, dropped writes) rather than as an error.
//
// `get` returns true when it HANDLED the key — false means "no such property",
// which is how a style object distinguishes an unset CSS property (handled,
// answers "") from a name that is not a property at all.
struct HostProxyTraps {
    // Consulted before `get` and `has`, and never enumerated: the object's
    // fixed method surface, which on the web would sit on a prototype. May be
    // undefined for a view that has none.
    Value methods = ev::undefined();
    std::function<bool(const std::string& key, Value& out)> get;
    std::function<void(const std::string& key, Value v)> set;
    std::function<bool(const std::string& key)> has;
    std::function<std::vector<std::string>()> ownKeys;
    std::function<void(const std::string& key)> remove;

    // A CALLABLE view. The four live views this file was written for are data,
    // and an empty object target is all they need; a view standing in for a
    // FUNCTION in another engine is not, because [[Call]] and [[Construct]]
    // are the target's and no trap can conjure them. So such a caller supplies
    // its own callable target — an unnamed host function, so that it carries
    // no own `name`/`length` for the 10.5 invariants to check the traps
    // against — and the two traps that forward through it.
    Value target = ev::undefined();
    std::function<Value(Value thisValue, std::span<const Value> args)> apply;
    std::function<Value(std::span<const Value> args)> construct;
};

// The proxy itself. Builds its own empty target — see host_proxy.cpp for why
// the target must stay empty for the 10.5 invariants to stay vacuous.
Value makeHostProxy(HostProxyTraps traps);

}  // namespace bro::bronze_host
