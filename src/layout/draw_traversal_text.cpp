// Text painting: text nodes (white-space collapsing, text-transform, shadows,
// letter/word spacing, decorations, placed IFC runs), ::before / ::after
// generated content, and the FontRef for an element's computed font.

#include "layout/draw_traversal_internal.h"
#include "dom/text_node.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace bro::layout {

// Apply text-transform to a string
std::string DrawTraversal::applyTextTransform(const std::string& text,
                                              const std::string& transform) {
    if (transform == "uppercase") {
        std::string r = text;
        for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return r;
    }
    if (transform == "lowercase") {
        std::string r = text;
        for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    }
    if (transform == "capitalize") {
        std::string r = text;
        bool nextCap = true;
        for (auto& c : r) {
            if (std::isspace(static_cast<unsigned char>(c))) { nextCap = true; }
            else if (nextCap) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); nextCap = false; }
        }
        return r;
    }
    return text;
}

// Parse text-shadow: a list of `offsetX offsetY [blur] [color]`, each colour
// defaulting to the text's own (currentcolor). The caller paints the list
// back to front, so the first shadow ends up on top.
static std::vector<CssShadow> parseTextShadows(const std::string& val,
                                               const bromath::Color& textColor,
                                               const CssLengthContext& lengths) {
    if (val.empty() || val == "none") return {};
    std::vector<CssShadow> out = parseCssShadowList(val, textColor, 3, lengths);
    std::erase_if(out, [](const CssShadow& s) { return s.inset; });
    return out;
}

