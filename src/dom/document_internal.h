#pragma once
// The computed-style diff shared by the two halves of the restyle pass.
//
// resolveStylesRecursive (document_style.cpp) asks it of a real element and
// applyPseudo (document_gencontent.cpp) asks it of a ::before/::after style, so
// the classification tables and the diff itself live here rather than being
// duplicated in both translation units. Nothing outside those two files needs
// it; this header is private to src/dom.

#include "css/cascade.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace bro::dom {

// Per-property flags the restyle diff needs. Both questions the diff asks are
// answered from one lookup instead of two separate hash-set probes per property:
//
//   PAINT_ONLY — value affects only how a box is painted, never its size or that
//     of anything around it. A restyle (e.g. a :hover) touching ONLY these can
//     re-record without re-running layout. Anything NOT flagged is treated as
//     layout-affecting — the conservative default, so a new or unrecognised
//     property forces a layout rather than risking a stale one.
//   INHERITED — the property inherits. This is the only channel through which a
//     change to one element's style can reach a descendant that did not itself
//     change: if none of these moved, no descendant's style can differ.
//
// The two source lists are kept verbatim below (bro's own policy — deliberately
// NOT htmlayout's classification, which differs on custom properties and on the
// paint-only/layout-read distinction) and unioned once into a single flag map.
enum PropFlags : uint8_t { PROP_PAINT_ONLY = 1, PROP_INHERITED = 2 };

inline uint8_t propFlags(const std::string& p) {
    // Custom properties (--*) inherit; they are never paint-only.
    if (p.size() >= 2 && p[0] == '-' && p[1] == '-') return PROP_INHERITED;
    static const std::unordered_map<std::string, uint8_t> kFlags = [] {
        static const char* kPaintOnly[] = {
            "color", "background", "background-color", "background-image",
            "background-position", "background-position-x", "background-position-y",
            "background-size", "background-repeat", "background-origin",
            "background-clip", "background-attachment", "background-blend-mode",
            "mix-blend-mode", "isolation",
            "border-color", "border-top-color", "border-right-color",
            "border-bottom-color", "border-left-color",
            "outline", "outline-color", "outline-style", "outline-width",
            "outline-offset",
            "box-shadow", "text-shadow",
            "border-radius", "border-top-left-radius", "border-top-right-radius",
            "border-bottom-left-radius", "border-bottom-right-radius",
            "opacity", "transform", "transform-origin", "transform-style",
            "translate", "rotate", "scale",
            "perspective", "perspective-origin", "backface-visibility",
            "transition", "transition-property", "transition-duration",
            "transition-timing-function", "transition-delay",
            "animation", "animation-name", "animation-duration",
            "animation-timing-function", "animation-delay",
            "animation-iteration-count", "animation-direction",
            "animation-fill-mode", "animation-play-state",
            "cursor", "text-decoration", "text-decoration-color",
            "text-decoration-line", "text-decoration-style",
            "text-decoration-thickness", "text-underline-offset",
            "filter", "backdrop-filter", "visibility", "pointer-events",
            "z-index", "user-select", "-webkit-user-select",
            "caret-color", "accent-color", "fill", "stroke", "stroke-width",
            "clip-path", "mask", "will-change", "-webkit-text-fill-color",
            "text-emphasis-color", "scrollbar-color", "color-scheme",
            "object-position", "image-rendering",
        };
        static const char* kInherited[] = {
            "color", "cursor", "direction", "visibility", "pointer-events",
            "font", "font-family", "font-size", "font-style", "font-variant",
            "font-weight", "font-stretch", "font-feature-settings",
            "font-variant-numeric", "-webkit-font-smoothing",
            "letter-spacing", "line-height", "word-spacing", "text-align",
            "text-align-last", "text-indent", "text-justify", "text-shadow",
            "text-transform", "text-rendering", "-webkit-text-fill-color",
            "text-emphasis-color", "text-orientation", "writing-mode",
            "white-space", "word-break", "word-wrap", "overflow-wrap", "hyphens",
            "tab-size", "quotes", "orphans", "widows",
            "list-style", "list-style-image", "list-style-position",
            "list-style-type",
            "border-collapse", "border-spacing", "empty-cells", "caption-side",
            "caret-color", "accent-color", "color-scheme", "scrollbar-color",
            "user-select", "-webkit-user-select", "image-rendering",
            "fill", "stroke", "stroke-width", "text-anchor", "paint-order",
        };
        std::unordered_map<std::string, uint8_t> m;
        for (const char* k : kPaintOnly) m[k] |= PROP_PAINT_ONLY;
        for (const char* k : kInherited) m[k] |= PROP_INHERITED;
        return m;
    }();
    auto it = kFlags.find(p);
    return it == kFlags.end() ? 0 : it->second;
}

