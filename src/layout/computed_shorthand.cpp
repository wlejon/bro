// getComputedStyle's shorthands, serialized from their longhands' resolved
// values the way Chromium does it: `margin` "1px 2px", `border` "1px solid
// rgb(255, 0, 0)" (empty when the sides differ), `font` "italic 700 14px /
// 21px Arial, sans-serif", `background` "rgb(0, 0, 255) none repeat scroll 0%
// 0% / auto padding-box border-box", `flex` "1 1 0%", and the rest below.
// htmlayout keeps the longhands (it expands every shorthand at parse), so a
// shorthand's own entry, where one is kept, is the author's text and not what
// the web reports.

#include "layout/computed_style.h"

#include <array>
#include <string>
#include <vector>

namespace bro::layout {

namespace {

// 1-4 values, collapsed as CSS writes a box: t r b l.
std::string box4(const std::array<std::string, 4>& v) {
    if (v[1] == v[3]) {
        if (v[0] == v[2]) return v[0] == v[1] ? v[0] : v[0] + " " + v[1];
        return v[0] + " " + v[1] + " " + v[2];
    }
    return v[0] + " " + v[1] + " " + v[2] + " " + v[3];
}

std::vector<std::string> splitTop(const std::string& s, char sep) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char c : s) {
        if (c == '(') depth++;
        if (c == ')') depth--;
        if (c == sep && depth == 0) {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    out.push_back(cur);
    for (auto& p : out) {
        size_t a = p.find_first_not_of(' ');
        size_t b = p.find_last_not_of(' ');
        p = a == std::string::npos ? std::string() : p.substr(a, b - a + 1);
    }
    return out;
}

}  // namespace

bool computedShorthand(dom::Element* el, const std::string& prop, SkiaTextMetrics* metrics, std::string& out) {
    auto get = [&](const std::string& p) { return computedProperty(el, p, metrics); };
    auto sides = [&](const std::string& pre, const std::string& post) {
        return std::array<std::string, 4>{get(pre + "top" + post), get(pre + "right" + post),
                                          get(pre + "bottom" + post), get(pre + "left" + post)};
    };

    if (prop == "margin" || prop == "padding" || prop == "scroll-margin" || prop == "scroll-padding") {
        out = box4(sides(prop + "-", ""));
        return true;
    }
    if (prop == "inset") {
        out = box4({get("top"), get("right"), get("bottom"), get("left")});
        return true;
    }
    if (prop == "border-width" || prop == "border-style" || prop == "border-color") {
        out = box4(sides("border-", prop.substr(6)));
        return true;
    }
    if (prop == "border-top" || prop == "border-right" || prop == "border-bottom" || prop == "border-left") {
        out = get(prop + "-width") + " " + get(prop + "-style") + " " + get(prop + "-color");
        return true;
    }
    if (prop == "border") {
        std::string sidesText[4];
        const char* names[4] = {"border-top", "border-right", "border-bottom", "border-left"};
        for (int i = 0; i < 4; ++i)
            sidesText[i] = get(std::string(names[i]) + "-width") + " " + get(std::string(names[i]) + "-style") + " " +
                           get(std::string(names[i]) + "-color");
        out = (sidesText[0] == sidesText[1] && sidesText[0] == sidesText[2] && sidesText[0] == sidesText[3])
                  ? sidesText[0] : std::string();
        return true;
    }
    if (prop == "border-radius") {
        const char* corners[4] = {"border-top-left-radius", "border-top-right-radius",
                                  "border-bottom-right-radius", "border-bottom-left-radius"};
        std::array<std::string, 4> h, v;
        bool elliptical = false;
        for (int i = 0; i < 4; ++i) {
            auto parts = splitTop(get(corners[i]), ' ');
            h[i] = parts[0].empty() ? "0px" : parts[0];
            if (h[i] == "0") h[i] = "0px";
            v[i] = parts.size() > 1 ? parts[1] : h[i];
            if (v[i] != h[i]) elliptical = true;
        }
        out = box4(h);
        if (elliptical) out += " / " + box4(v);
        return true;
    }
    if (prop == "outline") {
        out = get("outline-color") + " " + get("outline-style") + " " + get("outline-width");
        return true;
    }
    if (prop == "list-style") {
        out = get("list-style-position") + " " + get("list-style-image") + " " + get("list-style-type");
        return true;
    }
    if (prop == "columns") {
        const std::string w = get("column-width"), c = get("column-count");
        if (w == "auto") out = c;
        else if (c == "auto") out = w;
        else out = w + " " + c;
        return true;
    }
    if (prop == "place-items" || prop == "place-content" || prop == "place-self") {
        const std::string suffix = prop.substr(6);
        const std::string a = get("align-" + suffix), j = get("justify-" + suffix);
        out = a == j ? a : a + " " + j;
        return true;
    }
    if (prop == "flex") {
        out = get("flex-grow") + " " + get("flex-shrink") + " " + get("flex-basis");
        return true;
    }
    if (prop == "flex-flow") {
        out = get("flex-direction") + " " + get("flex-wrap");
        return true;
    }
    if (prop == "overflow") {
        const std::string x = get("overflow-x"), y = get("overflow-y");
        out = x == y ? x : x + " " + y;
        return true;
    }
    if (prop == "text-emphasis") {
        out = get("text-emphasis-style") + " " + get("text-emphasis-color");
        return true;
    }
    if (prop == "font") {
        std::string s;
        auto add = [&](const std::string& v, const char* skip) {
            if (!v.empty() && v != skip) s += v + " ";
        };
        add(get("font-style"), "normal");
        add(get("font-variant-caps"), "normal");
        add(get("font-weight"), "400");
        add(get("font-stretch"), "100%");
        s += get("font-size");
        const std::string lh = get("line-height");
        if (!lh.empty() && lh != "normal") s += " / " + lh;
        s += " " + get("font-family");
        out = s;
        return true;
    }
    if (prop == "background") {
        // One layer per background-image entry; the colour goes on the last.
        auto layers = [&](const char* p) { return splitTop(get(p), ','); };
        auto images = layers("background-image");
        auto repeat = layers("background-repeat");
        auto attach = layers("background-attachment");
        auto pos = layers("background-position");
        auto size = layers("background-size");
        auto origin = layers("background-origin");
        auto clip = layers("background-clip");
        auto at = [](const std::vector<std::string>& v, size_t i, const char* dflt) {
            const std::string& x = v.empty() ? std::string() : v[i % v.size()];
            return x.empty() ? std::string(dflt) : x;
        };
        std::string s;
        for (size_t i = 0; i < images.size(); ++i) {
            if (i) s += ", ";
            if (i + 1 == images.size()) s += get("background-color") + " ";
            s += at(images, i, "none") + " " + at(repeat, i, "repeat") + " " + at(attach, i, "scroll") + " " +
                 at(pos, i, "0% 0%") + " / " + at(size, i, "auto") + " " + at(origin, i, "padding-box") + " " +
                 at(clip, i, "border-box");
        }
        out = s;
        return true;
    }
    if (prop == "transition") {
        auto prop_ = splitTop(get("transition-property"), ',');
        auto dur = splitTop(get("transition-duration"), ',');
        auto tf = splitTop(get("transition-timing-function"), ',');
        auto delay = splitTop(get("transition-delay"), ',');
        auto at = [](const std::vector<std::string>& v, size_t i, const char* dflt) {
            const std::string& x = v.empty() ? std::string() : v[i % v.size()];
            return x.empty() ? std::string(dflt) : x;
        };
        std::string s;
        for (size_t i = 0; i < prop_.size(); ++i) {
            if (i) s += ", ";
            const std::string p = at(prop_, i, "all"), d = at(dur, i, "0s"), t = at(tf, i, "ease"),
                              dl = at(delay, i, "0s");
            std::string item = p;
            if (d != "0s" || dl != "0s") item += " " + d;
            if (t != "ease") item += " " + t;
            if (dl != "0s") item += " " + dl;
            s += item;
        }
        out = s;
        return true;
    }
    return false;
}

}  // namespace bro::layout
