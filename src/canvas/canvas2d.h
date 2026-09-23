#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bro::render { struct CssFilterParams; }

namespace bro::canvas {

// Parse a Canvas 2D `filter` value: "none" (an empty list) or a CSS
// <filter-value-list> of blur(), brightness(), contrast(), drop-shadow(),
// grayscale(), hue-rotate(), invert(), opacity(), saturate() and sepia().
// Returns false — and leaves `out` unspecified — for anything that does not
// parse, including url() references and negative amounts, so the caller can
// keep its previous filter as the spec asks.
bool parseCanvasFilter(const std::string& str, std::vector<render::CssFilterParams>& out);

// Parse CSS color: "#rgb", "#rrggbb", "#rrggbbaa", "rgb(r,g,b)", "rgba(r,g,b,a)",
// "hsl(h,s%,l%)", "hsla(h,s%,l%,a)", named colors
bool parseCSSColor(const std::string& str, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a);

// Parse CSS font: "16px Arial", "bold 20px monospace"
struct ParsedFont { std::string family; float size; int weight; bool italic; };
ParsedFont parseCSSFont(const std::string& font);

} // namespace bro::canvas
