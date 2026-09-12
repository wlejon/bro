#pragma once

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_internal.h"

namespace bro::dom {
class Range;
}

namespace bro::bronze_host {

extern HostClass g_rangeClass;

void installRangeGlobals();
Value wrapOwnedRange(bro::dom::Range* r);
bro::dom::Range* hostRangeOf(Value v);

} // namespace bro::bronze_host
