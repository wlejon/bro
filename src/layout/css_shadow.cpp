#include "layout/css_shadow.h"
#include "layout/draw_traversal.h"
#include "layout/formatting_context.h"  // htmlayout::layout::resolveLength (calc)

#include <algorithm>
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

bool startsWithCi(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != prefix[i]) return false;
    return true;
}

// A number followed by `unit`, in px.
bool unitToPx(float v, std::string_view unit, const CssLengthContext& cx, float& out) {
    std::string u(unit);
    for (auto& ch : u) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const float vw = cx.viewportW, vh = cx.viewportH;
    if (u.empty() || u == "px") out = v;
    else if (u == "em") out = v * cx.fontSize;
    else if (u == "rem") out = v * cx.rootFontSize;
    else if (u == "ch" || u == "ex") out = v * cx.fontSize * 0.5f;
    else if (u == "vw") out = v * vw / 100.0f;
    else if (u == "vh") out = v * vh / 100.0f;
    else if (u == "vmin") out = v * std::min(vw, vh) / 100.0f;
    else if (u == "vmax") out = v * std::max(vw, vh) / 100.0f;
    else if (u == "pt") out = v * 96.0f / 72.0f;
    else if (u == "pc") out = v * 16.0f;
    else if (u == "in") out = v * 96.0f;
    else if (u == "cm") out = v * 96.0f / 2.54f;
    else if (u == "mm") out = v * 96.0f / 25.4f;
    else if (u == "q") out = v * 96.0f / 101.6f;
    else return false;
    return true;
}

// A <length> token: a number with a unit (`0`, `2px`, `-1.5em`), or a math
// function (`calc(1em + 2px)`), resolved to px.
bool parseLengthToken(std::string_view tok, const CssLengthContext& cx, float& out) {
    if (tok.empty()) return false;
    if (startsWithCi(tok, "calc(") || startsWithCi(tok, "min(") || startsWithCi(tok, "max(") ||
        startsWithCi(tok, "clamp(")) {
        out = htmlayout::layout::resolveLength(std::string(tok), 0.0f, cx.fontSize,
                                               cx.viewportW, cx.viewportH);
        return true;
    }
    const char c0 = tok[0];
    if (!(std::isdigit(static_cast<unsigned char>(c0)) || c0 == '.' || c0 == '-' || c0 == '+'))
        return false;
    std::string s(tok);
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str()) return false;
    for (const char* p = end; *p; ++p)
        if (!std::isalpha(static_cast<unsigned char>(*p))) return false;
    return unitToPx(v, std::string_view(end), cx, out);
}

} // namespace

bool resolveCssLength(std::string_view token, const CssLengthContext& lengths, float& out) {
    return parseLengthToken(trim(token), lengths, out);
}

std::vector<CssShadow> parseCssShadowList(std::string_view list,
                                          const bromath::Color& currentColor, int maxLengths,
                                          const CssLengthContext& lengths) {
    std::vector<CssShadow> out;
    for (const std::string& item : splitCssShadowList(list)) {
        CssShadow s;
        if (parseCssShadow(item, currentColor, maxLengths, lengths, s)) out.push_back(s);
    }
    return out;
}

bool parseCssShadow(std::string_view item, const bromath::Color& currentColor, int maxLengths,
                    const CssLengthContext& cx, CssShadow& out) {
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
        } else if (parseLengthToken(tok, cx, v)) {
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