void DrawTraversal::drawText(dom::Node* textNode, dom::Element* parent,
                             float offsetX, float offsetY) {
    if (!textNode || !parent) return;

    auto* tn = static_cast<dom::TextNode*>(textNode);
    std::string text = tn->data();
    if (text.empty()) return;

    // Skip whitespace-only text
    bool allWhitespace = true;
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            allWhitespace = false;
            break;
        }
    }
    if (allWhitespace) return;

    auto& style = parent->computedStyle();

    // Collapse whitespace to match the CSS white-space property that the layout
    // measured. Without this, raw newlines/tabs render as tofu glyphs and the
    // drawn width drifts from the measured width.
    {
        auto wsIt = style.find("white-space");
        std::string ws = (wsIt != style.end()) ? wsIt->second : std::string("normal");
        if (ws != "pre" && ws != "pre-wrap" && ws != "pre-line") {
            std::string collapsed;
            collapsed.reserve(text.size());
            bool lastSpace = false;
            for (char c : text) {
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    if (!lastSpace) collapsed += ' ';
                    lastSpace = true;
                } else {
                    collapsed += c;
                    lastSpace = false;
                }
            }
            text = std::move(collapsed);
            if (text.empty()) return;
        }
    }
    render::FontRef fontRef = getFontRef(parent);

    // Pull vertical metrics straight from the renderer (empty-text measureText
    // returns just font metrics, no glyph shaping).
    auto baseMetrics = renderer_->measureText("", fontRef);
    float ascent  = baseMetrics.ascent;
    float descent = baseMetrics.descent;
    float lineH   = std::round(baseMetrics.ascent + baseMetrics.descent + baseMetrics.leading);
    if (lineH <= 0) lineH = fontRef.size * 1.2f;

    // Apply text-transform
    auto ttIt = style.find("text-transform");
    if (ttIt != style.end()) text = DrawTraversal::applyTextTransform(text, ttIt->second);

    // Get text color
    bromath::Color color = cfromColor8({0, 0, 0, 255});
    auto cIt = style.find("color");
    if (cIt != style.end()) tryParseColor(cIt->second, color);

    // Parse text-shadow
    std::vector<CssShadow> textShadows;
    auto tsIt = style.find("text-shadow");
    if (tsIt != style.end())
        textShadows = parseTextShadows(tsIt->second, color,
                                       shadowLengthContext(parent, style, viewportW_, viewportH_));

    // Resolve letter-spacing for the renderer. Layout already accounts for
    // it in the text-run widths (text.cpp), so the painter must add the same
    // per-glyph advance or the visible glyphs end up flush-left in their box.
    float letterSpacing = 0.0f;
    float wordSpacing = 0.0f;
    {
        float fs = 16.0f;
        auto fsIt = style.find("font-size");
        if (fsIt != style.end()) {
            char* end = nullptr;
            float v = std::strtof(fsIt->second.c_str(), &end);
            if (end != fsIt->second.c_str() && v > 0) fs = v;
        }
        auto resolveSpacing = [&](const char* prop) -> float {
            auto it = style.find(prop);
            if (it == style.end() || it->second.empty() ||
                it->second == "normal")
                return 0.0f;
            const std::string& v = it->second;
            char* end = nullptr;
            float n = std::strtof(v.c_str(), &end);
            if (end == v.c_str()) return 0.0f;
            std::string unit(end);
            if (unit == "em") return n * fs;
            if (unit == "rem") return n * 16.0f;
            return n; // px / unitless
        };
        letterSpacing = resolveSpacing("letter-spacing");
        wordSpacing = resolveSpacing("word-spacing");
    }

    // Parse text-decoration
    std::string decoration;
    auto tdIt = style.find("text-decoration");
    if (tdIt != style.end()) decoration = tdIt->second;
    if (decoration.empty()) {
        auto tdlIt = style.find("text-decoration-line");
        if (tdlIt != style.end()) decoration = tdlIt->second;
    }

    // Text-align: compute offset when layout provides a content width
    float textAlignOffset = 0;
    auto taIt = style.find("text-align");
    if (taIt != style.end() && (taIt->second == "center" || taIt->second == "right")) {
        auto& pbox = parent->layoutBox();
        float availW = pbox.contentRect.width;
        if (availW > 0) {
            auto tm = renderer_->measureText(text, fontRef);
            if (taIt->second == "center")
                textAlignOffset = (availW - tm.width) / 2.0f;
            else
                textAlignOffset = availW - tm.width;
            if (textAlignOffset < 0) textAlignOffset = 0;
        }
    }

    // Use layout-computed position if available (from IFC text positioning),
    // otherwise fall back to parent's content origin.
    // When the IFC provides positions, text-align is already applied in contentRect.x,
    // so only add textAlignOffset in the fallback path to avoid double-centering.
    float x, y;

    auto& tbox = tn->layoutBox();
    if (tbox.contentRect.width > 0) {
        x = offsetX + tbox.contentRect.x;
        y = offsetY + tbox.contentRect.y + ascent;
    } else {
        x = offsetX + textAlignOffset;
        y = offsetY + ascent;
    }

    // Helper to draw a single line of text with shadow and decoration
    auto drawLine = [&](std::string_view line, float lx, float ly) {
        auto tm = renderer_->measureText(line, fontRef);

        // Draw text shadow first (behind text). drawTextEx applies the blur
        // mask filter to the shadow paint so a non-zero blur radius produces
        // a real Gaussian halo instead of a sharp colored copy. The list
        // paints back to front, so its first shadow is on top.
        for (auto s = textShadows.rbegin(); s != textShadows.rend(); ++s) {
            renderer_->drawTextEx(line, lx + s->dx, ly + s->dy, fontRef, s->color,
                                  letterSpacing, s->blur, wordSpacing);
        }

        // Draw the text. Letter/word-spacing are applied here so visible
        // glyph advances match the widths the layout assumed.
        if (letterSpacing != 0.0f || wordSpacing != 0.0f) {
            renderer_->drawTextEx(line, lx, ly, fontRef, color, letterSpacing,
                                  0.0f, wordSpacing);
        } else {
            renderer_->drawText(line, lx, ly, fontRef, color);
        }

        // Draw text-decoration
        if (!decoration.empty() && decoration != "none") {
            float decoThickness = std::max(1.0f, ascent / 12.0f);
            if (decoration.find("underline") != std::string::npos) {
                float uy = ly + descent * 0.4f;
                renderer_->drawLine(lx, uy, lx + tm.width, uy, color, decoThickness);
            }
            if (decoration.find("overline") != std::string::npos) {
                float oy = ly - ascent;
                renderer_->drawLine(lx, oy, lx + tm.width, oy, color, decoThickness);
            }
            if (decoration.find("line-through") != std::string::npos) {
                float sy = ly - ascent * 0.35f;
                renderer_->drawLine(lx, sy, lx + tm.width, sy, color, decoThickness);
            }
        }
    };

    // Prefer the runs produced by the inline formatting context: one per
    // wrapped line segment, already positioned. This keeps drawn glyphs aligned
    // with the layout — and therefore with selection highlights and hit tests.
    const auto& runs = tbox.textRuns;
    if (!runs.empty()) {
        std::string transform;
        if (ttIt != style.end()) transform = ttIt->second;
        for (const auto& run : runs) {
            if (run.text.empty()) continue;
            std::string line = transform.empty()
                ? run.text
                : applyTextTransform(run.text, transform);
            float lx = offsetX + run.x;
            float ly = offsetY + run.y + ascent;
            drawLine(line, lx, ly);
        }
        return;
    }

    // Handle multi-line text (newlines in pre/pre-wrap)
    auto wsIt = style.find("white-space");
    bool preserveNewlines = false;
    if (wsIt != style.end()) {
        const auto& ws = wsIt->second;
        preserveNewlines = (ws == "pre" || ws == "pre-wrap" || ws == "pre-line");
    }

    if (preserveNewlines && text.find('\n') != std::string::npos) {
        float curY = y;
        size_t start = 0;
        while (start < text.size()) {
            size_t nl = text.find('\n', start);
            if (nl == std::string::npos) nl = text.size();
            if (nl > start) {
                std::string_view line(text.data() + start, nl - start);
                drawLine(line, x, curY);
            }
            curY += lineH;
            start = nl + 1;
        }
    } else {
        drawLine(text, x, y);
    }
}

