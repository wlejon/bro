#pragma once

// -----------------------------------------------------------------------------
// CSS font-family fallback-list resolution, shared by every text-rendering
// path (HTML/CSS raster text, Canvas 2D) so they resolve `font-family: a, b,
// c` identically.
// -----------------------------------------------------------------------------

#include <string_view>

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

} // namespace bro::render
