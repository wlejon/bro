#include "eye/eye_audit.h"
#include "eye/eye_color.h"
#include "eye/eye_measure.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/element_scroll.h"
#include "dom/text_node.h"
#include "engine/engine.h"
#include "engine/overflow.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::eye {
namespace {

std::string toLowerCopy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trimCopy(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string buildSelector(const dom::Element* el) {
    if (!el) return "";
    std::string s = toLowerCopy(el->tagName());
    if (!el->id().empty()) s += "#" + el->id();
    const std::string cls = trimCopy(el->getAttribute("class"));
    if (!cls.empty()) {
        std::istringstream iss(cls);
        std::string tok;
        int count = 0;
        while (iss >> tok && count < 2) {
            s += "." + tok;
            ++count;
        }
    }
    return s;
}

std::string styleProp(const dom::Element* el, const char* key) {
    if (!el) return {};
    const auto& s = el->computedStyle();
    auto it = s.find(key);
    return it != s.end() ? it->second : std::string{};
}

double parseCssPx(const std::string& val, double fallback) {
    if (val.empty()) return fallback;
    char* end = nullptr;
    const double d = std::strtod(val.c_str(), &end);
    return (end != val.c_str() && std::isfinite(d)) ? d : fallback;
}

double parseOpacity(const std::string& val) {
    if (val.empty()) return 1.0;
    char* end = nullptr;
    const double d = std::strtod(val.c_str(), &end);
    return (end != val.c_str() && std::isfinite(d)) ? std::clamp(d, 0.0, 1.0) : 1.0;
}

bool parseCssColor(std::string s, color::RGB& outRgb, double& outAlpha) {
    s = toLowerCopy(trimCopy(s));
    if (s.empty() || s == "transparent") {
        outRgb = {0, 0, 0};
        outAlpha = 0.0;
        return s == "transparent";
    }
    outAlpha = 1.0;

    static const std::unordered_map<std::string, color::RGB> kNamed = {
        {"black", {0, 0, 0}},       {"white", {255, 255, 255}}, {"red", {255, 0, 0}},
        {"green", {0, 128, 0}},     {"blue", {0, 0, 255}},      {"yellow", {255, 255, 0}},
        {"cyan", {0, 255, 255}},    {"magenta", {255, 0, 255}}, {"gray", {128, 128, 128}},
        {"grey", {128, 128, 128}},  {"silver", {192, 192, 192}},{"orange", {255, 165, 0}},
    };
    auto nit = kNamed.find(s);
    if (nit != kNamed.end()) {
        outRgb = nit->second;
        return true;
    }

    if (s[0] == '#') {
        auto hexByte = [](char h, char l) -> int {
            auto nib = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            const int hi = nib(h), lo = nib(l);
            return (hi < 0 || lo < 0) ? -1 : ((hi << 4) | lo);
        };
        if (s.size() == 4 || s.size() == 5) {
            const int r = hexByte(s[1], s[1]);
            const int g = hexByte(s[2], s[2]);
            const int b = hexByte(s[3], s[3]);
            const int a = s.size() == 5 ? hexByte(s[4], s[4]) : 255;
            if (r >= 0 && g >= 0 && b >= 0 && a >= 0) {
                outRgb = {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
                outAlpha = a / 255.0;
                return true;
            }
        } else if (s.size() == 7 || s.size() == 9) {
            const int r = hexByte(s[1], s[2]);
            const int g = hexByte(s[3], s[4]);
            const int b = hexByte(s[5], s[6]);
            const int a = s.size() == 9 ? hexByte(s[7], s[8]) : 255;
            if (r >= 0 && g >= 0 && b >= 0 && a >= 0) {
                outRgb = {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
                outAlpha = a / 255.0;
                return true;
            }
        }
        return false;
    }

    if (s.rfind("oklch", 0) == 0 || s.rfind("oklab", 0) == 0) {
        const bool isLch = (s.rfind("oklch", 0) == 0);
        const size_t open = s.find('(');
        const size_t close = s.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close <= open) return false;
        std::string inner = s.substr(open + 1, close - open - 1);
        for (char& ch : inner) {
            if (ch == ',' || ch == '/') ch = ' ';
        }
        std::istringstream iss(inner);
        std::string toks[4];
        int cnt = 0;
        while (cnt < 4 && (iss >> toks[cnt])) ++cnt;
        if (cnt < 3) return false;

        auto parsePctOr = [](const std::string& t, double pctScale) -> double {
            char* end = nullptr;
            double v = std::strtod(t.c_str(), &end);
            if (!std::isfinite(v)) return 0.0;
            if (end && *end == '%') v *= pctScale;
            return v;
        };
        auto parseHueDeg = [](const std::string& t) -> double {
            char* end = nullptr;
            double v = std::strtod(t.c_str(), &end);
            if (!std::isfinite(v)) return 0.0;
            std::string unit = end ? end : "";
            if (unit == "rad") v *= (180.0 / 3.14159265358979323846);
            else if (unit == "turn") v *= 360.0;
            else if (unit == "grad") v *= 0.9;
            return v;
        };

        const double L = parsePctOr(toks[0], 0.01);
        color::Oklab lab{};
        if (isLch) {
            const double C = std::max(0.0, parsePctOr(toks[1], 0.004));
            const double hRad = parseHueDeg(toks[2]) * (3.14159265358979323846 / 180.0);
            lab = {L, C * std::cos(hRad), C * std::sin(hRad)};
        } else {
            lab = {L, parsePctOr(toks[1], 0.004), parsePctOr(toks[2], 0.004)};
        }
        outRgb = color::oklabToSrgb(lab);
        outAlpha = (cnt == 4) ? std::clamp(parsePctOr(toks[3], 0.01), 0.0, 1.0) : 1.0;
        return true;
    }

    if (s.rfind("rgb", 0) == 0) {
        const size_t open = s.find('(');
        const size_t close = s.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close <= open) return false;
        std::string inner = s.substr(open + 1, close - open - 1);
        for (char& ch : inner) {
            if (ch == ',' || ch == '/') ch = ' ';
        }
        std::istringstream iss(inner);
        double vals[4] = {0, 0, 0, 1};
        int cnt = 0;
        std::string tok;
        while (iss >> tok && cnt < 4) {
            const bool pct = !tok.empty() && tok.back() == '%';
            double d = std::strtod(tok.c_str(), nullptr);
            if (pct && cnt < 3) d = d * 2.55;
            else if (pct && cnt == 3) d = d * 0.01;
            vals[cnt++] = d;
        }
        if (cnt >= 3) {
            outRgb = {
                static_cast<uint8_t>(std::clamp(std::lround(vals[0]), 0L, 255L)),
                static_cast<uint8_t>(std::clamp(std::lround(vals[1]), 0L, 255L)),
                static_cast<uint8_t>(std::clamp(std::lround(vals[2]), 0L, 255L)),
            };
            outAlpha = (cnt == 4) ? std::clamp(vals[3], 0.0, 1.0) : 1.0;
            return true;
        }
    }
    return false;
}

bool isIgnoredTag(const std::string& tag) {
    return tag == "SCRIPT" || tag == "STYLE" || tag == "HEAD" || tag == "META" ||
           tag == "LINK" || tag == "TITLE" || tag == "NOSCRIPT";
}

bool isDisabledControl(const dom::Element* el) {
    for (const dom::Element* cur = el; cur; cur = cur->parentElement()) {
        if (cur->hasAttribute("disabled")) return true;
        if (toLowerCopy(trimCopy(cur->getAttribute("aria-disabled"))) == "true") return true;
    }
    return false;
}

bool isInteractiveElement(const dom::Element* el) {
    if (!el) return false;
    const std::string& tag = el->tagName();
    if (tag == "BUTTON" || tag == "A" || tag == "INPUT" || tag == "SELECT" ||
        tag == "TEXTAREA" || tag == "SUMMARY") {
        return true;
    }
    const std::string role = toLowerCopy(el->getAttribute("role"));
    if (role == "button" || role == "link" || role == "checkbox" || role == "switch" || role == "tab") {
        return true;
    }
    return el->hasJsListener("click");
}

std::string directTextSnippet(const dom::Element* el) {
    std::string combined;
    for (auto* child : el->childNodes()) {
        if (child->nodeType() == dom::NodeType::Text) {
            auto* tn = static_cast<const dom::TextNode*>(child);
            const std::string t = trimCopy(tn->data());
            if (!t.empty()) {
                if (!combined.empty()) combined += ' ';
                combined += t;
            }
        }
    }
    if (combined.size() > 64) combined = combined.substr(0, 61) + "...";
    return combined;
}

struct WalkItem {
    dom::Element* el = nullptr;
    EyeRect viewportRect;
    EyeRect contentRect;
    EyeRect clipRect;
    bool clipped = false;
    bool insideScroller = false;
    double effectiveOpacity = 1.0;
    std::string directText;
    bool interactive = false;
};

double intersectArea(const EyeRect& a, const EyeRect& b) {
    const double x0 = std::max(a.x, b.x);
    const double y0 = std::max(a.y, b.y);
    const double x1 = std::min(a.x + a.w, b.x + b.w);
    const double y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return 0.0;
    return (x1 - x0) * (y1 - y0);
}

EyeRect intersectRect(const EyeRect& a, const EyeRect& b) {
    const double x0 = std::max(a.x, b.x);
    const double y0 = std::max(a.y, b.y);
    const double x1 = std::min(a.x + a.w, b.x + b.w);
    const double y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return {0, 0, 0, 0};
    return {x0, y0, x1 - x0, y1 - y0};
}

bool isAncestor(const dom::Element* a, const dom::Element* b) {
    for (const dom::Element* cur = b ? b->parentElement() : nullptr; cur; cur = cur->parentElement()) {
        if (cur == a) return true;
    }
    return false;
}

bool hasFloatingLayerOutside(const dom::Element* el, const dom::Element* other) {
    for (const dom::Element* cur = el; cur; cur = cur->parentElement()) {
        if (cur == other || isAncestor(cur, other)) break;
        const std::string pos = styleProp(cur, "position");
        if (pos == "fixed") return true;
        if (pos == "absolute") {
            const std::string zi = trimCopy(styleProp(cur, "z-index"));
            if (!zi.empty() && zi != "auto" && std::strtol(zi.c_str(), nullptr, 10) > 0) {
                return true;
            }
        }
    }
    return false;
}

bool isFloatingHudOverCanvas(const WalkItem* a, const WalkItem* b) {
    auto check = [](const WalkItem* canvas, const WalkItem* hud) -> bool {
        return canvas->el->tagName() == "CANVAS" && !canvas->interactive &&
               hasFloatingLayerOutside(hud->el, canvas->el);
    };
    return check(a, b) || check(b, a);
}

// Estimate background RGB around text from rendered frame pixels inside `devBox`.
// Returns both modal background color and worst-case contrast pair against `fgCss` (if valid).
void sampleRenderedContrast(const uint8_t* pixels, int frameW, int frameH,
                            int px0, int py0, int pw, int ph,
                            bool hasCssFg, color::RGB cssFg, double fgAlpha,
                            color::RGB& outFg, color::RGB& outBg,
                            double& outWcag, double& outApca) {
    px0 = std::clamp(px0, 0, std::max(0, frameW - 1));
    py0 = std::clamp(py0, 0, std::max(0, frameH - 1));
    const int px1 = std::clamp(px0 + pw, px0 + 1, frameW);
    const int py1 = std::clamp(py0 + ph, py0 + 1, frameH);
    const int rw = px1 - px0;
    const int rh = py1 - py0;

    const int stepX = std::max(1, rw / 96);
    const int stepY = std::max(1, rh / 24);

    struct QuantBin {
        int count = 0;
        int64_t rSum = 0, gSum = 0, bSum = 0;
    };
    std::unordered_map<uint32_t, QuantBin> bins;
    struct Sample { color::RGB rgb; color::Oklab lab; };
    std::vector<Sample> samples;
    samples.reserve(256);

    for (int y = py0; y < py1; y += stepY) {
        for (int x = px0; x < px1; x += stepX) {
            const uint8_t* p = pixels + (static_cast<size_t>(y) * frameW + x) * 4;
            const color::RGB rgb{p[0], p[1], p[2]};
            const auto lab = color::srgbToOklab(rgb.r, rgb.g, rgb.b);
            samples.push_back({rgb, lab});

            const uint32_t key = ((rgb.r >> 4) << 8) | ((rgb.g >> 4) << 4) | (rgb.b >> 4);
            auto& b = bins[key];
            ++b.count;
            b.rSum += rgb.r;
            b.gSum += rgb.g;
            b.bSum += rgb.b;
        }
    }

    if (samples.empty()) {
        outFg = cssFg;
        outBg = {0, 0, 0};
        outWcag = 1.0;
        outApca = 0.0;
        return;
    }

    // Mode background color from majority pixels in box.
    const QuantBin* bestBin = nullptr;
    for (const auto& kv : bins) {
        if (!bestBin || kv.second.count > bestBin->count) bestBin = &kv.second;
    }
    color::RGB modeBg{
        static_cast<uint8_t>(bestBin->rSum / bestBin->count),
        static_cast<uint8_t>(bestBin->gSum / bestBin->count),
        static_cast<uint8_t>(bestBin->bSum / bestBin->count),
    };

    auto blendOver = [](color::RGB fg, double a, color::RGB bg) -> color::RGB {
        if (a >= 0.999) return fg;
        return {
            static_cast<uint8_t>(std::clamp(std::lround(fg.r * a + bg.r * (1.0 - a)), 0L, 255L)),
            static_cast<uint8_t>(std::clamp(std::lround(fg.g * a + bg.g * (1.0 - a)), 0L, 255L)),
            static_cast<uint8_t>(std::clamp(std::lround(fg.b * a + bg.b * (1.0 - a)), 0L, 255L)),
        };
    };

    if (hasCssFg) {
        // Sample the dominant mode background color in up to 3 horizontal strips across
        // the box so solid UI boxes (even with padding: 0 where glyphs touch edges)
        // never mistake antialiased glyph edges for background, while gradients/canvas
        // halves behind text are measured at their weakest contrast strip.
        double minWcag = 99.0;
        color::RGB worstBg = modeBg;
        color::RGB worstFg = blendOver(cssFg, fgAlpha, modeBg);

        const int numStrips = rw >= 12 ? 3 : 1;
        for (int s = 0; s < numStrips; ++s) {
            const int sx0 = px0 + (s * rw) / numStrips;
            const int sx1 = px0 + ((s + 1) * rw) / numStrips;
            std::unordered_map<uint32_t, QuantBin> sbins;
            for (int y = py0; y < py1; y += stepY) {
                for (int x = sx0; x < sx1; x += stepX) {
                    const uint8_t* p = pixels + (static_cast<size_t>(y) * frameW + x) * 4;
                    const uint32_t key = ((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4);
                    auto& b = sbins[key];
                    ++b.count;
                    b.rSum += p[0];
                    b.gSum += p[1];
                    b.bSum += p[2];
                }
            }
            const QuantBin* stripBest = nullptr;
            for (const auto& kv : sbins) {
                if (!stripBest || kv.second.count > stripBest->count) stripBest = &kv.second;
            }
            const color::RGB stripBg = stripBest
                ? color::RGB{
                      static_cast<uint8_t>(stripBest->rSum / stripBest->count),
                      static_cast<uint8_t>(stripBest->gSum / stripBest->count),
                      static_cast<uint8_t>(stripBest->bSum / stripBest->count),
                  }
                : modeBg;
            const color::RGB ef = blendOver(cssFg, fgAlpha, stripBg);
            const double w = color::wcagContrastRatio(ef, stripBg);
            if (w < minWcag) {
                minWcag = w;
                worstBg = stripBg;
                worstFg = ef;
            }
        }
        outFg = worstFg;
        outBg = worstBg;
        outWcag = minWcag;
        outApca = color::apcaContrast(worstFg, worstBg);
        return;
    }

    // Fallback: estimate foreground from farthest Oklab sample from modeBg.
    const auto modeLab = color::srgbToOklab(modeBg.r, modeBg.g, modeBg.b);
    color::RGB farFg = modeBg;
    double maxDe = -1.0;
    for (const auto& s : samples) {
        const double de = color::oklabDeltaE(s.lab, modeLab);
        if (de > maxDe) { maxDe = de; farFg = s.rgb; }
    }
    outFg = farFg;
    outBg = modeBg;
    outWcag = color::wcagContrastRatio(farFg, modeBg);
    outApca = color::apcaContrast(farFg, modeBg);
}

}  // namespace

EyeAuditResult auditDocument(engine::Engine& engine, const EyeAuditOptions& opts) {
    EyeAuditResult result;
    engine.flush();

    const int fw = engine.framePixelWidth();
    const int fh = engine.framePixelHeight();
    const int vw = std::max(1, engine.viewportWidth());
    const int vh = std::max(1, engine.viewportHeight());
    const float scrollY = engine.viewportScrollY();
    const int cLeft = engine.contentLeft();
    const int cTop = engine.contentTop();
    const int cW = std::max(1, engine.contentWidth());
    const int cH = std::max(1, engine.contentHeight());

    std::vector<uint8_t> pixels = engine.capturePixels();
    if (pixels.empty() || fw <= 0 || fh <= 0) {
        result.ok = false;
        result.score = 0.0;
        EyeIssue iss;
        iss.kind = "no-frame";
        iss.severity = "error";
        iss.message = "Unable to capture rendered frame pixels";
        result.issues.push_back(std::move(iss));
        result.errorCount = 1;
        return result;
    }

    result.frame = measureImage({pixels.data(), fw, fh, fw * 4});

    dom::Document* doc = engine.document();
    dom::Element* root = nullptr;
    if (doc) {
        root = !opts.selector.empty() ? engine.querySelector(opts.selector) : doc->documentElement();
    }

    std::vector<WalkItem> items;
    if (root) {
        const EyeRect viewportClip{0.0, 0.0, static_cast<double>(cW), static_cast<double>(cH)};
        auto walk = [&](auto& self, dom::Element* el, const EyeRect& clip, bool insideScroller, double parentOpacity) -> void {
            if (!el) return;
            const std::string& tag = el->tagName();
            if (isIgnoredTag(tag)) return;

            const std::string display = styleProp(el, "display");
            if (display == "none") return;

            const std::string visibility = styleProp(el, "visibility");
            const double opacity = parentOpacity * parseOpacity(styleProp(el, "opacity"));
            if (opacity <= 0.02) return;

            const auto& box = el->layoutBox();
            const dom::AbsoluteRect absB = dom::absoluteBorderBox(el);
            const dom::AbsoluteRect absC = dom::absoluteContentBox(el);

            const EyeRect vpRect{
                static_cast<double>(absB.x),
                static_cast<double>(absB.y - scrollY),
                static_cast<double>(absB.width),
                static_cast<double>(absB.height),
            };
            const EyeRect ctRect{
                static_cast<double>(absC.x),
                static_cast<double>(absC.y - scrollY),
                static_cast<double>(absC.width),
                static_cast<double>(absC.height),
            };

            const bool hiddenSelf = (visibility == "hidden" || visibility == "collapse");
            if (!hiddenSelf && tag != "HTML" && tag != "BODY") {
                WalkItem item;
                item.el = el;
                item.viewportRect = vpRect;
                item.contentRect = ctRect;
                item.clipRect = clip;
                item.insideScroller = insideScroller;
                item.effectiveOpacity = opacity;
                item.directText = directTextSnippet(el);
                item.interactive = isInteractiveElement(el);
                item.clipped = (intersectArea(vpRect, clip) <= 0.0) && (vpRect.w > 0 && vpRect.h > 0);
                items.push_back(std::move(item));
            }

            const std::string ovX = engine::getOverflowX(el->computedStyle());
            const std::string ovY = engine::getOverflowY(el->computedStyle());
            const bool clipsHere = (tag != "HTML" && tag != "BODY") &&
                                   (engine::overflowClips(ovX) || engine::overflowClips(ovY));
            const bool scrollsHere = (tag != "HTML" && tag != "BODY") &&
                                     (engine::overflowScrollable(ovX) || engine::overflowScrollable(ovY));

            EyeRect nextClip = clip;
            if (clipsHere && vpRect.w > 0 && vpRect.h > 0) {
                nextClip = intersectRect(clip, vpRect);
            }

            for (auto* child : el->composedChildNodes()) {
                if (child->nodeType() == dom::NodeType::Element) {
                    self(self, static_cast<dom::Element*>(child), nextClip,
                         insideScroller || scrollsHere, opacity);
                }
            }
        };
        walk(walk, root, viewportClip, false, 1.0);
    }

    const double sx = static_cast<double>(fw) / static_cast<double>(vw);
    const double sy = static_cast<double>(fh) / static_cast<double>(vh);

    std::vector<EyeIssue> issues;

    for (const auto& item : items) {
        ++result.checkedElements;
        dom::Element* el = item.el;
        const auto& box = el->layoutBox();
        const std::string sel = buildSelector(el);
        const bool disabled = isDisabledControl(el);

        // 1. Rendered text contrast (WCAG 2.1 exempts disabled/inactive UI controls)
        if (!disabled && !item.directText.empty() && !item.clipped && item.contentRect.w >= 2.0 && item.contentRect.h >= 2.0) {
            const EyeRect visibleBox = intersectRect(item.viewportRect, item.clipRect);
            if (visibleBox.w >= 2.0 && visibleBox.h >= 2.0) {
                ++result.checkedTextNodes;
                const int px0 = static_cast<int>(std::floor((visibleBox.x + cLeft) * sx));
                const int py0 = static_cast<int>(std::floor((visibleBox.y + cTop) * sy));
                const int pw = std::max(1, static_cast<int>(std::ceil(visibleBox.w * sx)));
                const int ph = std::max(1, static_cast<int>(std::ceil(visibleBox.h * sy)));

                color::RGB cssFg{};
                double fgAlpha = 1.0;
                const bool hasFg = parseCssColor(styleProp(el, "color"), cssFg, fgAlpha);
                fgAlpha *= item.effectiveOpacity;

                color::RGB fgOut{}, bgOut{};
                double wcag = 21.0, apca = 0.0;
                sampleRenderedContrast(pixels.data(), fw, fh, px0, py0, pw, ph,
                                       hasFg, cssFg, fgAlpha, fgOut, bgOut, wcag, apca);

                const double fontSize = parseCssPx(styleProp(el, "font-size"), 16.0);
                const std::string fwStr = styleProp(el, "font-weight");
                const int fwNum = (fwStr == "bold" || fwStr == "bolder") ? 700 : static_cast<int>(parseCssPx(fwStr, 400.0));
                const bool largeText = (fontSize >= 24.0) || (fontSize >= 18.66 && fwNum >= 700);
                const double reqWcag = largeText ? std::min(opts.minContrast, 3.0) : opts.minContrast;

                const bool failWcag = wcag < reqWcag;
                const bool failApca = (opts.minApca > 0.0) && (std::abs(apca) < opts.minApca);
                if (failWcag || failApca) {
                    EyeIssue iss;
                    iss.kind = "text-contrast";
                    iss.severity = (wcag < 3.0) ? "error" : "warning";
                    iss.selector = sel;
                    iss.rect = visibleBox;
                    iss.hasContrast = true;
                    iss.wcag = wcag;
                    iss.apca = apca;
                    iss.fgHex = color::rgbToHex(fgOut);
                    iss.bgHex = color::rgbToHex(bgOut);
                    iss.fontSize = fontSize;
                    iss.text = item.directText;
                    char buf[220];
                    std::snprintf(buf, sizeof(buf),
                                  "Low rendered text contrast on %s: WCAG %.2f:1 (need %.1f:1), APCA %.1f (%s on %s)",
                                  sel.c_str(), wcag, reqWcag, apca, iss.fgHex.c_str(), iss.bgHex.c_str());
                    iss.message = buf;
                    issues.push_back(std::move(iss));
                }
            }
        }

        // 2. Text truncation & non-scrollable overflow clipping
        if (opts.checkClip && !item.clipped) {
            if (box.textTruncated) {
                EyeIssue iss;
                iss.kind = "text-truncated";
                iss.severity = "warning";
                iss.selector = sel;
                iss.rect = item.viewportRect;
                iss.text = item.directText;
                iss.message = "Text truncated inside " + sel;
                issues.push_back(std::move(iss));
            }
            const std::string ovX = engine::getOverflowX(el->computedStyle());
            const std::string ovY = engine::getOverflowY(el->computedStyle());
            const bool ellipsisX = box.textTruncated && (styleProp(el, "text-overflow") == "ellipsis");
            const bool clipsNonScrollX = engine::overflowClips(ovX) && !engine::overflowScrollable(ovX) && !ellipsisX;
            const bool clipsNonScrollY = engine::overflowClips(ovY) && !engine::overflowScrollable(ovY);
            const double overX = clipsNonScrollX ? std::max(0.0, static_cast<double>(box.naturalWidth - box.contentRect.width)) : 0.0;
            const double overY = clipsNonScrollY ? std::max(0.0, static_cast<double>(box.naturalHeight - box.contentRect.height)) : 0.0;
            if (overX > 2.0 || overY > 2.0) {
                EyeIssue iss;
                iss.kind = "overflow-clip";
                iss.severity = (overX > 8.0 || overY > 8.0) ? "error" : "warning";
                iss.selector = sel;
                iss.rect = item.viewportRect;
                iss.hasOverflow = true;
                iss.overflowX = overX;
                iss.overflowY = overY;
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              "Content clipped by %s (overflow-x %.1fpx, overflow-y %.1fpx)",
                              sel.c_str(), overX, overY);
                iss.message = buf;
                issues.push_back(std::move(iss));
            }
        }

        // 3. Offscreen content
        if (opts.checkOffscreen && !item.insideScroller &&
            (!item.directText.empty() || item.interactive || el->tagName() == "IMG" || el->tagName() == "CANVAS") &&
            item.viewportRect.w > 1.0 && item.viewportRect.h > 1.0) {
            const EyeRect& r = item.viewportRect;
            if (r.x + 1.0 < 0.0 || r.y + 1.0 < 0.0 ||
                r.x + r.w > cW + 1.0 || r.y + r.h > cH + 1.0) {
                EyeIssue iss;
                iss.kind = "offscreen";
                iss.severity = (r.x + r.w <= 0.0 || r.y + r.h <= 0.0 || r.x >= cW || r.y >= cH) ? "error" : "warning";
                iss.selector = sel;
                iss.rect = r;
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              "Element %s extends outside viewport (%.0f,%.0f %.0fx%.0f vs %dx%d)",
                              sel.c_str(), r.x, r.y, r.w, r.h, cW, cH);
                iss.message = buf;
                issues.push_back(std::move(iss));
            }
        }

        // 4. Small hit targets
        const std::string curStyle = styleProp(el, "cursor");
        const bool isResizeHandle = (curStyle == "col-resize" || curStyle == "row-resize");
        if (!disabled && !isResizeHandle && opts.checkHitTargets && item.interactive && !item.clipped &&
            item.viewportRect.w > 0.0 && item.viewportRect.h > 0.0) {
            if (item.viewportRect.w + 0.5 < opts.minHitTarget || item.viewportRect.h + 0.5 < opts.minHitTarget) {
                EyeIssue iss;
                iss.kind = "small-hit-target";
                iss.severity = "warning";
                iss.selector = sel;
                iss.rect = item.viewportRect;
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "Interactive %s is %.1fx%.1f CSS px (below %.0fpx target)",
                              sel.c_str(), item.viewportRect.w, item.viewportRect.h, opts.minHitTarget);
                iss.message = buf;
                issues.push_back(std::move(iss));
            }
        }
    }

    // 5. Pairwise sibling / non-ancestor overlap detection
    if (opts.checkOverlap) {
        std::vector<const WalkItem*> candidates;
        for (const auto& it : items) {
            if (it.clipped || it.viewportRect.w < 2.0 || it.viewportRect.h < 2.0) continue;
            if (styleProp(it.el, "pointer-events") == "none") continue;
            const bool leafContent = !it.directText.empty() || it.interactive ||
                                     it.el->tagName() == "IMG" || it.el->tagName() == "CANVAS";
            if (leafContent) candidates.push_back(&it);
        }
        for (size_t i = 0; i < candidates.size(); ++i) {
            for (size_t j = i + 1; j < candidates.size(); ++j) {
                const auto* a = candidates[i];
                const auto* b = candidates[j];
                if (isAncestor(a->el, b->el) || isAncestor(b->el, a->el)) continue;
                if (isFloatingHudOverCanvas(a, b)) continue;
                const EyeRect va = intersectRect(a->viewportRect, a->clipRect);
                const EyeRect vb = intersectRect(b->viewportRect, b->clipRect);
                const double area = intersectArea(va, vb);
                if (area <= opts.minOverlapPx) continue;
                const double minBoxArea = std::max(1.0, std::min(va.w * va.h, vb.w * vb.h));
                const double ratio = area / minBoxArea;
                if (ratio < 0.08) continue;

                const std::string sa = buildSelector(a->el);
                const std::string sb = buildSelector(b->el);
                EyeIssue iss;
                iss.kind = "overlap";
                iss.severity = ((!a->directText.empty() && !b->directText.empty()) ||
                                (a->interactive && b->interactive))
                                   ? "error"
                                   : "warning";
                iss.selector = sa;
                iss.otherSelector = sb;
                iss.rect = intersectRect(va, vb);
                iss.hasOverlap = true;
                iss.overlapRatio = ratio;
                char buf[220];
                std::snprintf(buf, sizeof(buf),
                              "Elements %s and %s overlap by %.0f%% (%.0f px^2)",
                              sa.c_str(), sb.c_str(), ratio * 100.0, area);
                iss.message = buf;
                issues.push_back(std::move(iss));
            }
        }
    }

    // 6. Frame-level optical defects
    if (opts.checkFrame) {
        if (result.frame.lumaStd < 0.004 && result.frame.chromaStd < 0.004) {
            EyeIssue iss;
            iss.kind = "blank-frame";
            iss.severity = "error";
            iss.message = "Frame appears blank / uniform (near-zero luminance & chroma variance)";
            iss.rect = {0, 0, static_cast<double>(cW), static_cast<double>(cH)};
            issues.push_back(std::move(iss));
        }
        if (result.frame.crushedBlackRatio > 0.50 || result.frame.blownWhiteRatio > 0.50) {
            EyeIssue iss;
            iss.kind = "exposure-clip";
            iss.severity = "warning";
            iss.value = std::max(result.frame.crushedBlackRatio, result.frame.blownWhiteRatio);
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "Heavy exposure clipping (crushed %.1f%%, blown %.1f%%)",
                          result.frame.crushedBlackRatio * 100.0, result.frame.blownWhiteRatio * 100.0);
            iss.message = buf;
            iss.rect = {0, 0, static_cast<double>(cW), static_cast<double>(cH)};
            issues.push_back(std::move(iss));
        }
        if (result.frame.bandingScore > 0.45) {
            EyeIssue iss;
            iss.kind = "banding";
            iss.severity = "warning";
            iss.value = result.frame.bandingScore;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Visible gradient banding detected (score %.2f)", result.frame.bandingScore);
            iss.message = buf;
            iss.rect = {0, 0, static_cast<double>(cW), static_cast<double>(cH)};
            issues.push_back(std::move(iss));
        }
        if (result.frame.tiling.score > 0.65) {
            EyeIssue iss;
            iss.kind = "tiling-artifact";
            iss.severity = "warning";
            iss.value = result.frame.tiling.score;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Strong periodic repetition / tiling (score %.2f)", result.frame.tiling.score);
            iss.message = buf;
            iss.rect = {0, 0, static_cast<double>(cW), static_cast<double>(cH)};
            issues.push_back(std::move(iss));
        }
    }

    std::stable_sort(issues.begin(), issues.end(), [](const EyeIssue& a, const EyeIssue& b) {
        if (a.severity != b.severity) return a.severity == "error";
        return false;
    });

    for (const auto& iss : issues) {
        if (iss.severity == "error") ++result.errorCount;
        else ++result.warningCount;
    }

    if (opts.maxIssues > 0 && issues.size() > static_cast<size_t>(opts.maxIssues)) {
        issues.resize(static_cast<size_t>(opts.maxIssues));
    }
    result.issues = std::move(issues);
    result.ok = (result.errorCount == 0);
    result.score = std::clamp(100.0 - 15.0 * result.errorCount - 5.0 * result.warningCount, 0.0, 100.0);
    return result;
}

}  // namespace bro::eye
