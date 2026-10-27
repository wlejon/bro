#pragma once

#include "bronze_host/host_class.h"

namespace bro::bronze_host {

/// HTMLIFrameElement's members; an <appview>'s prototype starts from them too
/// (host_appview.h).
void decorateIFrameProto(ObjectBuilder& b);

} // namespace bro::bronze_host
