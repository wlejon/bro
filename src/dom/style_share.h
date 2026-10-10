#pragma once
// Style sharing: what an element's computed style was resolved from.
//
// htmlayout splits the cascade in two (Cascade::matchRules / resolveMatched):
// the rules an element matches decide everything the stylesheet says about it,
// and the computed style is then a function of those rules, the inline style,
// the values the parent hands down, and — outside the cascade — the few
// document-wide inputs bro's own computed-value steps read (the root font size
// for rem, the media context for the colour scheme). A StyleShareKey records
// all of them. Two resolutions with equal keys produce equal styles, so:
//
//   * an element whose key is unchanged since its style was resolved keeps
//     that style (a row put back into the document, a subtree re-resolved
//     because an ancestor's class changed but nothing it matches moved);
//   * an element whose key equals one resolved earlier in the same pass copies
//     that element's style (twenty-five rows' identical cells).
//
// Elements whose style also reads their attributes (SVG presentation
// attributes, table spans, <svg width>), and a sheet that forces `inherit` onto
// a non-inherited property (which makes any parent value an input), never get
// a key: they always resolve in full. See Document::resolveStylesRecursive.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bro::dom {

struct StyleShareKey {
    uint64_t cascadeGeneration = 0;   // htmlayout Cascade::generation()
    uint64_t mediaGeneration = 0;     // Document's media context
    uint32_t rootFontSizeBits = 0;    // rem's reference, as resolved at the time
    uint64_t parentToken = 0;         // the parent's handed-down values (0: no parent)
    std::string tag;
    std::string inlineStyle;
    std::vector<uint32_t> rules;      // Cascade::matchRules()
    size_t hash = 0;

    void computeHash() {
        size_t h = std::hash<std::string>{}(inlineStyle);
        auto mix = [&h](uint64_t v) { h ^= static_cast<size_t>(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
        mix(cascadeGeneration);
        mix(mediaGeneration);
        mix(rootFontSizeBits);
        mix(parentToken);
        mix(std::hash<std::string>{}(tag));
        for (uint32_t r : rules) mix(r);
        hash = h;
    }

    bool operator==(const StyleShareKey& o) const {
        return hash == o.hash && cascadeGeneration == o.cascadeGeneration &&
               mediaGeneration == o.mediaGeneration && rootFontSizeBits == o.rootFontSizeBits &&
               parentToken == o.parentToken && rules == o.rules && tag == o.tag &&
               inlineStyle == o.inlineStyle;
    }
};

} // namespace bro::dom
