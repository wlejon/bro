#pragma once

// The iteration members of the DOM's list objects (WebIDL `iterable<T>` and
// the value iterator of an indexed list): HTMLCollection's
// `[Symbol.iterator]`, and NodeList's / DOMTokenList's `[Symbol.iterator]`,
// `forEach`, `entries`, `keys`, `values`, so `for…of`, spread and
// `Array.from` work on every one of them.

#include "embed/embed.h"

#include <functional>
#include <string>

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

/// The realm's Symbol.iterator.
Value symbolIterator();

/// Reflect.get(obj, key) for a symbol `key` (the embed API names properties
/// by string): what a host proxy answers for a symbol-keyed member of its
/// method surface.
Value getSymbolKeyed(Value obj, Value key);

/// `obj` (which has a `length` and indexed elements, a live collection's
/// proxy or the methods it answers first) gets Array.prototype's iterator as
/// its `[Symbol.iterator]`, and with `withForEach` its forEach / entries /
/// keys / values.
/// obj[Symbol.toStringTag] = name (non-enumerable, configurable, as WebIDL
/// gives an interface prototype): what Object.prototype.toString names.
void setToStringTag(Value obj, const std::string& name);

/// Reflect.get(obj, key, receiver): an accessor found on `obj` runs with
/// `receiver` as this (a host proxy's prototype members run on the proxy).
/// What it throws propagates.
Value reflectGet(Value obj, Value key, Value receiver);

void installArrayLikeIteration(Value obj, bool withForEach);

/// `obj` (a list with no indexed elements of its own, DOMTokenList) iterates
/// what `snapshot` answers, a real array of its items made at each call:
/// `[Symbol.iterator]`, forEach, entries, keys and values.
void installSnapshotIteration(Value obj, std::function<Value()> snapshot);

}  // namespace bro::bronze_host
