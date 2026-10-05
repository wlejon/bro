#pragma once

#include "bronze_host/host_builder.h"
#include "dom/element.h"
#include <include/core/SkRRect.h>
#include <string>

namespace bro::bronze_host {

bool parseRoundRectRadii(Value v, SkVector radii[4], std::string& err);
void installCanvas2DPaths(ObjectBuilder& b, dom::Element* el);

}  // namespace bro::bronze_host
