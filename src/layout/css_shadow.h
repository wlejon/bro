#pragma once

#include "render/renderer.h"
#include <string>
#include <string_view>
#include <vector>

namespace bro::layout {

// One shadow of a box-shadow / text-shadow list, or the argument of a
// `drop-shadow()` filter: `inset? && <length>{2,4} && <color>?` in any
// order. A shadow that names no colour is `currentcolor` (CSS Backgrounds 3,
// Text Decoration 3, Filter Effects 1), so the caller passes the element's
// `color` in and it is kept unless the shadow names one.
struct CssShadow {
    bool inset = false;
    float dx = 0, dy = 0, blur = 0, spread = 0;
    bromath::Color color;
};

// Parses one shadow item. Needs at least the two offsets; more than
// `maxLengths` lengths (3 for text-shadow / drop-shadow, 4 for box-shadow)
// or two colours is invalid.
bool parseCssShadow(std::string_view item, const bromath::Color& currentColor,
                    int maxLengths, CssShadow& out);

// Splits a shadow list on its top-level commas (commas inside rgb()/hsl()
// belong to the colour), trimming each item.
std::vector<std::string> splitCssShadowList(std::string_view list);

} // namespace bro::layout
