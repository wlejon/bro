#pragma once

// -----------------------------------------------------------------------------
// CSS font-family fallback-list resolution, shared by every text-rendering
// path (HTML/CSS raster text, Canvas 2D) so they resolve `font-family: a, b,
// c` identically.
// -----------------------------------------------------------------------------

#include <string>
#include <string_view>
#include <vector>

#include <include/core/SkFont.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkFontStyle.h>
#include <include/core/SkTypeface.h>

namespace bro::render {

// Resolve a CSS font-family value — possibly a comma-separated fallback
// list, each entry optionally quoted/padded with whitespace, e.g.
// `"system-ui, -apple-system, Segoe UI, sans-serif"` — to a concrete
// typeface. Each listed name is tried in order: first as a CSS generic
// keyword (sans-serif, serif, monospace, cursive, fantasy, system-ui) mapped
// to a real platform font, then as a literal family name. Only once every
// name in the list has failed does this fall back to the font manager's
// platform default typeface (never returns null when `mgr` is non-null).
sk_sp<SkTypeface> resolveFontFamilyList(std::string_view cssFamily,
                                         SkFontStyle style,
                                         SkFontMgr* mgr);

// One face registered by an @font-face rule. A family is usually declared
// several times, once per weight/style (Inter 400/500/600/700, ...).
struct CustomFontFace {
    std::string family;
    int weight = 400;
    bool italic = false;
    sk_sp<SkTypeface> typeface;
};

// The face CSS font matching picked, plus what the renderer must synthesize
// because no declared face was bold/italic enough (CSS Fonts 4 §5.2 step 4,
// `font-synthesis: weight style`, the default): embolden when the request is
// bold (>= 600) and the chosen face is not; slant when italic was asked for
// and only upright faces exist.
struct CustomFontMatch {
    const CustomFontFace* face = nullptr;
    bool syntheticBold = false;
    bool syntheticItalic = false;
};

// CSS Fonts 4 §5.2 font matching over the @font-face faces declared for
// `family` (compared ASCII case-insensitively): narrow by font-style (italic
// prefers italic faces, normal prefers normal ones, falling back to the
// other), then by font-weight — for a desired weight between 400 and 500,
// weights in [desired, 500] ascending, then below desired descending, then
// above 500 ascending; below 400, lighter descending then heavier ascending;
// above 500, heavier ascending then lighter descending. `face` is null when
// no face of that family is registered.
CustomFontMatch matchCustomFontFace(const std::vector<CustomFontFace>& faces,
                                    std::string_view family,
                                    int weight, bool italic);

// Apply a match's synthesis to a font built from its face.
void applyFontSynthesis(SkFont& font, const CustomFontMatch& match);

// resolveFontFamilyList with @font-face faces consulted first for each name
// in the list, the way a browser's font list puts web fonts ahead of local
// ones. `match` reports the custom face chosen (face null when the name
// resolved to a platform font).
sk_sp<SkTypeface> resolveFontFamilyList(std::string_view cssFamily,
                                         SkFontStyle style,
                                         SkFontMgr* mgr,
                                         const std::vector<CustomFontFace>& faces,
                                         CustomFontMatch* match);

} // namespace bro::render
