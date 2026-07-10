#pragma once

// -----------------------------------------------------------------------------
// Per-glyph font fallback for Skia text rendering.
//
// The SkiaRenderer (and CPU raster variants) draw UTF-8 strings with a single
// SkFont selected via CSS family. When that typeface lacks a glyph for a
// codepoint (e.g. &#x2B1A; in plain Arial), Skia renders `.notdef` — a tofu
// square. This helper splits a UTF-8 string into runs where each run's
// codepoints are all covered by a single typeface, using
// SkFontMgr::matchFamilyStyleCharacter (DirectWrite system fallback on
// Windows; FontConfig on Linux) to find a typeface for missing glyphs.
//
// Each renderer owns a FontFallbackCache (codepoint → typeface) keyed by the
// primary typeface ID so repeated draws don't re-query the font manager.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <include/core/SkFont.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkFontStyle.h>
#include <include/core/SkTypeface.h>

namespace bro::render {

struct TextRun {
    std::size_t start;   // byte offset into original UTF-8 string
    std::size_t length;  // byte length of this run
    SkFont      font;    // derived from primary; typeface swapped to the
                         // one that covers this run's glyphs
};

/// Cache of per-codepoint fallback typefaces. Keyed by (primary typeface ID,
/// codepoint). The stored typeface may be the primary itself (meaning the
/// primary covers that codepoint — no fallback needed). Populated lazily by
/// splitTextForFallback; owned by the renderer so it survives between frames.
class FontFallbackCache {
public:
    /// Look up the typeface to use for `cp` when the primary is `primaryId`.
    /// Returns nullptr if not cached yet.
    sk_sp<SkTypeface> lookup(uint32_t primaryId, int32_t cp) const;

    /// Remember that `face` should be used for `cp` under `primaryId`.
    void store(uint32_t primaryId, int32_t cp, sk_sp<SkTypeface> face);

    /// Drop everything. Call when fonts are released.
    void clear() { map_.clear(); }

private:
    struct Key {
        uint32_t primaryId;
        int32_t  cp;
        bool operator==(const Key& o) const noexcept {
            return primaryId == o.primaryId && cp == o.cp;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            return std::hash<uint64_t>{}(
                (static_cast<uint64_t>(k.primaryId) << 32) ^
                static_cast<uint64_t>(static_cast<uint32_t>(k.cp)));
        }
    };
    std::unordered_map<Key, sk_sp<SkTypeface>, KeyHash> map_;
};

/// Split `utf8` into runs of same-typeface codepoints, using `fontMgr` to
/// resolve fallbacks for codepoints the primary font lacks. Each returned
/// SkFont inherits size/edging/skew/etc. from `primary` with only the
/// typeface swapped. `cache` is populated as fallbacks are resolved.
///
/// Returns an empty vector for empty input. Malformed UTF-8 sequences are
/// consumed byte-by-byte with replacement character semantics (no throw).
std::vector<TextRun> splitTextForFallback(std::string_view utf8,
                                           const SkFont&    primary,
                                           SkFontMgr*       fontMgr,
                                           SkFontStyle      primaryStyle,
                                           FontFallbackCache& cache);

/// Guarantee `text` is valid UTF-8 before it reaches any SkFont/SkCanvas text
/// call. Skia's text APIs are release-fatal on invalid input (SkFont::countText
/// returns -1 and AutoSTArray(-1) aborts the process), so every renderer text
/// entry point must pass its input through this first.
///
/// Valid input is returned as-is (no copy). Invalid input — truncated or
/// overlong sequences, stray continuation bytes, surrogate encodings (WTF-8
/// lone surrogates from JS strings), codepoints above U+10FFFF — is copied
/// into `scratch` with each offending sequence replaced by U+FFFD, and a view
/// of `scratch` is returned. `scratch` must outlive the returned view.
std::string_view ensureValidUtf8(std::string_view text, std::string& scratch);

} // namespace bro::render
