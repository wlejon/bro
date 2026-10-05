#pragma once

#include "bronze_host/host_class.h"

namespace bro::bronze_host {

void decorateIFrameProto(ObjectBuilder& b);
void clearContentWindowProxy(uint64_t scopeId);

} // namespace bro::bronze_host
