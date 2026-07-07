#include "layout/svg_text.h"
#include "layout/svg_common.h"
#include "dom/element.h"
#include "dom/text_node.h"
#include "render/renderer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace bro::layout {

using svgcommon::lower;
using svgcommon::attrOf;
using svgcommon::attrFloat;

namespace {

// ---- computed-style accessors ----------------------------------------------

std::string styleOr(const dom::Element* el, const char* prop, const char* fallback) {
    const auto& cs = el->computedStyle();
    auto it = cs.find(prop);
    if (it != cs.end() && !it->second.empty()) return it->second;
    return fallback;
}

float styleFloat(const dom::Element* el, const char* prop, float fallback) {
    const auto& cs = el->computedStyle();
    auto it = cs.find(prop);
    if (it == cs.end() || it->second.empty()) return fallback;
    char* end = nullptr;
    float f = std::strtof(it->second.c_str(), &end);
    return end == it->second.c_str() ? fallback : f;
}

int parseWeight(const std::string& w) {
    if (w == "bold") return 700;
    if (w == "bolder") return 700;
    if (w == "lighter") return 300;
    if (w == "normal" || w.empty()) return 400;
    char* end = nullptr;
    long v = std::strtol(w.c_str(), &end, 10);
    if (end != w.c_str() && v > 0) return static_cast<int>(v);
    return 400;
}

SvgFont resolveFont(const dom::Element* el) {
    SvgFont f;
    std::string fam = styleOr(el, "font-family", "Arial");
    // Take the first family in the list and strip quotes/whitespace.
    if (auto comma = fam.find(','); comma != std::string::npos) fam = fam.substr(0, comma);
    size_t a = fam.find_first_not_of(" \t\"'");
    size_t b = fam.find_last_not_of(" \t\"'");
    f.family = (a == std::string::npos) ? "Arial" : fam.substr(a, b - a + 1);
    if (f.family.empty()) f.family = "Arial";
    f.size = styleFloat(el, "font-size", 16.0f);
    f.weight = parseWeight(styleOr(el, "font-weight", "400"));
    std::string st = styleOr(el, "font-style", "normal");
    f.italic = (st == "italic" || st == "oblique");
    return f;
}

// text-anchor -> 0 start, 1 middle, 2 end.
int anchorMode(const dom::Element* el) {
    std::string a = styleOr(el, "text-anchor", "start");
    if (a == "middle") return 1;
    if (a == "end") return 2;
    return 0;
}

// dominant-baseline / alignment-baseline -> downward baseline shift (user
// units) so the named baseline lands on the run's anchor y.
float baselineShift(const dom::Element* el, const SvgFont& font, SvgTextMeasurer& m) {
    std::string key = styleOr(el, "dominant-baseline", "auto");
    if (key == "auto" || key.empty()) key = styleOr(el, "alignment-baseline", "auto");
    if (key.empty() || key == "auto" || key == "alphabetic" || key == "baseline")
        return 0.0f;
    float asc = 0, desc = 0, xh = 0;
    m.vmetrics(font, asc, desc, xh);
    if (key == "middle" || key == "mathematical") return xh * 0.5f;
    if (key == "central") return (asc - desc) * 0.5f;
    if (key == "hanging" || key == "text-before-edge") return asc;
    if (key == "text-after-edge" || key == "ideographic") return -desc;
    return 0.0f;
}

bool isTextContainer(const std::string& tag) {
    return tag == "tspan" || tag == "a";
}

// Streaming SVG whitespace collapse (xml:space="default"): newlines removed,
// tabs->space, runs of space collapsed to one, leading space of the whole
// <text> trimmed. State is shared across sibling text nodes so a single inter-
// word space survives across element boundaries but never doubles.
std::string collapseWs(const std::string& in, bool& prevSpace, bool& any) {
    std::string out;
    bool local = prevSpace;   // is the char before `out` a space (or start)?
    for (char c : in) {
        if (c == '\n' || c == '\r') continue;
        bool ws = (c == ' ' || c == '\t');
        if (ws) {
            if (!local && any) { out += ' '; local = true; }
        } else {
            out += c;
            local = false;
            any = true;
        }
    }
    prevSpace = local;
    return out;
}

struct WalkState {
    SvgTextMeasurer& m;
    std::vector<SvgTextRun> runs;
    std::vector<std::pair<size_t, int>> chunks;  // (firstRunIndex, anchorMode)
    float cx = 0, cy = 0;
    bool prevSpace = true;   // trims leading whitespace
    bool any = false;
};

void startChunk(WalkState& st, int anchor) {
    st.chunks.push_back({st.runs.size(), anchor});
}

void walk(const dom::Element* el, bool isRoot, WalkState& st) {
    std::string tag = lower(el->tagName());
    SvgFont font = resolveFont(el);

    bool hasX = !attrOf(el, "x").empty();
    bool hasY = !attrOf(el, "y").empty();
    if (isRoot) {
        if (hasX) st.cx = attrFloat(el, "x");
        if (hasY) st.cy = attrFloat(el, "y");
        startChunk(st, anchorMode(el));
    } else {
        if (hasX) { st.cx = attrFloat(el, "x"); startChunk(st, anchorMode(el)); }
        if (hasY) st.cy = attrFloat(el, "y");
    }
    st.cx += attrFloat(el, "dx", 0.0f);
    st.cy += attrFloat(el, "dy", 0.0f);

    float bshift = baselineShift(el, font, st.m);

    for (dom::Node* child : el->childNodes()) {
        if (child->nodeType() == dom::NodeType::Text) {
            auto* tn = static_cast<dom::TextNode*>(child);
            std::string t = collapseWs(tn->data(), st.prevSpace, st.any);
            if (t.empty()) continue;
            float w = st.m.advance(t, font);
            float asc = 0, desc = 0, xh = 0;
            st.m.vmetrics(font, asc, desc, xh);
            SvgTextRun run;
            run.text = std::move(t);
            run.x = st.cx;
            run.baseline = st.cy + bshift;
            run.width = w;
            run.ascent = asc;
            run.descent = desc;
            run.styleEl = el;
            run.font = font;
            st.runs.push_back(std::move(run));
            st.cx += w;
        } else if (child->nodeType() == dom::NodeType::Element) {
            auto* ce = static_cast<dom::Element*>(child);
            if (isTextContainer(lower(ce->tagName()))) walk(ce, false, st);
        }
    }
}

// Shift each chunk's runs so the text-anchor point lands correctly.
void applyAnchors(WalkState& st) {
    for (size_t i = 0; i < st.chunks.size(); ++i) {
        size_t begin = st.chunks[i].first;
        size_t end = (i + 1 < st.chunks.size()) ? st.chunks[i + 1].first : st.runs.size();
        if (end <= begin) continue;
        int anchor = st.chunks[i].second;
        if (anchor == 0) continue;  // start
        float left = st.runs[begin].x;
        float right = st.runs[end - 1].x + st.runs[end - 1].width;
        float shift = (anchor == 1) ? -(right - left) * 0.5f : -(right - left);
        for (size_t j = begin; j < end; ++j) st.runs[j].x += shift;
    }
}

const dom::Element* textRootOf(const dom::Element* el) {
    for (const dom::Element* p = el; p; p = p->parentElement())
        if (lower(p->tagName()) == "text") return p;
    return nullptr;
}

// Whether run element `runEl` is `el` or nested inside it (up to `root`).
bool belongsTo(const dom::Element* runEl, const dom::Element* el,
               const dom::Element* root) {
    for (const dom::Element* p = runEl; p; p = p->parentElement()) {
        if (p == el) return true;
        if (p == root) break;
    }
    return false;
}

} // namespace

