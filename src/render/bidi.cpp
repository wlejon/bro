#include "render/bidi.h"

#include <algorithm>
#include <cstring>

#if BRO_WITH_ICU_BIDI
// The ONE translation unit in bro that sees an ICU header. Everything ICU
// declares here is renamed to a `_skia` suffix by the defines the build sets on
// this file's target — see third_party/skia/skia_modules.cmake.
#include <unicode/ubidi.h>
#include <unicode/utypes.h>
#endif

namespace bro::render::bidi {

namespace {

// --- UTF-8/UTF-16 conversion --------------------------------------------
//
// ICU's bidi works on UTF-16; every layer above bro::render speaks UTF-8. Both
// conversions have to carry an index map, because a resolved level lands on a
// UTF-16 unit and has to be reported against a UTF-8 byte.

struct Utf16Text {
    std::vector<uint16_t>    units;
    // utf8Of[i] = byte offset in the source UTF-8 of the character that
    // produced UTF-16 unit i. Both units of a surrogate pair map to the same
    // byte offset, which is what makes the level lookup below a plain index.
    std::vector<std::size_t> utf8Of;
};

// Decode one UTF-8 sequence. Returns the codepoint and advances `i`. Malformed
// bytes decode as U+FFFD and consume one byte, so this can never loop forever
// or run past the end regardless of what the caller hands it.
uint32_t nextUtf8(std::string_view s, std::size_t& i) {
    const auto at = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c0 = at(i);
    std::size_t need = 0;
    uint32_t cp = 0;
    if ((c0 & 0x80) == 0x00)      { cp = c0;        need = 0; }
    else if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1F; need = 1; }
    else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0F; need = 2; }
    else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07; need = 3; }
    else                          { ++i; return 0xFFFD; }
    if (i + need >= s.size()) {
        // Truncated sequence at the end of the string.
        ++i;
        return 0xFFFD;
    }
    for (std::size_t k = 1; k <= need; ++k) {
        const unsigned char cc = at(i + k);
        if ((cc & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += need + 1;
    return cp;
}

Utf16Text toUtf16(std::string_view utf8) {
    Utf16Text out;
    out.units.reserve(utf8.size());
    out.utf8Of.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const std::size_t byteStart = i;
        const uint32_t cp = nextUtf8(utf8, i);
        if (cp >= 0x10000) {
            const uint32_t v = cp - 0x10000;
            out.units.push_back(static_cast<uint16_t>(0xD800 + (v >> 10)));
            out.utf8Of.push_back(byteStart);
            out.units.push_back(static_cast<uint16_t>(0xDC00 + (v & 0x3FF)));
            out.utf8Of.push_back(byteStart);
        } else {
            out.units.push_back(static_cast<uint16_t>(cp));
            out.utf8Of.push_back(byteStart);
        }
    }
    return out;
}

// Characters rule L1 resets to the paragraph level when they trail a line:
// segment/paragraph separators, whitespace, and the isolate formatting
// characters. (L1 also names the characters *between* an isolate initiator and
// its PDI, which the caller cannot express in a per-byte flag; a trailing run
// of whitespace is what L1 is actually used for at a line end and is what this
// covers.)
bool isL1Resettable(uint32_t cp) {
    switch (cp) {
        case 0x0009: case 0x000A: case 0x000B: case 0x000C: case 0x000D:
        case 0x001C: case 0x001D: case 0x001E: case 0x001F:
        case 0x0020: case 0x0085: case 0x1680:
        case 0x2028: case 0x2029: case 0x205F: case 0x3000:
        case 0x2066: case 0x2067: case 0x2068: case 0x2069:  // LRI RLI FSI PDI
            return true;
        default:
            return (cp >= 0x2000 && cp <= 0x200A);
    }
}

} // namespace

bool isFormattingChar(uint32_t cp) {
    switch (cp) {
        case 0x200E:  // LRM
        case 0x200F:  // RLM
        case 0x061C:  // ALM
        case 0x202A:  // LRE
        case 0x202B:  // RLE
        case 0x202C:  // PDF
        case 0x202D:  // LRO
        case 0x202E:  // RLO
        case 0x2066:  // LRI
        case 0x2067:  // RLI
        case 0x2068:  // FSI
        case 0x2069:  // PDI
            return true;
        default:
            return false;
    }
}