void DrawTraversal::drawPseudo(dom::Element* host, const std::string& which,
                                float offsetX, float offsetY) {
    if (!host) return;
    if (!host->hasPseudo(which)) return;
    auto& style = host->pseudoStyle(which);
    auto& pbox = host->pseudoBox(which);

    // Paint the pseudo box's background and border first — a generated-content
    // box renders like any element, and a `content: ""` box can still carry a
    // visible background/border (e.g. a coloured block spacer or a badge dot).
    {
        float bx = offsetX + pbox.contentRect.x - pbox.padding.left - pbox.border.left;
        float by = offsetY + pbox.contentRect.y - pbox.padding.top - pbox.border.top;
        float bw = pbox.fullWidth();
        float bh = pbox.fullHeight();
        if (bw > 0.0f && bh > 0.0f) {
            render::Radii radii = getRadii(style, bw, bh);
            bool rounded = !radii.isZero();
            auto bgIt = style.find("background-color");
            if (bgIt != style.end() && !bgIt->second.empty()) {
                bromath::Color bc;
                if (tryParseColor(bgIt->second, bc) && bc.a > 0) {
                    if (rounded) renderer_->fillRoundRectRadii(bx, by, bw, bh, radii, bc);
                    else         renderer_->fillRect(bx, by, bw, bh, bc);
                }
            }
            // Uniform solid borders, painted per side as rectangles (dashed /
            // dotted / rounded-corner borders on pseudos are uncommon and fall
            // back to solid — sufficient for generated badges and rules).
            const char* wp[4] = {"border-top-width","border-right-width","border-bottom-width","border-left-width"};
            const char* sp[4] = {"border-top-style","border-right-style","border-bottom-style","border-left-style"};
            const char* cp[4] = {"border-top-color","border-right-color","border-bottom-color","border-left-color"};
            float bt[4];
            for (int i = 0; i < 4; ++i) {
                auto stIt = style.find(sp[i]);
                std::string st = (stIt == style.end()) ? "none" : stIt->second;
                bt[i] = (st == "none" || st == "hidden") ? 0.0f : styleLengthPx(style, wp[i]);
            }
            auto sideColor = [&](int i) {
                bromath::Color c = cfromColor8({0, 0, 0, 255});
                auto it = style.find(cp[i]);
                if (it != style.end() && !it->second.empty()) tryParseColor(it->second, c);
                return c;
            };
            if (bt[0] > 0) renderer_->fillRect(bx, by, bw, bt[0], sideColor(0));                    // top
            if (bt[2] > 0) renderer_->fillRect(bx, by + bh - bt[2], bw, bt[2], sideColor(2));       // bottom
            if (bt[3] > 0) renderer_->fillRect(bx, by, bt[3], bh, sideColor(3));                    // left
            if (bt[1] > 0) renderer_->fillRect(bx + bw - bt[1], by, bt[1], bh, sideColor(1));       // right
        }
    }

    // Font from pseudo style — pseudo styles inherit from the host but may
    // override font-* / color, so we cannot reuse the host's font handle.
    std::string family = "Arial";
    auto famIt = style.find("font-family");
    if (famIt != style.end() && !famIt->second.empty()) {
        family = famIt->second;
        if (family.front() == '"' || family.front() == '\'') {
            family = family.substr(1, family.size() - 2);
        }
    }
    float size = 16.0f;
    auto sizeIt = style.find("font-size");
    if (sizeIt != style.end()) {
        char* end = nullptr;
        float v = std::strtof(sizeIt->second.c_str(), &end);
        if (end != sizeIt->second.c_str() && v > 0) size = v;
    }
    int weight = 400;
    auto weightIt = style.find("font-weight");
    if (weightIt != style.end()) {
        const auto& w = weightIt->second;
        if (w == "bold") weight = 700;
        else if (w == "lighter") weight = 100;
        else if (w == "normal" || w.empty()) weight = 400;
        else {
            char* end = nullptr;
            long v = std::strtol(w.c_str(), &end, 10);
            if (end != w.c_str() && v > 0) weight = static_cast<int>(v);
        }
    }
    bool italic = false;
    auto styleIt = style.find("font-style");
    if (styleIt != style.end()) {
        italic = (styleIt->second == "italic" || styleIt->second == "oblique");
    }
    render::FontRef fontRef{family, size, weight, italic};
    auto fm = renderer_->measureText("", fontRef);
    float ascent = fm.ascent;

    bromath::Color color = cfromColor8({0, 0, 0, 255});
    auto cIt = style.find("color");
    if (cIt != style.end()) tryParseColor(cIt->second, color);

    std::vector<CssShadow> textShadows;
    auto tsIt = style.find("text-shadow");
    if (tsIt != style.end())
        textShadows = parseTextShadows(tsIt->second, color,
                                       shadowLengthContext(host, style, viewportW_, viewportH_));

    // Resolve letter-spacing for the pseudo's own style (pseudos inherit it
    // by default but the rule may override).
    float letterSpacing = 0.0f;
    float wordSpacing = 0.0f;
    {
        auto resolveSpacing = [&](const char* prop) -> float {
            auto it = style.find(prop);
            if (it == style.end() || it->second.empty() ||
                it->second == "normal")
                return 0.0f;
            const std::string& v = it->second;
            char* end = nullptr;
            float n = std::strtof(v.c_str(), &end);
            if (end == v.c_str()) return 0.0f;
            std::string unit(end);
            if (unit == "em") return n * size;
            if (unit == "rem") return n * 16.0f;
            return n;
        };
        letterSpacing = resolveSpacing("letter-spacing");
        wordSpacing = resolveSpacing("word-spacing");
    }

    // Use placed runs lifted onto pseudoBox by the layout adapter.
    // Run positions are relative to the pseudo wrapper's content origin
    // (recursive layoutInline produces a local IFC), so add the wrapper's
    // contentRect to translate them into the host's IFC coord space.
    float baseX = offsetX + pbox.contentRect.x;
    float baseY = offsetY + pbox.contentRect.y;
    for (const auto& run : pbox.textRuns) {
        if (run.text.empty()) continue;
        float lx = baseX + run.x;
        float ly = baseY + run.y + ascent;
        for (auto s = textShadows.rbegin(); s != textShadows.rend(); ++s) {
            renderer_->drawTextEx(run.text, lx + s->dx, ly + s->dy, fontRef, s->color,
                                  letterSpacing, s->blur, wordSpacing);
        }
        if (letterSpacing != 0.0f || wordSpacing != 0.0f) {
            renderer_->drawTextEx(run.text, lx, ly, fontRef, color,
                                  letterSpacing, 0.0f, wordSpacing);
        } else {
            renderer_->drawText(run.text, lx, ly, fontRef, color);
        }
    }
}

