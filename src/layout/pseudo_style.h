#pragma once

#include "css/cascade.h"
#include "dom/document.h"
#include "dom/element.h"
#include "layout/control_text.h"
#include "layout/draw_traversal.h"
#include "layout/element_ref_adapter.h"
#include "render/renderer.h"

#include <cstdlib>
#include <string>

// Styled-box pseudo-elements (::placeholder, ::selection). Unlike ::before/
// ::after these generate no content — they restyle paint the engine already
// does (placeholder text, selection wash). Consumers resolve the pseudo style
// for the originating element and read the small property subset that applies;
// an empty result (no rule targets the pseudo) means "use the legacy default
// paint", which keeps unstyled apps pixel-identical.

namespace bro::layout {

// Resolve ::<name> for `elem` from its document's cascade. Returns an empty
// style when the element is detached or no rule in the cascade targets the
// pseudo (the hasPseudoElementRules gate makes that miss cheap).
inline htmlayout::css::ComputedStyle resolveStyledPseudo(dom::Element* elem,
                                                         const std::string& name) {
    if (!elem || !elem->document()) return {};
    auto& cascade = elem->document()->cascade();
    if (!cascade.hasPseudoElementRules(name)) return {};
    auto* adapter = ElementRefAdapter::getOrCreate(elem);
    if (!adapter) return {};
    return cascade.resolvePseudo(*adapter, name, elem->computedStyle());
}

// Apply the font-* subset of a resolved pseudo style onto a FontRef.
// The FontRef's family string_view borrows from `style` — the style map must
// outlive the ref (keep it a local in the draw function).
inline void applyPseudoFont(const htmlayout::css::ComputedStyle& style,
                            render::FontRef& font) {
    auto fam = style.find("font-family");
    if (fam != style.end() && !fam->second.empty())
        font.family = fam->second;

    auto sz = style.find("font-size");
    if (sz != style.end()) {
        char* end = nullptr;
        float v = std::strtof(sz->second.c_str(), &end);
        if (end != sz->second.c_str() && v > 0) font.size = v;
    }

    auto wt = style.find("font-weight");
    if (wt != style.end() && !wt->second.empty()) {
        const std::string& w = wt->second;
        if (w == "bold" || w == "bolder") font.weight = 700;
        else if (w == "normal") font.weight = 400;
        else if (w == "lighter") font.weight = 300;
        else {
            char* end = nullptr;
            long v = std::strtol(w.c_str(), &end, 10);
            if (end != w.c_str() && v >= 1 && v <= 1000)
                font.weight = static_cast<int>(v);
        }
    }

    auto st = style.find("font-style");
    if (st != style.end())
        font.italic = (st->second == "italic" || st->second == "oblique");
}

// Parse `prop` from a resolved pseudo style into a color; false when the
// property is absent, empty, or unparsable.
inline bool pseudoColor(const htmlayout::css::ComputedStyle& style,
                        const char* prop, bromath::Color& out) {
    auto it = style.find(prop);
    if (it == style.end() || it->second.empty()) return false;
    return DrawTraversal::tryParseColor(it->second, out);
}

// The wash painted behind selected control text: ::selection background-color
// when a matching rule resolves a visible one, else the legacy accent wash
// (an explicit `transparent` also falls back — indistinguishable from the
// filled-in initial value).
inline bromath::Color selectionWash(const htmlayout::css::ComputedStyle& selStyle,
                                    const bromath::Color& accent) {
    if (!selStyle.empty()) {
        bromath::Color c;
        if (pseudoColor(selStyle, "background-color", c) && c.a > 0.0f) return c;
    }
    return selectionFill(accent);
}

// Opacity multiplier from a resolved pseudo style (1.0 when absent/invalid).
inline float pseudoOpacity(const htmlayout::css::ComputedStyle& style) {
    auto it = style.find("opacity");
    if (it == style.end() || it->second.empty()) return 1.0f;
    char* end = nullptr;
    float v = std::strtof(it->second.c_str(), &end);
    if (end == it->second.c_str()) return 1.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

} // namespace bro::layout