bool isTriviallyLtr(std::string_view utf8) {
    // A conservative scan over raw bytes: everything below U+0590 is either
    // strong LTR, a number, or a neutral, and neutrals alone can never produce
    // a non-zero level under an LTR base. In UTF-8 that is exactly the bytes
    // < 0xD6, plus 0xD6 with a continuation below 0x90.
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0xD6) continue;
        if (c == 0xD6 && i + 1 < utf8.size() &&
            static_cast<unsigned char>(utf8[i + 1]) < 0x90) {
            continue;
        }
        return false;
    }
    return true;
}

std::string_view stripFormatting(std::string_view utf8, std::string& scratch) {
    // Fast path: the formatting characters are all 3-byte sequences beginning
    // 0xE2 0x80 / 0xE2 0x81, or the 2-byte U+061C (0xD8 0x9C). Nothing else can
    // match, so a byte scan decides "nothing to do" without decoding.
    bool any = false;
    for (std::size_t i = 0; i + 1 < utf8.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(utf8[i]);
        const unsigned char b = static_cast<unsigned char>(utf8[i + 1]);
        if ((a == 0xE2 && (b == 0x80 || b == 0x81)) || (a == 0xD8 && b == 0x9C)) {
            any = true;
            break;
        }
    }
    if (!any) return utf8;

    scratch.clear();
    scratch.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const std::size_t start = i;
        const uint32_t cp = nextUtf8(utf8, i);
        if (isFormattingChar(cp)) continue;
        scratch.append(utf8.data() + start, i - start);
    }
    return scratch;
}

std::vector<LevelRun> Paragraph::runs() const {
    std::vector<LevelRun> out;
    if (levels.empty()) return out;
    std::size_t start = 0;
    for (std::size_t i = 1; i <= levels.size(); ++i) {
        if (i == levels.size() || levels[i] != levels[start]) {
            out.push_back(LevelRun{start, i, levels[start]});
            start = i;
        }
    }
    return out;
}

#if BRO_WITH_ICU_BIDI

bool available() { return true; }

