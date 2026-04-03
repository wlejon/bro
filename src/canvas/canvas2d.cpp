#include "canvas/canvas2d.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

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
    if (str.substr(0, 3) == "hsl") {
        auto p = str.find('(');
        auto e = str.find(')');
        if (p == std::string::npos || e == std::string::npos) return false;
        std::string inner = str.substr(p + 1, e - p - 1);
        float vals[4] = {0, 0, 0, 1.0f};
        std::istringstream iss(inner);
        std::string tok;
        for (int i = 0; i < 4 && std::getline(iss, tok, ','); i++) {
            vals[i] = std::strtof(tok.c_str(), nullptr);
        }
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
        r = (uint8_t)((rf + m) * 255.0f);
        g = (uint8_t)((gf + m) * 255.0f);
        b = (uint8_t)((bf + m) * 255.0f);
        a = (uint8_t)(std::min(1.0f, std::max(0.0f, vals[3])) * 255.0f);
        return true;
    }

    // rgb(r,g,b) / rgba(r,g,b,a)
    if (str.substr(0, 4) == "rgba") {
        auto p = str.find('(');
        auto e = str.find(')');
        if (p == std::string::npos || e == std::string::npos) return false;
        std::string inner = str.substr(p + 1, e - p - 1);
        float vals[4] = {0, 0, 0, 1.0f};
        std::istringstream iss(inner);
        std::string tok;
        for (int i = 0; i < 4 && std::getline(iss, tok, ','); i++) {
            vals[i] = std::strtof(tok.c_str(), nullptr);
        }
        r = (uint8_t)std::min(255.0f, std::max(0.0f, vals[0]));
        g = (uint8_t)std::min(255.0f, std::max(0.0f, vals[1]));
        b = (uint8_t)std::min(255.0f, std::max(0.0f, vals[2]));
        a = (uint8_t)(std::min(1.0f, std::max(0.0f, vals[3])) * 255.0f);
        return true;
    }
    if (str.substr(0, 3) == "rgb") {
        auto p = str.find('(');
        auto e = str.find(')');
        if (p == std::string::npos || e == std::string::npos) return false;
        std::string inner = str.substr(p + 1, e - p - 1);
        float vals[3] = {0, 0, 0};
        std::istringstream iss(inner);
        std::string tok;
        for (int i = 0; i < 3 && std::getline(iss, tok, ','); i++) {
            vals[i] = std::strtof(tok.c_str(), nullptr);
        }
        r = (uint8_t)std::min(255.0f, std::max(0.0f, vals[0]));
        g = (uint8_t)std::min(255.0f, std::max(0.0f, vals[1]));
        b = (uint8_t)std::min(255.0f, std::max(0.0f, vals[2]));
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

    std::istringstream iss(font);
    std::string token;
    std::vector<std::string> tokens;
    while (iss >> token) tokens.push_back(token);

    for (size_t i = 0; i < tokens.size(); i++) {
        auto& t = tokens[i];
        if (t == "bold") { pf.weight = 700; continue; }
        if (t == "italic") { pf.italic = true; continue; }
        if (t == "normal") continue;

        // Check for size (e.g., "16px", "20pt")
        bool isSize = false;
        for (size_t j = 0; j < t.size(); j++) {
            if (std::isdigit(static_cast<unsigned char>(t[j])) || t[j] == '.') {
                isSize = true;
            } else if (isSize) {
                pf.size = std::strtof(t.c_str(), nullptr);
                // Remaining tokens are the family
                if (i + 1 < tokens.size()) {
                    pf.family.clear();
                    for (size_t k = i + 1; k < tokens.size(); k++) {
                        if (!pf.family.empty()) pf.family += ' ';
                        pf.family += tokens[k];
                    }
                }
                return pf;
            }
        }
    }

    return pf;
}

} // namespace bro::canvas
