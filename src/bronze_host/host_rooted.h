#pragma once

// Rooted: an ev::Persistent that reads as a Value.
//
// A Value in a plain C++ local already stays current across allocating calls
// (bronze embed.h, "THE GC CONTRACT": the collector pins what the stack
// names). A Rooted is a root of its own wherever it lives, so it also holds a
// Value as a member of a host struct or a capture kept past the frame. Each
// use converts to the value's current address:
//
//     const Rooted opts(optsIn);
//     out.a = getPropNumber(opts, "a", 0);

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
