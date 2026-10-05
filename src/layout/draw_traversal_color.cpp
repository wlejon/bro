// CSS colour parsing for the painter and the replaced controls: hex, the
// functional notations (through htmlayout's parser) and the named colours.

#include "layout/draw_traversal.h"
#include "css/color.h"

#include <cctype>
#include <cstdlib>
#include <unordered_map>

namespace bro::layout {

using bromath::cfromColor8;

bool DrawTraversal::tryParseColor(const std::string& colorStr, bromath::Color& out) {
    if (colorStr.empty()) return false;
    if (colorStr == "transparent") {
        // CSS transparent = rgba(0,0,0,0). Returning false would leave the
        // caller's `out` unchanged (often opaque black), so gradient stops
        // and similar uses of "transparent" would paint as solid black.
        out = cfromColor8({0, 0, 0, 0});
        return true;
    }

    // Hex color
    if (colorStr[0] == '#') {
        std::string hex = colorStr.substr(1);
        if (hex.size() == 3) {
            hex = {hex[0], hex[0], hex[1], hex[1], hex[2], hex[2]};
        }
        if (hex.size() == 6 || hex.size() == 8) {
            unsigned long val = std::strtoul(hex.c_str(), nullptr, 16);
            bromath::Color8 p;
            if (hex.size() == 6) {
                p = {static_cast<uint8_t>((val >> 16) & 0xFF),
                     static_cast<uint8_t>((val >> 8) & 0xFF),
                     static_cast<uint8_t>(val & 0xFF),
                     255};
            } else {
                p = {static_cast<uint8_t>((val >> 24) & 0xFF),
                     static_cast<uint8_t>((val >> 16) & 0xFF),
                     static_cast<uint8_t>((val >> 8) & 0xFF),
                     static_cast<uint8_t>(val & 0xFF)};
            }
            out = cfromColor8(p);
            return true;
        }
    }

    // Every functional notation — rgb()/hsl() in legacy and modern syntax,
    // hwb/lab/lch/oklab/oklch, color() in the wide-gamut spaces (gamut-mapped
    // into sRGB), color-mix(), relative colours, calc() components — goes
    // through htmlayout's parser, the same one the cascade validates with.
    // light-dark() has already been resolved to one branch by the restyle
    // pass (Document::resolveColorSchemeValues). A `currentcolor` nested in a
    // function resolves to black here: this parser has no element; a bare
    // `currentcolor` is left to the caller (false), which knows the colour.
    if (colorStr.find('(') != std::string::npos) {
        htmlayout::css::Color c;
        if (!htmlayout::css::tryParseColor(colorStr, c)) return false;
        out = cfromColor8({c.r, c.g, c.b, c.a});
        return true;
    }

    // Named colors — full CSS Color Level 4 set
    static const std::unordered_map<std::string, bromath::Color> named = {
        {"aliceblue",cfromColor8({240, 248, 255, 255})},{"antiquewhite",cfromColor8({250, 235, 215, 255})},
        {"aqua",cfromColor8({0, 255, 255, 255})},{"aquamarine",cfromColor8({127, 255, 212, 255})},
        {"azure",cfromColor8({240, 255, 255, 255})},{"beige",cfromColor8({245, 245, 220, 255})},
        {"bisque",cfromColor8({255, 228, 196, 255})},{"black",cfromColor8({0, 0, 0, 255})},
        {"blanchedalmond",cfromColor8({255, 235, 205, 255})},{"blue",cfromColor8({0, 0, 255, 255})},
        {"blueviolet",cfromColor8({138, 43, 226, 255})},{"brown",cfromColor8({165, 42, 42, 255})},
        {"burlywood",cfromColor8({222, 184, 135, 255})},{"cadetblue",cfromColor8({95, 158, 160, 255})},
        {"chartreuse",cfromColor8({127, 255, 0, 255})},{"chocolate",cfromColor8({210, 105, 30, 255})},
        {"coral",cfromColor8({255, 127, 80, 255})},{"cornflowerblue",cfromColor8({100, 149, 237, 255})},
        {"cornsilk",cfromColor8({255, 248, 220, 255})},{"crimson",cfromColor8({220, 20, 60, 255})},
        {"cyan",cfromColor8({0, 255, 255, 255})},{"darkblue",cfromColor8({0, 0, 139, 255})},
        {"darkcyan",cfromColor8({0, 139, 139, 255})},{"darkgoldenrod",cfromColor8({184, 134, 11, 255})},
        {"darkgray",cfromColor8({169, 169, 169, 255})},{"darkgreen",cfromColor8({0, 100, 0, 255})},
        {"darkgrey",cfromColor8({169, 169, 169, 255})},{"darkkhaki",cfromColor8({189, 183, 107, 255})},
        {"darkmagenta",cfromColor8({139, 0, 139, 255})},{"darkolivegreen",cfromColor8({85, 107, 47, 255})},
        {"darkorange",cfromColor8({255, 140, 0, 255})},{"darkorchid",cfromColor8({153, 50, 204, 255})},
        {"darkred",cfromColor8({139, 0, 0, 255})},{"darksalmon",cfromColor8({233, 150, 122, 255})},
        {"darkseagreen",cfromColor8({143, 188, 143, 255})},{"darkslateblue",cfromColor8({72, 61, 139, 255})},
        {"darkslategray",cfromColor8({47, 79, 79, 255})},{"darkslategrey",cfromColor8({47, 79, 79, 255})},
        {"darkturquoise",cfromColor8({0, 206, 209, 255})},{"darkviolet",cfromColor8({148, 0, 211, 255})},
        {"deeppink",cfromColor8({255, 20, 147, 255})},{"deepskyblue",cfromColor8({0, 191, 255, 255})},
        {"dimgray",cfromColor8({105, 105, 105, 255})},{"dimgrey",cfromColor8({105, 105, 105, 255})},
        {"dodgerblue",cfromColor8({30, 144, 255, 255})},{"firebrick",cfromColor8({178, 34, 34, 255})},
        {"floralwhite",cfromColor8({255, 250, 240, 255})},{"forestgreen",cfromColor8({34, 139, 34, 255})},
        {"fuchsia",cfromColor8({255, 0, 255, 255})},{"gainsboro",cfromColor8({220, 220, 220, 255})},
        {"ghostwhite",cfromColor8({248, 248, 255, 255})},{"gold",cfromColor8({255, 215, 0, 255})},
        {"goldenrod",cfromColor8({218, 165, 32, 255})},{"gray",cfromColor8({128, 128, 128, 255})},
        {"green",cfromColor8({0, 128, 0, 255})},{"greenyellow",cfromColor8({173, 255, 47, 255})},
        {"grey",cfromColor8({128, 128, 128, 255})},{"honeydew",cfromColor8({240, 255, 240, 255})},
        {"hotpink",cfromColor8({255, 105, 180, 255})},{"indianred",cfromColor8({205, 92, 92, 255})},
        {"indigo",cfromColor8({75, 0, 130, 255})},{"ivory",cfromColor8({255, 255, 240, 255})},
        {"khaki",cfromColor8({240, 230, 140, 255})},{"lavender",cfromColor8({230, 230, 250, 255})},
        {"lavenderblush",cfromColor8({255, 240, 245, 255})},{"lawngreen",cfromColor8({124, 252, 0, 255})},
        {"lemonchiffon",cfromColor8({255, 250, 205, 255})},{"lightblue",cfromColor8({173, 216, 230, 255})},
        {"lightcoral",cfromColor8({240, 128, 128, 255})},{"lightcyan",cfromColor8({224, 255, 255, 255})},
        {"lightgoldenrodyellow",cfromColor8({250, 250, 210, 255})},{"lightgray",cfromColor8({211, 211, 211, 255})},
        {"lightgreen",cfromColor8({144, 238, 144, 255})},{"lightgrey",cfromColor8({211, 211, 211, 255})},
        {"lightpink",cfromColor8({255, 182, 193, 255})},{"lightsalmon",cfromColor8({255, 160, 122, 255})},
        {"lightseagreen",cfromColor8({32, 178, 170, 255})},{"lightskyblue",cfromColor8({135, 206, 250, 255})},
        {"lightslategray",cfromColor8({119, 136, 153, 255})},{"lightslategrey",cfromColor8({119, 136, 153, 255})},
        {"lightsteelblue",cfromColor8({176, 196, 222, 255})},{"lightyellow",cfromColor8({255, 255, 224, 255})},
        {"lime",cfromColor8({0, 255, 0, 255})},{"limegreen",cfromColor8({50, 205, 50, 255})},
        {"linen",cfromColor8({250, 240, 230, 255})},{"magenta",cfromColor8({255, 0, 255, 255})},
        {"maroon",cfromColor8({128, 0, 0, 255})},{"mediumaquamarine",cfromColor8({102, 205, 170, 255})},
        {"mediumblue",cfromColor8({0, 0, 205, 255})},{"mediumorchid",cfromColor8({186, 85, 211, 255})},
        {"mediumpurple",cfromColor8({147, 111, 219, 255})},{"mediumseagreen",cfromColor8({60, 179, 113, 255})},
        {"mediumslateblue",cfromColor8({123, 104, 238, 255})},{"mediumspringgreen",cfromColor8({0, 250, 154, 255})},
        {"mediumturquoise",cfromColor8({72, 209, 204, 255})},{"mediumvioletred",cfromColor8({199, 21, 133, 255})},
        {"midnightblue",cfromColor8({25, 25, 112, 255})},{"mintcream",cfromColor8({245, 255, 250, 255})},
        {"mistyrose",cfromColor8({255, 228, 225, 255})},{"moccasin",cfromColor8({255, 228, 181, 255})},
        {"navajowhite",cfromColor8({255, 222, 173, 255})},{"navy",cfromColor8({0, 0, 128, 255})},
        {"oldlace",cfromColor8({253, 245, 230, 255})},{"olive",cfromColor8({128, 128, 0, 255})},
        {"olivedrab",cfromColor8({107, 142, 35, 255})},{"orange",cfromColor8({255, 165, 0, 255})},
        {"orangered",cfromColor8({255, 69, 0, 255})},{"orchid",cfromColor8({218, 112, 214, 255})},
        {"palegoldenrod",cfromColor8({238, 232, 170, 255})},{"palegreen",cfromColor8({152, 251, 152, 255})},
        {"paleturquoise",cfromColor8({175, 238, 238, 255})},{"palevioletred",cfromColor8({219, 112, 147, 255})},
        {"papayawhip",cfromColor8({255, 239, 213, 255})},{"peachpuff",cfromColor8({255, 218, 185, 255})},
        {"peru",cfromColor8({205, 133, 63, 255})},{"pink",cfromColor8({255, 192, 203, 255})},
        {"plum",cfromColor8({221, 160, 221, 255})},{"powderblue",cfromColor8({176, 224, 230, 255})},
        {"purple",cfromColor8({128, 0, 128, 255})},{"rebeccapurple",cfromColor8({102, 51, 153, 255})},
        {"red",cfromColor8({255, 0, 0, 255})},{"rosybrown",cfromColor8({188, 143, 143, 255})},
        {"royalblue",cfromColor8({65, 105, 225, 255})},{"saddlebrown",cfromColor8({139, 69, 19, 255})},
        {"salmon",cfromColor8({250, 128, 114, 255})},{"sandybrown",cfromColor8({244, 164, 96, 255})},
        {"seagreen",cfromColor8({46, 139, 87, 255})},{"seashell",cfromColor8({255, 245, 238, 255})},
        {"sienna",cfromColor8({160, 82, 45, 255})},{"silver",cfromColor8({192, 192, 192, 255})},
        {"skyblue",cfromColor8({135, 206, 235, 255})},{"slateblue",cfromColor8({106, 90, 205, 255})},
        {"slategray",cfromColor8({112, 128, 144, 255})},{"slategrey",cfromColor8({112, 128, 144, 255})},
        {"snow",cfromColor8({255, 250, 250, 255})},{"springgreen",cfromColor8({0, 255, 127, 255})},
        {"steelblue",cfromColor8({70, 130, 180, 255})},{"tan",cfromColor8({210, 180, 140, 255})},
        {"teal",cfromColor8({0, 128, 128, 255})},{"thistle",cfromColor8({216, 191, 216, 255})},
        {"tomato",cfromColor8({255, 99, 71, 255})},{"turquoise",cfromColor8({64, 224, 208, 255})},
        {"violet",cfromColor8({238, 130, 238, 255})},{"wheat",cfromColor8({245, 222, 179, 255})},
        {"white",cfromColor8({255, 255, 255, 255})},{"whitesmoke",cfromColor8({245, 245, 245, 255})},
        {"yellow",cfromColor8({255, 255, 0, 255})},{"yellowgreen",cfromColor8({154, 205, 50, 255})},
    };
    std::string lower = colorStr;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto it = named.find(lower);
    if (it != named.end()) {
        out = it->second;
        return true;
    }

    return false;
}

bromath::Color DrawTraversal::parseColor(const std::string& color) {
    bromath::Color c = cfromColor8({0, 0, 0, 255});
    tryParseColor(color, c);
    return c;
}

} // namespace bro::layout
