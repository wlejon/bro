#pragma once

// The SVG element interfaces (host_svg_interfaces.cpp): SVGElement, the
// abstract SVGGraphicsElement / SVGGeometryElement / text-content family and
// one interface per SVG tag.

#include "bronze_host/host_class.h"

#include <string>

namespace bro::bronze_host {

// Into the current realm, after Element.
void installSvgInterfaces();
// The prototype for an SVG-namespace element by its tag (any case): its own
// interface's, or SVGElement's.
Value svgInterfaceProto(const std::string& tagName);

}  // namespace bro::bronze_host
