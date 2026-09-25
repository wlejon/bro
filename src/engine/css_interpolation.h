#pragma once

#include "engine/css_transitions.h"
#include <css/cascade.h>
#include <string>
#include <vector>

namespace bro::engine {

// Parse a duration string (e.g., "0.3s", "300ms") to milliseconds.
double parseDurationMs(const std::string& val);

// Split a comma-separated CSS value list, respecting parentheses.
std::vector<std::string> splitCSS(const std::string& val);

// Build an identity transform string matching the structure of the target value.
std::string identityTransform(const std::string& target);

// Interpolate two `transform` lists at progress t (CSS Transforms 2): pairs
// sharing a primitive blend argument by argument, the rest through matrix
// decomposition. False when the pair can only flip (percentages in the part
// that needs matrices, a singular matrix, an unparseable list).
bool interpolateTransformLists(const std::string& from, const std::string& to, float t,
                               std::string& out);

// Return the CSS initial value for a property matching structure.
std::string initialValueForProperty(const std::string& prop,
                                    const std::string& newVal);

} // namespace bro::engine
