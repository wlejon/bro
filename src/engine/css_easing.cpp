#include "engine/css_easing.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace bro::engine {

namespace {

constexpr bromath::CubicEase kEaseC{0.25f, 0.1f, 0.25f, 1.0f};
constexpr bromath::CubicEase kEaseInC{0.42f, 0.0f, 1.0f, 1.0f};
constexpr bromath::CubicEase kEaseOutC{0.0f, 0.0f, 0.58f, 1.0f};
constexpr bromath::CubicEase kEaseInOutC{0.42f, 0.0f, 0.58f, 1.0f};

bool sameCubic(const bromath::CubicEase& a, const bromath::CubicEase& b) {
    return a.p1x == b.p1x && a.p1y == b.p1y && a.p2x == b.p2x && a.p2y == b.p2y;
}

std::string trimLower(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    std::string out = s.substr(b, e - b);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The argument text of `name(...)`, or false when `s` is not that function.
bool functionArgs(const std::string& s, const char* name, std::string& args) {
    const size_t n = std::char_traits<char>::length(name);
    if (s.size() < n + 2 || s.compare(0, n, name) != 0) return false;
    size_t i = n;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size() || s[i] != '(' || s.back() != ')') return false;
    args = s.substr(i + 1, s.size() - i - 2);
    return true;
}

std::vector<std::string> splitCommas(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',') {
            std::string part = s.substr(start, i - start);
            size_t b = 0, e = part.size();
            while (b < e && std::isspace(static_cast<unsigned char>(part[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(part[e - 1]))) --e;
            out.push_back(part.substr(b, e - b));
            start = i + 1;
        }
    }
    return out;
}

bool parseNumber(const std::string& s, float& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtof(s.c_str(), &end);
    return end == s.c_str() + s.size() && std::isfinite(out);
}

// Whitespace-separated tokens of one linear() stop.
std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        size_t b = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i > b) out.push_back(s.substr(b, i - b));
    }
    return out;
}

bool parseLinearFunction(const std::string& args, TimingFunction& out) {
    // Each stop: <number> [<percentage>{1,2}]. Missing inputs are filled in
    // afterwards (CSS Easing 2 §3.1 "canonicalize a linear() function").
    struct Stop { float output; float input; bool hasInput; };
    std::vector<Stop> stops;
    for (const std::string& part : splitCommas(args)) {
        std::vector<std::string> tk = tokens(part);
        if (tk.empty() || tk.size() > 3) return false;
        float outV = 0;
        if (!parseNumber(tk[0], outV)) return false;
        std::vector<float> ins;
        for (size_t k = 1; k < tk.size(); ++k) {
            const std::string& p = tk[k];
            if (p.size() < 2 || p.back() != '%') return false;
            float v = 0;
            if (!parseNumber(p.substr(0, p.size() - 1), v)) return false;
            ins.push_back(v / 100.0f);
        }
        if (ins.empty()) stops.push_back({outV, 0, false});
        for (float in : ins) stops.push_back({outV, in, true});
    }
    if (stops.size() < 2) return false;
    if (!stops.front().hasInput) { stops.front().input = 0; stops.front().hasInput = true; }
    if (!stops.back().hasInput) { stops.back().input = 1; stops.back().hasInput = true; }
    // Inputs never decrease.
    float maxIn = stops.front().input;
    for (auto& s : stops) {
        if (!s.hasInput) continue;
        if (s.input < maxIn) s.input = maxIn;
        maxIn = s.input;
    }
    // Runs without an input spread evenly between their neighbours.
    for (size_t i = 0; i < stops.size();) {
        if (stops[i].hasInput) { ++i; continue; }
        size_t runStart = i;
        while (i < stops.size() && !stops[i].hasInput) ++i;
        float lo = stops[runStart - 1].input, hi = stops[i].input;
        size_t count = i - runStart + 1;
        for (size_t k = runStart; k < i; ++k) {
            stops[k].input = lo + (hi - lo) * static_cast<float>(k - runStart + 1) / count;
            stops[k].hasInput = true;
        }
    }
    out = TimingFunction{};
    out.kind = TimingFunction::Kind::Linear;
    for (auto& s : stops) out.points.emplace_back(s.input, s.output);
    return true;
}

std::string num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return buf;
}

}  // namespace

float TimingFunction::apply(float t, bool beforeFlag) const {
    switch (kind) {
        case Kind::Cubic:
            if (isLinear()) return t;
            return bromath::ccubicEase(cubic, t);
        case Kind::Steps: {
            // CSS Easing 2 §4.2.
            const float scaled = t * static_cast<float>(steps);
            int step = static_cast<int>(std::floor(scaled));
            if (position == StepPosition::JumpStart || position == StepPosition::JumpBoth) ++step;
            if (beforeFlag && std::floor(scaled) == scaled) --step;
            if (t >= 0 && step < 0) step = 0;
            int jumps = steps;
            if (position == StepPosition::JumpNone) jumps = steps - 1;
            else if (position == StepPosition::JumpBoth) jumps = steps + 1;
            if (t <= 1 && step > jumps) step = jumps;
            return jumps > 0 ? static_cast<float>(step) / static_cast<float>(jumps) : t;
        }
        case Kind::Linear: {
            if (points.empty()) return t;
            if (points.size() == 1) return points[0].second;
            // The segment holding t; the end segments extrapolate.
            size_t i = 0;
            while (i + 2 < points.size() && t >= points[i + 1].first) ++i;
            const auto& a = points[i];
            const auto& b = points[i + 1];
            if (b.first == a.first) return t < a.first ? a.second : b.second;
            float u = (t - a.first) / (b.first - a.first);
            return a.second + (b.second - a.second) * u;
        }
    }
    return t;
}

