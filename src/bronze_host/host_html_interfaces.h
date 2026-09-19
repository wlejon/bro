#pragma once

#include "bronze_host/host_internal.h"
#include <string>

namespace bro::bronze_host {

const HostClass& nodeHostClass();
const HostClass& documentHostClass();
const HostClass& elementHostClass();
const HostClass& htmlElementHostClass();
const HostClass& htmlImageElementClass();
// Brands for host-built plain objects: set as the prototype of every 2D
// context (host_canvas2d.cpp) and dataTransfer (host_dom_events.cpp).
const HostClass& canvasRenderingContext2DHostClass();
const HostClass& dataTransferHostClass();
const HostClass& svgElementHostClass();

Value htmlInterfaceProto(const std::string& tagName);

void installHtmlInterfaces();

}  // namespace bro::bronze_host