// Two questions the caller asks of a re-resolve, answered in one walk:
//   layoutAffecting — did any layout-affecting property differ? (paint-only
//     props ignored, so a :hover that only recolours skips layout.)
//   inherited       — did any inherited property differ? i.e. can this
//     re-resolve have changed a descendant's style.
struct StyleChange {
    bool layoutAffecting = false;
    bool inherited = false;
};

// Combined single-pass diff replacing the old separate layoutAffectingChanged /
// inheritedChanged, each of which walked both maps in full (four traversals) and
// paid the property classification and the cross-map find() twice per key. Here
// each key is visited once, classified once for each question, and the expensive
// find() is skipped entirely when neither question cares about the key.
//
// `wantLayout` lets the caller drop the layout-affecting question when the whole
// tree is already queued for relayout (fullLayout_) — the diff would tell it
// nothing. When false, `.layoutAffecting` stays false and its classification is
// never run, and the walk short-circuits as soon as the inherited answer is in.
inline StyleChange classifyStyleChange(const htmlayout::css::ComputedStyle& a,
                                       const htmlayout::css::ComputedStyle& b,
                                       bool wantLayout) {
    StyleChange c;
    bool layoutDone = !wantLayout;   // "already answered" ⇒ skip the layout half

    // Inherited custom properties are not in the maps: they are a set shared by
    // pointer down the tree (see htmlayout ComputedStyle). A different pointer
    // means an ancestor's tokens changed, and every var() below may resolve
    // differently — an inherited change, exactly like a changed `color`.
    if (a.inheritedVars != b.inheritedVars) c.inherited = true;

    // New or changed: every key in b, compared against a.
    for (const auto& [k, v] : b) {
        const uint8_t f = propFlags(k);                      // one lookup, both flags
        const bool inhRelevant = !c.inherited && (f & PROP_INHERITED);
        const bool layRelevant = !layoutDone && !(f & PROP_PAINT_ONLY);
        if (!inhRelevant && !layRelevant) continue;          // neither cares — no find
        auto it = a.find(k);
        if (it != a.end() && it->second == v) continue;      // unchanged
        if (inhRelevant) c.inherited = true;                 // added or changed
        if (layRelevant) { c.layoutAffecting = true; layoutDone = true; }
        if (c.inherited && layoutDone) return c;             // both answered
    }
    // Removed: keys in a that no longer appear in b.
    for (const auto& [k, v] : a) {
        const uint8_t f = propFlags(k);
        const bool inhRelevant = !c.inherited && (f & PROP_INHERITED);
        const bool layRelevant = !layoutDone && !(f & PROP_PAINT_ONLY);
        if (!inhRelevant && !layRelevant) continue;
        if (b.find(k) != b.end()) continue;                  // still present
        if (inhRelevant) c.inherited = true;
        if (layRelevant) { c.layoutAffecting = true; layoutDone = true; }
        if (c.inherited && layoutDone) return c;
    }
    return c;
}

} // namespace bro::dom