std::string TimingFunction::toString() const {
    switch (kind) {
        case Kind::Cubic:
            if (sameCubic(cubic, {0.0f, 0.0f, 1.0f, 1.0f})) return "linear";
            if (sameCubic(cubic, kEaseC)) return "ease";
            if (sameCubic(cubic, kEaseInC)) return "ease-in";
            if (sameCubic(cubic, kEaseOutC)) return "ease-out";
            if (sameCubic(cubic, kEaseInOutC)) return "ease-in-out";
            return "cubic-bezier(" + num(cubic.p1x) + ", " + num(cubic.p1y) + ", " +
                   num(cubic.p2x) + ", " + num(cubic.p2y) + ")";
        case Kind::Steps: {
            std::string s = "steps(" + std::to_string(steps);
            switch (position) {
                case StepPosition::JumpEnd: break;
                case StepPosition::JumpStart: s += ", start"; break;
                case StepPosition::JumpNone: s += ", jump-none"; break;
                case StepPosition::JumpBoth: s += ", jump-both"; break;
            }
            return s + ")";
        }
        case Kind::Linear: {
            std::string s = "linear(";
            for (size_t i = 0; i < points.size(); ++i) {
                if (i) s += ", ";
                s += num(points[i].second) + " " + num(points[i].first * 100.0f) + "%";
            }
            return s + ")";
        }
    }
    return "linear";
}

bool TimingFunction::operator==(const TimingFunction& o) const {
    if (kind != o.kind) return false;
    switch (kind) {
        case Kind::Cubic: return sameCubic(cubic, o.cubic);
        case Kind::Steps: return steps == o.steps && position == o.position;
        case Kind::Linear: return points == o.points;
    }
    return false;
}

bool tryParseEasing(const std::string& text, TimingFunction& out) {
    const std::string s = trimLower(text);
    if (s == "linear") { out = TimingFunction::linear(); return true; }
    if (s == "ease") { out = TimingFunction::fromCubic(kEaseC); return true; }
    if (s == "ease-in") { out = TimingFunction::fromCubic(kEaseInC); return true; }
    if (s == "ease-out") { out = TimingFunction::fromCubic(kEaseOutC); return true; }
    if (s == "ease-in-out") { out = TimingFunction::fromCubic(kEaseInOutC); return true; }
    if (s == "step-start" || s == "step-end") {
        TimingFunction f;
        f.kind = TimingFunction::Kind::Steps;
        f.steps = 1;
        f.position = s == "step-start" ? TimingFunction::StepPosition::JumpStart
                                       : TimingFunction::StepPosition::JumpEnd;
        out = f;
        return true;
    }
    std::string args;
    if (functionArgs(s, "cubic-bezier", args)) {
        std::vector<std::string> parts = splitCommas(args);
        if (parts.size() != 4) return false;
        float v[4];
        for (int i = 0; i < 4; ++i)
            if (!parseNumber(parts[i], v[i])) return false;
        if (v[0] < 0 || v[0] > 1 || v[2] < 0 || v[2] > 1) return false;
        out = TimingFunction::fromCubic({v[0], v[1], v[2], v[3]});
        return true;
    }
    if (functionArgs(s, "steps", args)) {
        std::vector<std::string> parts = splitCommas(args);
        if (parts.empty() || parts.size() > 2) return false;
        char* end = nullptr;
        long n = std::strtol(parts[0].c_str(), &end, 10);
        if (end != parts[0].c_str() + parts[0].size() || n < 1 || n > 1000000) return false;
        TimingFunction f;
        f.kind = TimingFunction::Kind::Steps;
        f.steps = static_cast<int>(n);
        if (parts.size() == 2) {
            const std::string& p = parts[1];
            if (p == "jump-start" || p == "start") f.position = TimingFunction::StepPosition::JumpStart;
            else if (p == "jump-end" || p == "end") f.position = TimingFunction::StepPosition::JumpEnd;
            else if (p == "jump-none") f.position = TimingFunction::StepPosition::JumpNone;
            else if (p == "jump-both") f.position = TimingFunction::StepPosition::JumpBoth;
            else return false;
        }
        if (f.position == TimingFunction::StepPosition::JumpNone && f.steps < 2) return false;
        out = f;
        return true;
    }
    if (functionArgs(s, "linear", args)) {
        TimingFunction f;
        if (!parseLinearFunction(args, f)) return false;
        out = f;
        return true;
    }
    return false;
}

TimingFunction parseTimingFunction(const std::string& text) {
    TimingFunction f = TimingFunction::fromCubic(kEaseC);
    tryParseEasing(text, f);
    return f;
}

} // namespace bro::engine
