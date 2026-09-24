#pragma once

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_internal.h"

#include <memory>

namespace bro::dom {
class Range;
}

namespace bro::bronze_host {

extern HostClass g_rangeClass;

void installRangeGlobals();
// A new script Range taking ownership of `r`.
Value wrapOwnedRange(bro::dom::Range* r);
// A script Range over a range something else also holds — the Selection's
// live range (getRangeAt).
Value wrapSharedRange(std::shared_ptr<bro::dom::Range> r);
bro::dom::Range* hostRangeOf(Value v);
std::shared_ptr<bro::dom::Range> hostSharedRangeOf(Value v);

} // namespace bro::bronze_host
