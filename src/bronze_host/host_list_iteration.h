#pragma once

// The iteration members of the DOM's list objects (WebIDL `iterable<T>` and
// the value iterator of an indexed list): HTMLCollection's
// `[Symbol.iterator]`, and NodeList's / DOMTokenList's `[Symbol.iterator]`,
// `forEach`, `entries`, `keys`, `values`, so `for…of`, spread and
// `Array.from` work on every one of them.

#include "embed/embed.h"

#include <functional>

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
void installArrayLikeIteration(Value obj, bool withForEach);

/// `obj` (a list with no indexed elements of its own, DOMTokenList) iterates
/// what `snapshot` answers, a real array of its items made at each call:
/// `[Symbol.iterator]`, forEach, entries, keys and values.
void installSnapshotIteration(Value obj, std::function<Value()> snapshot);

}  // namespace bro::bronze_host
