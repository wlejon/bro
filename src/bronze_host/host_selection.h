#pragma once

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_class.h"

namespace bro::dom {
class Selection;
}

namespace bro::bronze_host {

extern HostClass g_selectionClass;

void installSelectionGlobals();
Value wrapSelection(bro::dom::Selection* s);
bro::dom::Selection* hostSelectionOf(Value v);

} // namespace bro::bronze_host
