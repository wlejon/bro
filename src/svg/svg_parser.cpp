#include "svg/svg_parser.h"
#include "util/log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <unordered_map>

namespace bro::svg {

namespace {

// ---------------------------------------------------------------------------
// Color parsing
// ---------------------------------------------------------------------------

static const std::unordered_map<std::string, render::Color> kNamedColors = {
    {"black",   {0,0,0,255}},       {"white",   {255,255,255,255}},
    {"red",     {255,0,0,255}},     {"green",   {0,128,0,255}},
    {"blue",    {0,0,255,255}},     {"yellow",  {255,255,0,255}},
    {"cyan",    {0,255,255,255}},   {"magenta", {255,0,255,255}},
    {"orange",  {255,165,0,255}},   {"purple",  {128,0,128,255}},
    {"pink",    {255,192,203,255}}, {"gray",    {128,128,128,255}},
    {"grey",    {128,128,128,255}}, {"silver",  {192,192,192,255}},
    {"maroon",  {128,0,0,255}},     {"olive",   {128,128,0,255}},
    {"lime",    {0,255,0,255}},     {"aqua",    {0,255,255,255}},
    {"teal",    {0,128,128,255}},   {"navy",    {0,0,128,255}},
    {"fuchsia", {255,0,255,255}},   {"brown",   {165,42,42,255}},
    {"coral",   {255,127,80,255}},  {"gold",    {255,215,0,255}},
    {"indigo",  {75,0,130,255}},    {"violet",  {238,130,238,255}},
    {"crimson", {220,20,60,255}},   {"tomato",  {255,99,71,255}},
    {"salmon",  {250,128,114,255}}, {"khaki",   {240,230,140,255}},
    {"plum",    {221,160,221,255}}, {"orchid",  {218,112,214,255}},
    {"sienna",  {160,82,45,255}},   {"peru",    {205,133,63,255}},
    {"tan",     {210,180,140,255}}, {"wheat",   {245,222,179,255}},
    {"linen",   {250,240,230,255}},
    {"steelblue",   {70,130,180,255}},
    {"dodgerblue",  {30,144,255,255}},
    {"royalblue",   {65,105,225,255}},
    {"cornflowerblue", {100,149,237,255}},
    {"darkblue",    {0,0,139,255}},
    {"darkred",     {139,0,0,255}},
    {"darkgreen",   {0,100,0,255}},
    {"darkorange",  {255,140,0,255}},
    {"darkviolet",  {148,0,211,255}},
    {"deeppink",    {255,20,147,255}},
    {"forestgreen", {34,139,34,255}},
    {"lightblue",   {173,216,230,255}},
    {"lightcoral",  {240,128,128,255}},
    {"lightgreen",  {144,238,144,255}},
    {"lightgray",   {211,211,211,255}},
    {"lightgrey",   {211,211,211,255}},
    {"lightyellow", {255,255,224,255}},
    {"mediumblue",  {0,0,205,255}},
    {"orangered",   {255,69,0,255}},
    {"slategray",   {112,128,144,255}},
    {"yellowgreen", {154,205,50,255}},
};

bool parseColor(const std::string& str, render::Color& out) {
    if (str.empty() || str == "none") return false;
    if (str == "currentColor") { out = {0, 0, 0, 255}; return true; }

    // Named colors
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto it = kNamedColors.find(lower);
    if (it != kNamedColors.end()) { out = it->second; return true; }

    // Hex
    if (str[0] == '#') {
        unsigned long val = 0;
        std::string hex = str.substr(1);
        if (hex.size() == 3) {
            // #RGB -> #RRGGBB
            hex = {hex[0], hex[0], hex[1], hex[1], hex[2], hex[2]};
        }
        if (hex.size() == 6 || hex.size() == 8) {
            val = std::strtoul(hex.c_str(), nullptr, 16);
            if (hex.size() == 6) {
                out.r = (val >> 16) & 0xFF;
                out.g = (val >> 8) & 0xFF;
                out.b = val & 0xFF;
                out.a = 255;
            } else {
                out.r = (val >> 24) & 0xFF;
                out.g = (val >> 16) & 0xFF;
                out.b = (val >> 8) & 0xFF;
                out.a = val & 0xFF;
            }
            return true;
        }
    }

    // rgb(r, g, b) / rgba(r, g, b, a)
    if (str.substr(0, 4) == "rgb(" || str.substr(0, 5) == "rgba(") {
        auto start = str.find('(');
        auto end = str.rfind(')');
        if (start != std::string::npos && end != std::string::npos) {
            std::string inner = str.substr(start + 1, end - start - 1);
            // Replace commas and slashes with spaces
            for (char& c : inner) { if (c == ',' || c == '/') c = ' '; }
            std::istringstream iss(inner);
            float r, g, b, a = 1.0f;
            if (iss >> r >> g >> b) {
                iss >> a; // optional alpha
                out.r = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
                out.g = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
                out.b = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
                out.a = static_cast<uint8_t>(std::clamp(a * 255.0f, 0.0f, 255.0f));
                return true;
            }
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Attribute helpers
// ---------------------------------------------------------------------------

std::string getAttr(const litehtml::element::ptr& el, const char* name) {
    const char* val = el->get_attr(name);
    return val ? val : "";
}

float getAttrFloat(const litehtml::element::ptr& el, const char* name, float def = 0.0f) {
    const char* val = el->get_attr(name);
    if (!val || !*val) return def;
    char* end = nullptr;
    float f = std::strtof(val, &end);
    return (end != val) ? f : def;
}

std::string getTagName(const litehtml::element::ptr& el) {
    const char* tag = el->get_tagName();
    if (!tag) return "";
    std::string result(tag);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

// Parse style attributes (fill, stroke, stroke-width, opacity) with inheritance
SvgStyle parseStyle(const litehtml::element::ptr& el, const SvgStyle& parentStyle) {
    SvgStyle style = parentStyle; // inherit from parent

    std::string fillStr = getAttr(el, "fill");
    if (!fillStr.empty()) {
        if (fillStr == "none") {
            style.hasFill = false;
            style.fill.a = 0;
        } else {
            render::Color c;
            if (parseColor(fillStr, c)) {
                style.fill = c;
                style.hasFill = true;
            }
        }
    }

    std::string strokeStr = getAttr(el, "stroke");
    if (!strokeStr.empty()) {
        if (strokeStr == "none") {
            style.hasStroke = false;
            style.stroke.a = 0;
        } else {
            render::Color c;
            if (parseColor(strokeStr, c)) {
                style.stroke = c;
                style.hasStroke = true;
            }
        }
    }

    const char* sw = el->get_attr("stroke-width");
    if (sw && *sw) {
        char* end = nullptr;
        float f = std::strtof(sw, &end);
        if (end != sw) style.strokeWidth = f;
    }

    const char* op = el->get_attr("opacity");
    if (op && *op) {
        char* end = nullptr;
        float f = std::strtof(op, &end);
        if (end != op) style.opacity = std::clamp(f, 0.0f, 1.0f);
    }

    // Apply opacity to fill and stroke alpha
    if (style.opacity < 1.0f) {
        style.fill.a = static_cast<uint8_t>(style.fill.a * style.opacity);
        style.stroke.a = static_cast<uint8_t>(style.stroke.a * style.opacity);
    }

    return style;
}

// Parse a points attribute "x1,y1 x2,y2 ..." or "x1 y1 x2 y2 ..."
std::vector<render::PointF> parsePoints(const std::string& str) {
    std::vector<render::PointF> pts;
    std::string s = str;
    for (char& c : s) { if (c == ',') c = ' '; }
    std::istringstream iss(s);
    float x, y;
    while (iss >> x >> y) {
        pts.push_back({x, y});
    }
    return pts;
}

// Get text content from a litehtml element's children
std::string getTextContent(const litehtml::element::ptr& el) {
    std::string text;
    for (auto& child : el->children()) {
        if (child && child->is_text()) {
            litehtml::string t;
            child->get_text(t);
            text += t;
        }
    }
    return text;
}

// Recursively parse SVG children
void parseChildren(const litehtml::element::ptr& el, const SvgStyle& parentStyle,
                   std::vector<SvgNode>& out) {
    for (auto& child : el->children()) {
        if (!child) continue;

        std::string tag = getTagName(child);
        if (tag.empty()) continue; // text node

        SvgStyle style = parseStyle(child, parentStyle);
        SvgNode node{};
        node.style = style;

        if (tag == "rect") {
            node.type = SvgNodeType::Rect;
            node.x = getAttrFloat(child, "x");
            node.y = getAttrFloat(child, "y");
            node.width = getAttrFloat(child, "width");
            node.height = getAttrFloat(child, "height");
            node.rx = getAttrFloat(child, "rx");
            node.ry = getAttrFloat(child, "ry");
            // SVG spec: if only one of rx/ry is set, the other defaults to it
            if (node.rx > 0 && node.ry == 0) node.ry = node.rx;
            if (node.ry > 0 && node.rx == 0) node.rx = node.ry;
            out.push_back(std::move(node));
        } else if (tag == "circle") {
            node.type = SvgNodeType::Circle;
            node.cx = getAttrFloat(child, "cx");
            node.cy = getAttrFloat(child, "cy");
            node.r = getAttrFloat(child, "r");
            out.push_back(std::move(node));
        } else if (tag == "ellipse") {
            node.type = SvgNodeType::Ellipse;
            node.cx = getAttrFloat(child, "cx");
            node.cy = getAttrFloat(child, "cy");
            node.rx = getAttrFloat(child, "rx");
            node.ry = getAttrFloat(child, "ry");
            out.push_back(std::move(node));
        } else if (tag == "line") {
            node.type = SvgNodeType::Line;
            node.x1 = getAttrFloat(child, "x1");
            node.y1 = getAttrFloat(child, "y1");
            node.x2 = getAttrFloat(child, "x2");
            node.y2 = getAttrFloat(child, "y2");
            // Lines default to stroke visible
            if (!style.hasStroke && getAttr(child, "stroke").empty()) {
                node.style.stroke = parentStyle.fill; // use parent fill as stroke
                node.style.hasStroke = true;
            }
            out.push_back(std::move(node));
        } else if (tag == "polyline") {
            node.type = SvgNodeType::Polyline;
            node.points = parsePoints(getAttr(child, "points"));
            out.push_back(std::move(node));
        } else if (tag == "polygon") {
            node.type = SvgNodeType::Polygon;
            node.points = parsePoints(getAttr(child, "points"));
            out.push_back(std::move(node));
        } else if (tag == "path") {
            node.type = SvgNodeType::Path;
            node.pathData = getAttr(child, "d");
            out.push_back(std::move(node));
        } else if (tag == "text") {
            node.type = SvgNodeType::Text;
            node.x = getAttrFloat(child, "x");
            node.y = getAttrFloat(child, "y");
            node.textContent = getTextContent(child);
            const char* fs = child->get_attr("font-size");
            if (fs && *fs) {
                char* end = nullptr;
                float f = std::strtof(fs, &end);
                if (end != fs) node.fontSize = f;
            }
            std::string ff = getAttr(child, "font-family");
            if (!ff.empty()) node.fontFamily = ff;
            out.push_back(std::move(node));
        } else if (tag == "g") {
            node.type = SvgNodeType::Group;
            parseChildren(child, style, node.children);
            out.push_back(std::move(node));
        }
        // Skip unknown tags (defs, clipPath, etc.)
    }
}

} // anonymous namespace

SvgRoot parseSvgTree(const litehtml::element::ptr& svgElement) {
    SvgRoot root;

    root.width = getAttrFloat(svgElement, "width", 300.0f);
    root.height = getAttrFloat(svgElement, "height", 150.0f);

    // Parse viewBox — try both cases since HTML parsers may lowercase it
    std::string viewBox = getAttr(svgElement, "viewBox");
    if (viewBox.empty()) viewBox = getAttr(svgElement, "viewbox");
    if (!viewBox.empty()) {
        for (char& c : viewBox) { if (c == ',') c = ' '; }
        std::istringstream iss(viewBox);
        if (iss >> root.viewBoxX >> root.viewBoxY >> root.viewBoxW >> root.viewBoxH) {
            root.hasViewBox = true;
        }
    }

    SvgStyle defaultStyle;
    defaultStyle = parseStyle(svgElement, defaultStyle);
    parseChildren(svgElement, defaultStyle, root.children);

    return root;
}

} // namespace bro::svg
