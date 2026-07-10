#include "render/font_fallback.h"

#include <include/core/SkString.h>

namespace bro::render {

namespace {

// Decode one UTF-8 codepoint starting at `s[i]`. Advances `i` past the
// consumed bytes. Returns the codepoint, or 0xFFFD (replacement char) for
// malformed input (advancing by one byte to make progress).
int32_t nextUtf8(std::string_view s, std::size_t& i) {
    if (i >= s.size()) return 0;
    unsigned char c = static_cast<unsigned char>(s[i]);
    int32_t cp = 0;
    int extra = 0;
    if (c < 0x80) { cp = c; extra = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { ++i; return 0xFFFD; }  // stray continuation byte
    ++i;
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size()) return 0xFFFD;
        unsigned char cc = static_cast<unsigned char>(s[i]);
        if ((cc & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (cc & 0x3F);
        ++i;
    }
    return cp;
}

// Byte length of the well-formed UTF-8 sequence starting at `s[i]`, or 0 if
// the bytes there are not well-formed (truncated, overlong, surrogate,
// > U+10FFFF, stray continuation). Follows RFC 3629 / the Unicode table of
// valid byte ranges — strictly narrower than Skia's validator, so any
// sequence accepted here is safe to hand to SkFont/SkCanvas text calls.
std::size_t wellFormedUtf8SeqLen(std::string_view s, std::size_t i) {
    const auto b = [&](std::size_t k) -> unsigned {
        return static_cast<unsigned char>(s[i + k]);
    };
    const unsigned b0 = b(0);
    if (b0 < 0x80) return 1;
    const auto cont = [&](std::size_t k, unsigned lo = 0x80, unsigned hi = 0xBF) {
        return i + k < s.size() && b(k) >= lo && b(k) <= hi;
    };
    if (b0 >= 0xC2 && b0 <= 0xDF)
        return cont(1) ? 2 : 0;
    if (b0 == 0xE0)
        return cont(1, 0xA0) && cont(2) ? 3 : 0;             // no overlongs
    if ((b0 >= 0xE1 && b0 <= 0xEC) || b0 == 0xEE || b0 == 0xEF)
        return cont(1) && cont(2) ? 3 : 0;
    if (b0 == 0xED)
        return cont(1, 0x80, 0x9F) && cont(2) ? 3 : 0;       // no surrogates
    if (b0 == 0xF0)
        return cont(1, 0x90) && cont(2) && cont(3) ? 4 : 0;  // no overlongs
    if (b0 >= 0xF1 && b0 <= 0xF3)
        return cont(1) && cont(2) && cont(3) ? 4 : 0;
    if (b0 == 0xF4)
        return cont(1, 0x80, 0x8F) && cont(2) && cont(3) ? 4 : 0; // <= U+10FFFF
    return 0; // 0x80..0xC1, 0xF5..0xFF: never a lead byte
}

} // namespace

std::string_view ensureValidUtf8(std::string_view text, std::string& scratch) {
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t n = wellFormedUtf8SeqLen(text, i);
        if (n == 0) break;
        i += n;
    }
    if (i == text.size()) return text;  // common case: already valid

    scratch.clear();
    scratch.reserve(text.size() + 2);
    scratch.append(text, 0, i);
    while (i < text.size()) {
        std::size_t n = wellFormedUtf8SeqLen(text, i);
        if (n == 0) {
            scratch.append("\xEF\xBF\xBD");  // U+FFFD, one per offending byte
            ++i;
        } else {
            scratch.append(text, i, n);
            i += n;
        }
    }
    return scratch;
}

// -----------------------------------------------------------------------------
// FontFallbackCache
// -----------------------------------------------------------------------------

sk_sp<SkTypeface> FontFallbackCache::lookup(uint32_t primaryId, int32_t cp) const {
    auto it = map_.find(Key{primaryId, cp});
    if (it == map_.end()) return nullptr;
    return it->second;
}

void FontFallbackCache::store(uint32_t primaryId, int32_t cp, sk_sp<SkTypeface> face) {
    map_[Key{primaryId, cp}] = std::move(face);
}

// -----------------------------------------------------------------------------
// splitTextForFallback
// -----------------------------------------------------------------------------

std::vector<TextRun> splitTextForFallback(std::string_view utf8,
                                           const SkFont&    primary,
                                           SkFontMgr*       fontMgr,
                                           SkFontStyle      primaryStyle,
                                           FontFallbackCache& cache) {
    std::vector<TextRun> runs;
    if (utf8.empty()) return runs;

    sk_sp<SkTypeface> primaryTypeface = primary.refTypeface();
    if (!primaryTypeface) return runs;
    const uint32_t primaryId = primaryTypeface->uniqueID();

    // Family name hint for matchFamilyStyleCharacter — passing the family we
    // already resolved lets the platform keep script style consistent
    // (e.g. prefer Segoe UI Symbol over Arial Unicode when hinted with
    //  "Segoe UI").
    SkString hintFamily;
    primaryTypeface->getFamilyName(&hintFamily);

    std::size_t i = 0;
    std::size_t runStart = 0;
    sk_sp<SkTypeface> runFace;

    auto flushRun = [&](std::size_t end) {
        if (end <= runStart || !runFace) return;
        SkFont f = primary;
        f.setTypeface(runFace);
        runs.push_back(TextRun{runStart, end - runStart, std::move(f)});
        runFace.reset();
    };

    while (i < utf8.size()) {
        std::size_t cpStart = i;
        int32_t cp = nextUtf8(utf8, i);
        if (cp <= 0) continue;

        sk_sp<SkTypeface> face = cache.lookup(primaryId, cp);
        if (!face) {
            // Primary first — covers the vast majority of ASCII / CJK when
            // the primary is a full-coverage font.
            if (primaryTypeface->unicharToGlyph(cp) != 0) {
                face = primaryTypeface;
            } else if (fontMgr) {
                face = sk_sp<SkTypeface>(fontMgr->matchFamilyStyleCharacter(
                    hintFamily.c_str(), primaryStyle, nullptr, 0, cp));
                if (!face) face = primaryTypeface;  // no system coverage → tofu
            } else {
                face = primaryTypeface;
            }
            cache.store(primaryId, cp, face);
        }

        if (!runFace) {
            runFace = face;
            runStart = cpStart;
        } else if (face.get() != runFace.get()) {
            flushRun(cpStart);
            runFace = face;
            runStart = cpStart;
        }
    }
    flushRun(utf8.size());
    return runs;
}

} // namespace bro::render
