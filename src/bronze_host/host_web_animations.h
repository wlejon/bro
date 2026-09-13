#pragma once

#include "bronze_host/host_internal.h"
#include <vector>
#include <cstdint>

namespace bro::bronze_host {

void installWebAnimationGlobals();
void decorateElementWebAnimations(ObjectBuilder& b);
void decorateDocumentWebAnimations(ObjectBuilder& b);
void deliverWebAnimationFinishEvents();

} // namespace bro::bronze_host
