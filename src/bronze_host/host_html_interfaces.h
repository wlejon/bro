#pragma once

#include "bronze_host/host_internal.h"
#include <string>

namespace bro::bronze_host {

const HostClass& nodeHostClass();
const HostClass& documentHostClass();
const HostClass& elementHostClass();
const HostClass& htmlElementHostClass();
const HostClass& htmlMediaElementHostClass();
const HostClass& htmlImageElementClass();

Value htmlInterfaceProto(const std::string& tagName);

void installHtmlInterfaces();

}  // namespace bro::bronze_host
