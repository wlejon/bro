#pragma once

#include <cstdint>
#include <string>

namespace bro::canvas {

// Parse CSS color: "#rgb", "#rrggbb", "#rrggbbaa", "rgb(r,g,b)", "rgba(r,g,b,a)",
// "hsl(h,s%,l%)", "hsla(h,s%,l%,a)", named colors
bool parseCSSColor(const std::string& str, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a);

// Parse CSS font: "16px Arial", "bold 20px monospace"
struct ParsedFont { std::string family; float size; int weight; bool italic; };
ParsedFont parseCSSFont(const std::string& font);

} // namespace bro::canvas
