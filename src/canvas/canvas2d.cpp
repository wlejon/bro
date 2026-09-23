#include "canvas/canvas2d.h"
#include "render/renderer.h"

#include <bromath/color.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

namespace bro::canvas {

// --- Named CSS colors (common subset) ---
struct NamedColor { const char* name; uint8_t r, g, b; };
static const NamedColor NAMED_COLORS[] = {
    {"black",   0,0,0},       {"white",   255,255,255}, {"red",     255,0,0},
    {"green",   0,128,0},     {"blue",    0,0,255},     {"yellow",  255,255,0},
    {"cyan",    0,255,255},   {"magenta", 255,0,255},   {"orange",  255,165,0},
    {"purple",  128,0,128},   {"gray",    128,128,128}, {"grey",    128,128,128},
    {"silver",  192,192,192}, {"maroon",  128,0,0},     {"navy",    0,0,128},
    {"teal",    0,128,128},   {"lime",    0,255,0},     {"aqua",    0,255,255},
    {"fuchsia", 255,0,255},   {"olive",   128,128,0},   {"brown",   165,42,42},
    {"pink",    255,192,203}, {"gold",    255,215,0},   {"coral",   255,127,80},
    {"tomato",  255,99,71},   {"crimson", 220,20,60},   {"indigo",  75,0,130},
    {"violet",  238,130,238}, {"salmon",  250,128,114}, {"khaki",   240,230,140},
    {"plum",    221,160,221}, {"tan",     210,180,140}, {"beige",   245,245,220},
    {"ivory",   255,255,240}, {"linen",   250,240,230}, {"snow",    255,250,250},
    {"transparent", 0,0,0},   // alpha=0 handled specially
};

/**
 * Scan up to `max` numbers out of a CSS functional-notation argument list.
 *
 * This replaces an std::istringstream + std::getline tokeniser, which is what
 * `rgba(...)` used to cost: constructing an istringstream drags in locale and
 * sentry machinery and allocates, and it happened once per assignment of
 * ctx.fillStyle / ctx.strokeStyle. Measured on a 40,000-assignment loop,
 * `rgba(150,190,235,0.4)` took 0.87 us per parse against 0.12 us for the
 * equivalent `#rrggbb` — a 7x penalty for writing the colour the way almost
 * every generated stylesheet writes it.
 *
 * `str` is a std::string and therefore NUL-terminated, so strtof cannot run off
 * the end; it stops at ')' or any other non-numeric byte regardless of `end`.
 * Separators are commas, whitespace, and the CSS Color 4 slash before alpha; a
 * '%' suffix is consumed and its meaning left to the caller, which is what the
 * previous tokeniser did implicitly by letting strtof stop at it.
 */
static int parseNumberList(const char* p, const char* end, float* out, int max) {
    int n = 0;
    while (n < max && p < end) {
        while (p < end && (*p == ' ' || *p == ',' || *p == '	' || *p == '/')) ++p;
        if (p >= end) break;
        char* q = nullptr;
        float v = std::strtof(p, &q);
        if (q == p || q == nullptr) break;
        out[n++] = v;
        p = q;
        if (p < end && *p == '%') ++p;
    }
    return n;
}

/** Case-sensitive prefix test that does not allocate, unlike substr(0,n)==lit. */
static bool startsWith(const std::string& s, const char* lit) {
    return s.compare(0, std::strlen(lit), lit) == 0;
}

// CSS Color 4 rounds to the nearest byte; truncating made rgba(0,0,0,0.5)
// alpha 127 (0.498) where every browser keeps 128, and the serialized form
// then failed to read back as the value that was set.
static uint8_t unitToByte(float v) {
    return static_cast<uint8_t>(std::lround(std::min(1.0f, std::max(0.0f, v)) * 255.0f));
}
static uint8_t channelToByte(float v) {
    return static_cast<uint8_t>(std::lround(std::min(255.0f, std::max(0.0f, v))));
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

bool parseCSSColor(const std::string& str, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    if (str.empty()) return false;
    a = 255;

    // Hex formats
    if (str[0] == '#') {
        if (str.size() == 4) { // #RGB
            int rv = hexVal(str[1]), gv = hexVal(str[2]), bv = hexVal(str[3]);
            if (rv < 0 || gv < 0 || bv < 0) return false;
            r = (uint8_t)(rv * 17); g = (uint8_t)(gv * 17); b = (uint8_t)(bv * 17);
            return true;
        }
        if (str.size() == 7) { // #RRGGBB
            int rv = hexVal(str[1]) * 16 + hexVal(str[2]);
            int gv = hexVal(str[3]) * 16 + hexVal(str[4]);
            int bv = hexVal(str[5]) * 16 + hexVal(str[6]);
            if (rv < 0 || gv < 0 || bv < 0) return false;
            r = (uint8_t)rv; g = (uint8_t)gv; b = (uint8_t)bv;
            return true;
        }
        if (str.size() == 9) { // #RRGGBBAA
            int rv = hexVal(str[1]) * 16 + hexVal(str[2]);
            int gv = hexVal(str[3]) * 16 + hexVal(str[4]);
            int bv = hexVal(str[5]) * 16 + hexVal(str[6]);
            int av = hexVal(str[7]) * 16 + hexVal(str[8]);
            if (rv < 0 || gv < 0 || bv < 0 || av < 0) return false;
            r = (uint8_t)rv; g = (uint8_t)gv; b = (uint8_t)bv; a = (uint8_t)av;
            return true;
        }
        return false;
    }

    // hsl(h, s%, l%) / hsla(h, s%, l%, a)
    if (startsWith(str, "hsl")) {
        auto p = str.find('(');
        auto e = str.find(')');
        if (p == std::string::npos || e == std::string::npos) return false;
        float vals[4] = {0, 0, 0, 1.0f};
        parseNumberList(str.data() + p + 1, str.data() + e, vals, 4);
        float h = std::fmod(vals[0], 360.0f);
        if (h < 0) h += 360.0f;
        float s = std::min(100.0f, std::max(0.0f, vals[1])) / 100.0f;
        float l = std::min(100.0f, std::max(0.0f, vals[2])) / 100.0f;
        // HSL to RGB conversion
        float c = (1.0f - std::abs(2.0f * l - 1.0f)) * s;
        float x = c * (1.0f - std::abs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
        float m = l - c / 2.0f;
        float rf, gf, bf;
        if      (h < 60)  { rf = c; gf = x; bf = 0; }
        else if (h < 120) { rf = x; gf = c; bf = 0; }
        else if (h < 180) { rf = 0; gf = c; bf = x; }
        else if (h < 240) { rf = 0; gf = x; bf = c; }
        else if (h < 300) { rf = x; gf = 0; bf = c; }
        else              { rf = c; gf = 0; bf = x; }
        r = unitToByte(rf + m);
        g = unitToByte(gf + m);
        b = unitToByte(bf + m);
        a = unitToByte(vals[3]);
        return true;
    }

    // rgb(r,g,b) / rgba(r,g,b,a)
    if (startsWith(str, "rgba")) {
        auto p = str.find('(');
        auto e = str.find(')');
        if (p == std::string::npos || e == std::string::npos) return false;
        float vals[4] = {0, 0, 0, 1.0f};
        parseNumberList(str.data() + p + 1, str.data() + e, vals, 4);
        r = channelToByte(vals[0]);
        g = channelToByte(vals[1]);
        b = channelToByte(vals[2]);
        a = unitToByte(vals[3]);
        return true;
    }
    if (startsWith(str, "rgb")) {
        auto p = str.find('(');
        auto e = str.find(')');
        if (p == std::string::npos || e == std::string::npos) return false;
        // CSS Color 4 allows an alpha in plain rgb() too: rgb(1 2 3 / 0.5).
        float vals[4] = {0, 0, 0, 1.0f};
        parseNumberList(str.data() + p + 1, str.data() + e, vals, 4);
        a = unitToByte(vals[3]);
        r = channelToByte(vals[0]);
        g = channelToByte(vals[1]);
        b = channelToByte(vals[2]);
        return true;
    }

    // Named colors
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (auto& nc : NAMED_COLORS) {
        if (lower == nc.name) {
            r = nc.r; g = nc.g; b = nc.b;
            if (lower == "transparent") a = 0;
            return true;
        }
    }
    return false;
}

ParsedFont parseCSSFont(const std::string& font) {
    ParsedFont pf;
    pf.family = "sans-serif";
    pf.size = 16.0f;
    pf.weight = 400;
    pf.italic = false;

    size_t pos = 0, n = font.size();
    while (pos < n) {
        while (pos < n && std::isspace(static_cast<unsigned char>(font[pos]))) pos++;
        size_t start = pos;
        while (pos < n && !std::isspace(static_cast<unsigned char>(font[pos]))) pos++;
        if (start == pos) break;
        std::string t = font.substr(start, pos - start);

        if (t == "bold") { pf.weight = 700; continue; }
        if (t == "italic") { pf.italic = true; continue; }
        if (t == "normal") continue;

        // Numeric font-weight keyword (100..900), no unit suffix.
        bool allDigits = !t.empty();
        for (char c : t) if (!std::isdigit(static_cast<unsigned char>(c))) { allDigits = false; break; }
        if (allDigits) {
            int w = std::atoi(t.c_str());
            if (w >= 100 && w <= 900) { pf.weight = w; continue; }
        }

        // Size token (e.g., "16px", "20pt"): digits/decimal point followed
        // by a non-digit unit suffix.
        bool isSize = false;
        for (size_t j = 0; j < t.size(); j++) {
            if (std::isdigit(static_cast<unsigned char>(t[j])) || t[j] == '.') {
                isSize = true;
            } else if (isSize) {
                pf.size = std::strtof(t.c_str(), nullptr);
                // Everything after this token is the family list, taken
                // verbatim — commas and internal spaces (e.g. "Segoe UI")
                // must survive for comma-separated fallback resolution.
                std::string rest = pos < n ? font.substr(pos) : std::string();
                size_t a = rest.find_first_not_of(" \t\n\r");
                if (a != std::string::npos) {
                    size_t b = rest.find_last_not_of(" \t\n\r");
                    pf.family = rest.substr(a, b - a + 1);
                }
                return pf;
            }
        }
    }

    return pf;
}

// ---------------------------------------------------------------------------
// ctx.filter
// ---------------------------------------------------------------------------

namespace {

std::string asciiLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// A number followed by its unit, the unit lower-cased. False when the token
// does not start with a number.
bool splitDimension(const std::string& tok, double& v, std::string& unit) {
    if (tok.empty()) return false;
    char* end = nullptr;
    v = std::strtod(tok.c_str(), &end);
    if (end == tok.c_str() || !std::isfinite(v)) return false;
    unit = asciiLower(std::string(end));
    return true;
}

// <length> in CSS px. Absolute units only: the relative ones would need the
// canvas's font and viewport, which this layer does not have; a unitless
// number is accepted only as zero.
bool parseLength(const std::string& tok, float& out) {
    double v; std::string u;
    if (!splitDimension(tok, v, u)) return false;
    double k;
    if (u == "px") k = 1.0;
    else if (u.empty()) { if (v != 0.0) return false; k = 1.0; }
    else if (u == "in") k = 96.0;
    else if (u == "cm") k = 96.0 / 2.54;
    else if (u == "mm") k = 96.0 / 25.4;
    else if (u == "q")  k = 96.0 / 101.6;
    else if (u == "pt") k = 96.0 / 72.0;
    else if (u == "pc") k = 16.0;
    else return false;
    out = static_cast<float>(v * k);
    return true;
}

// <number> | <percentage>, non-negative.
bool parseAmount(const std::string& tok, float& out) {
    double v; std::string u;
    if (!splitDimension(tok, v, u)) return false;
    if (u == "%") v /= 100.0;
    else if (!u.empty()) return false;
    if (v < 0.0) return false;
    out = static_cast<float>(v);
    return true;
}

// <angle> in degrees; a unitless number only as zero.
bool parseAngle(const std::string& tok, float& out) {
    double v; std::string u;
    if (!splitDimension(tok, v, u)) return false;
    if (u == "deg") {}
    else if (u == "rad") v = v * 180.0 / 3.14159265358979323846;
    else if (u == "grad") v = v * 0.9;
    else if (u == "turn") v = v * 360.0;
    else if (u.empty()) { if (v != 0.0) return false; }
    else return false;
    out = static_cast<float>(v);
    return true;
}

// Split a function's argument text on whitespace (and commas) outside any
// nested parentheses, so `rgba(0, 0, 0, 0.5)` stays one token.
std::vector<std::string> splitArgs(const std::string& s) {
    std::vector<std::string> toks;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        if (depth == 0 && (isSpace(c) || c == ',')) {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) toks.push_back(cur);
    return toks;
}

}  // namespace

bool parseCanvasFilter(const std::string& str, std::vector<render::CssFilterParams>& out) {
    out.clear();
    size_t a = 0, b = str.size();
    while (a < b && isSpace(str[a])) ++a;
    while (b > a && isSpace(str[b - 1])) --b;
    if (a == b) return false;  // the empty string is not a filter value
    if (asciiLower(str.substr(a, b - a)) == "none") return true;

    size_t pos = a;
    while (pos < b) {
        while (pos < b && isSpace(str[pos])) ++pos;
        if (pos >= b) break;
        size_t nameStart = pos;
        while (pos < b && str[pos] != '(' && !isSpace(str[pos])) ++pos;
        if (pos >= b || str[pos] != '(') return false;
        const std::string name = asciiLower(str.substr(nameStart, pos - nameStart));
        ++pos;  // '('
        size_t argStart = pos;
        int depth = 0;
        while (pos < b) {
            if (str[pos] == '(') ++depth;
            else if (str[pos] == ')') { if (depth == 0) break; --depth; }
            ++pos;
        }
        if (pos >= b) return false;  // unterminated
        const std::vector<std::string> args = splitArgs(str.substr(argStart, pos - argStart));
        ++pos;  // ')'

        render::CssFilterParams f{};
        auto amount = [&](render::CssFilterParams::Kind kind, bool clampToOne) {
            f.kind = kind;
            f.a = 1.0f;  // every amount function defaults to 1
            if (args.size() > 1) return false;
            if (args.size() == 1 && !parseAmount(args[0], f.a)) return false;
            if (clampToOne && f.a > 1.0f) f.a = 1.0f;
            return true;
        };

        bool ok = true;
        if (name == "blur") {
            f.kind = render::CssFilterParams::Blur;
            f.a = 0.0f;
            if (args.size() > 1) ok = false;
            else if (args.size() == 1) ok = parseLength(args[0], f.a) && f.a >= 0.0f;
        } else if (name == "brightness") {
            ok = amount(render::CssFilterParams::Brightness, false);
        } else if (name == "contrast") {
            ok = amount(render::CssFilterParams::Contrast, false);
        } else if (name == "grayscale") {
            ok = amount(render::CssFilterParams::Grayscale, true);
        } else if (name == "invert") {
            ok = amount(render::CssFilterParams::Invert, true);
        } else if (name == "opacity") {
            ok = amount(render::CssFilterParams::Opacity, true);
        } else if (name == "saturate") {
            ok = amount(render::CssFilterParams::Saturate, false);
        } else if (name == "sepia") {
            ok = amount(render::CssFilterParams::Sepia, true);
        } else if (name == "hue-rotate") {
            f.kind = render::CssFilterParams::HueRotate;
            f.a = 0.0f;
            if (args.size() > 1) ok = false;
            else if (args.size() == 1) ok = parseAngle(args[0], f.a);
        } else if (name == "drop-shadow") {
            // <color>? && <length>{2,3}: the color may come first or last.
            f.kind = render::CssFilterParams::DropShadow;
            std::vector<float> lens;
            bool haveColor = false;
            uint8_t cr = 0, cg = 0, cb = 0, ca = 255;  // currentcolor -> black
            for (size_t i = 0; i < args.size() && ok; ++i) {
                float len;
                if (parseLength(args[i], len)) {
                    lens.push_back(len);
                } else if (!haveColor && (i == 0 || i + 1 == args.size())) {
                    // Only at either end, which keeps the lengths contiguous.
                    std::string c = asciiLower(args[i]);
                    if (c == "currentcolor") { haveColor = true; }
                    else if (parseCSSColor(args[i], cr, cg, cb, ca)) { haveColor = true; }
                    else ok = false;
                } else {
                    ok = false;
                }
            }
            if (ok && (lens.size() < 2 || lens.size() > 3)) ok = false;
            if (ok && lens.size() == 3 && lens[2] < 0.0f) ok = false;
            if (ok) {
                f.dx = lens[0];
                f.dy = lens[1];
                f.blur = lens.size() == 3 ? lens[2] : 0.0f;
                f.shadowColor = bromath::cfromColor8({cr, cg, cb, ca});
            }
        } else {
            // url() (an SVG <filter> reference) and anything unknown.
            ok = false;
        }
        if (!ok) return false;
        out.push_back(f);
    }
    return !out.empty();
}

} // namespace bro::canvas
