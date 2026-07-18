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

// The base direction a <text> resolves against. `direction` is an inherited
// CSS property, so the cascade has already pushed the <text> element's value
// down to every <tspan>; reading it off whichever element owns the chunk gives
// the same answer as reading it off the root, and is right when a <tspan>
// overrides it.
render::TextDirection baseDirection(const dom::Element* el) {
    return styleOr(el, "direction", "ltr") == "rtl" ? render::TextDirection::RTL
                                                    : render::TextDirection::LTR;
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

// One text chunk: the runs between two absolute-x resets. Anchoring and bidi
// reordering both operate on a whole chunk, because both need its full extent.
struct Chunk {
    size_t                firstRun = 0;
    int                   anchor = 0;
    render::TextDirection direction = render::TextDirection::LTR;
};

struct WalkState {
    SvgTextMeasurer& m;
    std::vector<SvgTextRun> runs;
    std::vector<Chunk> chunks;
    float cx = 0, cy = 0;
    bool prevSpace = true;   // trims leading whitespace
    bool any = false;
};

void startChunk(WalkState& st, int anchor, render::TextDirection dir) {
    st.chunks.push_back(Chunk{st.runs.size(), anchor, dir});
}

void walk(const dom::Element* el, bool isRoot, WalkState& st) {
    std::string tag = lower(el->tagName());
    SvgFont font = resolveFont(el);
    render::TextDirection dir = baseDirection(el);

    bool hasX = !attrOf(el, "x").empty();
    bool hasY = !attrOf(el, "y").empty();
    if (isRoot) {
        if (hasX) st.cx = attrFloat(el, "x");
        if (hasY) st.cy = attrFloat(el, "y");
        startChunk(st, anchorMode(el), dir);
    } else {
        if (hasX) { st.cx = attrFloat(el, "x"); startChunk(st, anchorMode(el), dir); }
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
            float w = st.m.advance(t, font, dir);
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
            run.direction = dir;
            st.runs.push_back(std::move(run));
            st.cx += w;
        } else if (child->nodeType() == dom::NodeType::Element) {
            auto* ce = static_cast<dom::Element*>(child);
            if (isTextContainer(lower(ce->tagName()))) walk(ce, false, st);
        }
    }
}

// Reorder each chunk's runs into visual order (UAX #9 rule L2) and re-run the
// pen over them.
//
// The walk above places runs left to right in LOGICAL order, which is only the
// visual order when everything resolved to level 0. Levels are resolved over
// the chunk's concatenated text rather than per run, because that is what a
// paragraph is here: a run boundary in SVG is a <tspan>, and the W and N rules
// have to see across it — a number in `<tspan>שלום</tspan>123<tspan>עולם</tspan>`
// resolves differently depending on what surrounds it, and asking each run in
// isolation gets that wrong.
//
// Chunks that resolve uniformly at level 0 — every chunk in Latin text — skip
// this entirely and keep byte-identical positions to before bidi existed.
void reorderChunks(WalkState& st) {
    for (size_t i = 0; i < st.chunks.size(); ++i) {
        const size_t begin = st.chunks[i].firstRun;
        const size_t end = (i + 1 < st.chunks.size()) ? st.chunks[i + 1].firstRun
                                                      : st.runs.size();
        if (end - begin == 0) continue;
        const auto base = st.chunks[i].direction == render::TextDirection::RTL
                              ? render::bidi::BaseDirection::RTL
                              : render::bidi::BaseDirection::LTR;

        // Cheap out: an LTR base over text that cannot resolve to any non-zero
        // level is the overwhelmingly common case and needs no work at all.
        std::string joined;
        std::vector<size_t> runStart(end - begin, 0);
        for (size_t j = begin; j < end; ++j) {
            runStart[j - begin] = joined.size();
            joined += st.runs[j].text;
        }
        if (base == render::bidi::BaseDirection::LTR &&
            render::bidi::isTriviallyLtr(joined)) {
            continue;
        }

        render::bidi::Paragraph para = render::bidi::resolveParagraph(joined, base);
        if (para.uniform && !para.isRtlParagraph()) continue;

        // A run's level is the level of its first byte. Runs are finer than
        // level runs (they split on <tspan> and on style), which L2 tolerates:
        // reversing a span of equal-level entries gives the same answer as
        // reversing the unsplit run they came from.
        std::vector<render::bidi::Level> levels(end - begin, para.paragraphLevel);
        for (size_t k = 0; k < levels.size(); ++k) {
            const size_t off = runStart[k];
            if (off < para.levels.size()) levels[k] = para.levels[off];
        }
        std::vector<int32_t> visual = render::bidi::reorderVisual(levels);
        if (visual.size() != levels.size()) continue;

        // Re-run the pen from the chunk's origin in visual order. The origin is
        // where the first logical run was placed, which is still the chunk's
        // left edge — anchoring, which happens next, is what moves it.
        float pen = st.runs[begin].x;
        std::vector<SvgTextRun> ordered;
        ordered.reserve(visual.size());
        for (int32_t slot : visual) {
            SvgTextRun r = st.runs[begin + static_cast<size_t>(slot)];
            r.x = pen;
            pen += r.width;
            ordered.push_back(std::move(r));
        }
        for (size_t k = 0; k < ordered.size(); ++k) st.runs[begin + k] = std::move(ordered[k]);
    }
}

// Shift each chunk's runs so the text-anchor point lands correctly.
//
// `start` and `end` are direction-relative in SVG: for a right-to-left chunk
// the start edge is the RIGHT one, so the two anchors trade places. `middle`
// means the same thing either way.
void applyAnchors(WalkState& st) {
    for (size_t i = 0; i < st.chunks.size(); ++i) {
        size_t begin = st.chunks[i].firstRun;
        size_t end = (i + 1 < st.chunks.size()) ? st.chunks[i + 1].firstRun : st.runs.size();
        if (end <= begin) continue;
        int anchor = st.chunks[i].anchor;
        const bool rtl = st.chunks[i].direction == render::TextDirection::RTL;
        if (rtl && anchor == 0) anchor = 2;
        else if (rtl && anchor == 2) anchor = 0;
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

float RendererTextMeasurer::advance(const std::string& text, const SvgFont& f,
                                   render::TextDirection dir) {
    if (!r) return 0.0f;
    return r->measureText(text,
        render::FontRef{f.family, f.size, f.weight, f.italic}, dir).width;
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
    reorderChunks(st);
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
