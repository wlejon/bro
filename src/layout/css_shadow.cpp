#include "layout/css_shadow.h"
#include "layout/draw_traversal.h"

#include <cctype>
#include <cstdlib>

namespace bro::layout {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
    return s;
}

// Whitespace-separated tokens at paren depth 0, so `rgb(0 0 0 / 50%)` stays
// one token rather than lending its channels to the offsets.
std::vector<std::string_view> topLevelTokens(std::string_view s) {
    std::vector<std::string_view> out;
    int depth = 0;
    size_t start = std::string_view::npos;
    for (size_t i = 0; i <= s.size(); ++i) {
        const bool end = i == s.size();
        const char c = end ? ' ' : s[i];
        if (c == '(') ++depth;
        else if (c == ')' && depth > 0) --depth;
        if (isSpace(c) && depth == 0) {
            if (start != std::string_view::npos) out.push_back(s.substr(start, i - start));
            start = std::string_view::npos;
        } else if (start == std::string_view::npos) {
            start = i;
        }
    }
    return out;
}

// A <length> token: a number with an optional unit (`0`, `2px`, `-1.5px`).
// Units other than px are taken at face value, as the painter did before.
bool parseLengthToken(std::string_view tok, float& out) {
    if (tok.empty()) return false;
    const char c0 = tok[0];
    if (!(std::isdigit(static_cast<unsigned char>(c0)) || c0 == '.' || c0 == '-' || c0 == '+'))
        return false;
    std::string s(tok);
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str()) return false;
    for (const char* p = end; *p; ++p)
        if (!std::isalpha(static_cast<unsigned char>(*p))) return false;
    out = v;
    return true;
}

} // namespace

bool parseCssShadow(std::string_view item, const bromath::Color& currentColor, int maxLengths,
                    CssShadow& out) {
    out = CssShadow{};
    out.color = currentColor;
    float lengths[4] = {0, 0, 0, 0};
    int count = 0;
    bool haveColor = false;
    for (std::string_view tok : topLevelTokens(trim(item))) {
        float v = 0;
        if (tok == "inset") {
            if (out.inset) return false;
            out.inset = true;
        } else if (parseLengthToken(tok, v)) {
            if (count >= maxLengths || count >= 4) return false;
            lengths[count++] = v;
        } else {
            if (haveColor) return false;
            haveColor = true;
            // A bare `currentcolor` is not parsed here (it has no element);
            // the colour stays the one passed in, which is what it means.
            DrawTraversal::tryParseColor(std::string(tok), out.color);
        }
    }
    if (count < 2) return false;
    out.dx = lengths[0];
    out.dy = lengths[1];
    out.blur = lengths[2];
    out.spread = lengths[3];
    return true;
}

std::vector<std::string> splitCssShadowList(std::string_view list) {
    std::vector<std::string> out;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i <= list.size(); ++i) {
        const bool end = i == list.size();
        const char c = end ? ',' : list[i];
        if (c == '(') ++depth;
        else if (c == ')' && depth > 0) --depth;
        else if (c == ',' && depth == 0) {
            std::string_view s = trim(list.substr(start, i - start));
            if (!s.empty()) out.emplace_back(s);
            start = i + 1;
        }
    }
    return out;
}

} // namespace bro::layout