render::FontRef DrawTraversal::getFontRef(dom::Element* elem) {
    auto& style = elem->computedStyle();

    // Family: borrow the string from computedStyle. Quoted forms get unquoted
    // into a side string stashed on the element so the string_view stays
    // valid until the next style resolution.
    std::string_view family = "Arial";
    auto famIt = style.find("font-family");
    if (famIt != style.end() && !famIt->second.empty()) {
        const std::string& v = famIt->second;
        if (!v.empty() && (v.front() == '"' || v.front() == '\'')) {
            // Strip surrounding quotes via a substring view — the underlying
            // string in computedStyle is stable for the draw pass.
            if (v.size() >= 2 && v.back() == v.front())
                family = std::string_view(v).substr(1, v.size() - 2);
            else
                family = v;
        } else {
            family = v;
        }
    }

    float size = 16.0f;
    auto sizeIt = style.find("font-size");
    if (sizeIt != style.end()) {
        char* end = nullptr;
        float v = std::strtof(sizeIt->second.c_str(), &end);
        if (end != sizeIt->second.c_str() && v > 0) size = v;
    }

    int weight = 400;
    auto weightIt = style.find("font-weight");
    if (weightIt != style.end()) {
        const auto& w = weightIt->second;
        if (w == "bold") weight = 700;
        else if (w == "lighter") weight = 100;
        else if (w == "normal" || w.empty()) weight = 400;
        else {
            char* end = nullptr;
            long v = std::strtol(w.c_str(), &end, 10);
            if (end != w.c_str() && v > 0) weight = static_cast<int>(v);
        }
    }

    bool italic = false;
    auto styleIt = style.find("font-style");
    if (styleIt != style.end()) {
        italic = (styleIt->second == "italic" || styleIt->second == "oblique");
    }

    return render::FontRef{family, size, weight, italic};
}

} // namespace bro::layout