float RendererTextMeasurer::advance(const std::string& text, const SvgFont& f) {
    if (!r) return 0.0f;
    return r->measureText(text,
        render::FontRef{f.family, f.size, f.weight, f.italic}).width;
}

void RendererTextMeasurer::vmetrics(const SvgFont& f, float& ascent,
                                    float& descent, float& xHeight) {
    if (!r) { ascent = f.size * 0.8f; descent = f.size * 0.2f; xHeight = f.size * 0.5f; return; }
    auto tm = r->measureText("",
        render::FontRef{f.family, f.size, f.weight, f.italic});
    ascent = tm.ascent; descent = tm.descent; xHeight = tm.xHeight;
}

std::vector<SvgTextRun> layoutSvgText(const dom::Element* textEl, SvgTextMeasurer& m) {
    WalkState st{m};
    if (textEl) walk(textEl, /*isRoot=*/true, st);
    applyAnchors(st);
    return std::move(st.runs);
}

bool svgTextElementBounds(const dom::Element* el, SvgTextMeasurer& m, SkRect& out) {
    if (!el) return false;
    const dom::Element* root = textRootOf(el);
    if (!root) return false;
    std::vector<SvgTextRun> runs = layoutSvgText(root, m);

    bool wantAll = (el == root);
    bool any = false;
    float l = 0, t = 0, r = 0, b = 0;
    for (const auto& run : runs) {
        if (!wantAll && !belongsTo(run.styleEl, el, root)) continue;
        float rl = run.x, rt = run.baseline - run.ascent;
        float rr = run.x + run.width, rb = run.baseline + run.descent;
        if (!any) { l = rl; t = rt; r = rr; b = rb; any = true; }
        else {
            l = std::min(l, rl); t = std::min(t, rt);
            r = std::max(r, rr); b = std::max(b, rb);
        }
    }
    if (!any) return false;
    out = SkRect::MakeLTRB(l, t, r, b);
    return true;
}

} // namespace bro::layout
