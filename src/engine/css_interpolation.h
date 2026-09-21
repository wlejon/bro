#pragma once

#include "engine/css_transitions.h"
#include <css/cascade.h>
#include <string>
#include <vector>

namespace bro::engine {

inline constexpr bromath::CubicEase kLinear    {0.0f,  0.0f,  1.0f,  1.0f};
inline constexpr bromath::CubicEase kEase      {0.25f, 0.1f,  0.25f, 1.0f};
inline constexpr bromath::CubicEase kEaseIn    {0.42f, 0.0f,  1.0f,  1.0f};
inline constexpr bromath::CubicEase kEaseOut   {0.0f,  0.0f,  0.58f, 1.0f};
inline constexpr bromath::CubicEase kEaseInOut {0.42f, 0.0f,  0.58f, 1.0f};

// Parse a duration string (e.g., "0.3s", "300ms") to milliseconds.
double parseDurationMs(const std::string& val);

// Split a comma-separated CSS value list, respecting parentheses.
std::vector<std::string> splitCSS(const std::string& val);

// Build an identity transform string matching the structure of the target value.
std::string identityTransform(const std::string& target);

// Return the CSS initial value for a property matching structure.
std::string initialValueForProperty(const std::string& prop,
                                    const std::string& newVal);

// Interpolate keyframe stops for an active animation on an element.
void applyKeyframeInterpolation(const htmlayout::css::KeyframeBlock* kf,
                                const Animation& anim,
                                double currentTime,
                                htmlayout::css::ComputedStyle& style);

} // namespace bro::engine