Paragraph resolveParagraph(std::string_view utf8, BaseDirection base, Override ov) {
    Paragraph out;
    out.paragraphLevel = (base == BaseDirection::RTL) ? 1 : 0;
    if (utf8.empty()) return out;

    // Nothing here can resolve to anything but level 0 under an LTR base, and
    // ICU would agree at the cost of two allocations and a UTF-16 conversion.
    // Text is overwhelmingly this case, so it is worth naming.
    if (base != BaseDirection::RTL && ov == Override::Normal && isTriviallyLtr(utf8)) {
        out.levels.assign(utf8.size(), 0);
        out.resettableToParagraph.assign(utf8.size(), false);
        std::size_t i = 0;
        while (i < utf8.size()) {
            const std::size_t s = i;
            const uint32_t cp = nextUtf8(utf8, i);
            if (isL1Resettable(cp)) {
                for (std::size_t k = s; k < i; ++k) out.resettableToParagraph[k] = true;
            }
        }
        return out;
    }

    const Utf16Text u16 = toUtf16(utf8);
    if (u16.units.empty()) return out;

    UErrorCode status = U_ZERO_ERROR;
    UBiDi* bidi = ubidi_openSized(static_cast<int32_t>(u16.units.size()), 0, &status);
    if (U_FAILURE(status) || !bidi) {
        if (bidi) ubidi_close(bidi);
        out.levels.assign(utf8.size(), out.paragraphLevel);
        out.resettableToParagraph.assign(utf8.size(), false);
        return out;
    }

    UBiDiLevel paraLevel;
    switch (base) {
        case BaseDirection::RTL:  paraLevel = 1; break;
        case BaseDirection::Auto: paraLevel = UBIDI_DEFAULT_LTR; break;
        default:                  paraLevel = 0; break;
    }

    // `unicode-bidi: bidi-override` is rule X6: force every character to the
    // base level whatever its own class says. ICU expresses that as a
    // per-character embedding-levels array with the UBIDI_LEVEL_OVERRIDE bit
    // set — which is exactly X6 and not an approximation of it.
    std::vector<UBiDiLevel> embedding;
    if (ov == Override::Override) {
        // An overriding paragraph must know its own level up front; Auto's
        // answer comes from P2/P3, so run that first on the unoverridden text.
        UBiDiLevel effective = paraLevel;
        if (paraLevel == UBIDI_DEFAULT_LTR) {
            UErrorCode probeStatus = U_ZERO_ERROR;
            ubidi_setPara(bidi, reinterpret_cast<const UChar*>(u16.units.data()),
                          static_cast<int32_t>(u16.units.size()),
                          UBIDI_DEFAULT_LTR, nullptr, &probeStatus);
            effective = U_SUCCESS(probeStatus) ? ubidi_getParaLevel(bidi) : 0;
        }
        embedding.assign(u16.units.size(),
                         static_cast<UBiDiLevel>(effective | UBIDI_LEVEL_OVERRIDE));
        paraLevel = effective;
    }

    ubidi_setPara(bidi, reinterpret_cast<const UChar*>(u16.units.data()),
                  static_cast<int32_t>(u16.units.size()), paraLevel,
                  embedding.empty() ? nullptr : embedding.data(), &status);
    if (U_FAILURE(status)) {
        ubidi_close(bidi);
        out.levels.assign(utf8.size(), out.paragraphLevel);
        out.resettableToParagraph.assign(utf8.size(), false);
        return out;
    }

    out.paragraphLevel = static_cast<Level>(ubidi_getParaLevel(bidi));

    const UBiDiLevel* levels16 = ubidi_getLevels(bidi, &status);
    if (U_FAILURE(status) || !levels16) {
        ubidi_close(bidi);
        out.levels.assign(utf8.size(), out.paragraphLevel);
        out.resettableToParagraph.assign(utf8.size(), false);
        return out;
    }

    // Project UTF-16 levels back onto UTF-8 bytes. Each UTF-16 unit names the
    // byte its character starts at; filling forward from there gives every byte
    // of a multi-byte character its character's level.
    out.levels.assign(utf8.size(), out.paragraphLevel);
    out.resettableToParagraph.assign(utf8.size(), false);
    for (std::size_t k = 0; k < u16.units.size(); ++k) {
        const std::size_t byteStart = u16.utf8Of[k];
        std::size_t byteEnd = utf8.size();
        for (std::size_t j = k + 1; j < u16.units.size(); ++j) {
            if (u16.utf8Of[j] != byteStart) { byteEnd = u16.utf8Of[j]; break; }
        }
        for (std::size_t b = byteStart; b < byteEnd; ++b) {
            out.levels[b] = static_cast<Level>(levels16[k]);
        }
    }

    std::size_t i = 0;
    while (i < utf8.size()) {
        const std::size_t s = i;
        const uint32_t cp = nextUtf8(utf8, i);
        if (isL1Resettable(cp)) {
            for (std::size_t k = s; k < i; ++k) out.resettableToParagraph[k] = true;
        }
    }

    ubidi_close(bidi);

    out.uniform = true;
    for (Level l : out.levels) {
        if (l != out.levels[0]) { out.uniform = false; break; }
    }
    return out;
}

std::vector<int32_t> reorderVisual(const std::vector<Level>& runLevels) {
    std::vector<int32_t> out(runLevels.size());
    if (runLevels.empty()) return out;
    ubidi_reorderVisual(runLevels.data(), static_cast<int32_t>(runLevels.size()),
                        out.data());
    return out;
}

#else  // !BRO_WITH_ICU_BIDI

bool available() { return false; }

Paragraph resolveParagraph(std::string_view utf8, BaseDirection base, Override) {
    Paragraph out;
    out.paragraphLevel = (base == BaseDirection::RTL) ? 1 : 0;
    out.levels.assign(utf8.size(), out.paragraphLevel);
    out.resettableToParagraph.assign(utf8.size(), false);
    return out;
}

std::vector<int32_t> reorderVisual(const std::vector<Level>& runLevels) {
    // No bidi: logical order is visual order.
    std::vector<int32_t> out(runLevels.size());
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<int32_t>(i);
    return out;
}

#endif  // BRO_WITH_ICU_BIDI

} // namespace bro::render::bidi
