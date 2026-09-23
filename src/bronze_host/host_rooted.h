#pragma once

// Rooted: an ev::Persistent that reads as a Value.
//
// The embed GC contract (bronze embed.h) makes a raw Value stale after any
// allocating call, and a property read is one: getProperty and getElement may
// run a getter. A function that reads an options object field after field, or
// walks an array element by element, therefore cannot hold the object in a
// plain Value. It roots it once instead:
//
//     bool readThing(Value optsIn, Thing& out) {
//         const Rooted opts(optsIn);
//         out.a = getPropNumber(opts, "a", 0);   // each use re-reads the slot
//         out.b = getPropNumber(opts, "b", 0);
//     }
//
// Each use converts to the value's CURRENT address. The argument-order rule
// still applies: a use that sits beside an allocating argument in the same
// call (`setProperty(o, "k", fromUtf8(s))`) may be read before that argument
// runs, so build the allocating argument in its own statement first.

#include "embed/embed.h"

namespace bro::bronze_host {

class Rooted {
public:
    explicit Rooted(bronze::Value v) : slot_(v) {}

    bronze::Value get() const { return slot_.get(); }
    operator bronze::Value() const { return slot_.get(); }
    void set(bronze::Value v) { slot_.set(v); }

private:
    bronze::embed::Persistent slot_;
};

}  // namespace bro::bronze_host
