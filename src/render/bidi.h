#pragma once

// -----------------------------------------------------------------------------
// bidi.h — the Unicode Bidirectional Algorithm (UAX #9), as much of it as
// displaying text requires.
//
// Two things live here and they answer different questions:
//
//   resolveParagraph()  "given this text and a base direction, what embedding
//                        level does every byte end up at?"  (rules P2-P3, X1-X8,
//                        W1-W7, N0-N2, I1-I2)
//   reorderVisual()     "given the levels of a line's runs in logical order,
//                        what order do they appear on screen?"  (rule L2)
//
// The split matters because they happen in different places. Levels are
// resolved over a whole paragraph, once; reordering happens per LINE, after
// line breaking has decided which runs share a line. Resolving levels per line
// would get a different (wrong) answer, because W and N rules see across a
// break that the paragraph does not have.
//
// Rule L1 (whitespace at a line's end reverts to the paragraph level) is the
// caller's, for the same reason: only the caller knows where the line ends.
// resolveParagraph() reports which bytes are L1-resettable so the caller can
// apply it without re-deriving character classes.
//
// This is backed by ICU's UAX#9 subset — the same 14 source files Skia vendors
// for SkUnicode, not the 30 MB ICU blob. Nothing about that leaks through this
// header: no ICU type appears below, so exactly one translation unit in bro
// includes an ICU header.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bro::render {

enum class TextDirection : uint8_t { LTR, RTL };

namespace bidi {

using Level = uint8_t;

// The paragraph embedding level asked for. `Auto` is UAX #9 P2/P3 — scan for
// the first strong character and take its direction — which is what HTML
// `dir="auto"` and CSS `unicode-bidi: plaintext` mean. Auto is resolved to a
// concrete level by resolveParagraph(); nothing downstream ever sees it.
enum class BaseDirection : uint8_t { LTR, RTL, Auto };

// How `unicode-bidi` modifies level resolution for one element's text.
//   Normal   — the algorithm runs unmodified.
//   Override — rule X6: every character takes the base level regardless of its
//              own class, so the text is displayed in strictly one direction.
// `isolate` and `plaintext` are deliberately absent: they change what a
// paragraph *is*, not just how levels resolve, and are out of scope here.
enum class Override : uint8_t { Normal, Override };

// A maximal span of UTF-8 bytes sharing one resolved embedding level. Byte
// ranges, not character indices — every layer above bro::render speaks UTF-8
// bytes and converting at this boundary is what keeps it that way.
struct LevelRun {
    std::size_t start = 0;
    std::size_t end   = 0;
    Level       level = 0;

    bool isRtl() const { return (level & 1) != 0; }
};

struct Paragraph {
    // The resolved paragraph level: 0 or 1. With BaseDirection::Auto this is
    // P2/P3's answer, which is the only way to learn it.
    Level                paragraphLevel = 0;
    // One entry per UTF-8 byte. Every byte of a multi-byte sequence carries its
    // character's level, so `levels[byteOffset]` is always meaningful and no
    // caller has to decode UTF-8 to use it.
    std::vector<Level>   levels;
    // Bytes belonging to characters rule L1 resets to the paragraph level when
    // they trail a line: whitespace and isolate formatting characters. The
    // caller applies L1 because only it knows where lines end.
    std::vector<bool>    resettableToParagraph;
    // True when everything resolved to one level — the overwhelmingly common
    // case, and the one where callers can skip reordering entirely.
    bool                 uniform = true;

    bool isRtlParagraph() const { return (paragraphLevel & 1) != 0; }

    // Maximal same-level spans in logical order. Empty for empty text.
    std::vector<LevelRun> runs() const;
};

/// True when the ICU bidi subset was compiled in. With it OFF every function
/// here still works and reports a single LTR run, which is what a build without
/// text shaping had before bidi existed.
bool available();

/// Resolve `utf8` against `base`. Never fails: with bidi unavailable, or on
/// empty input, the result is one uniform run at the base level (Auto → LTR).
Paragraph resolveParagraph(std::string_view utf8,
                           BaseDirection base,
                           Override ov = Override::Normal);

/// Rule L2. `runLevels` are a line's runs in LOGICAL order; the result maps
/// visual slot → logical index, so drawing `runs[out[0]], runs[out[1]], ...`
/// left to right is the correct visual order.
///
/// Runs may be finer than level runs (split by font, by element, by style) —
/// L2 only ever reverses contiguous same-or-higher-level spans, so splitting a
/// level run into several adjacent entries of equal level and reordering those
/// gives the same answer as reordering the unsplit run.
std::vector<int32_t> reorderVisual(const std::vector<Level>& runLevels);

/// Convenience: is this codepoint one of the bidi formatting characters
/// (LRM/RLM/ALM, LRE/RLE/LRO/RLO/PDF, LRI/RLI/FSI/PDI)? They participate in
/// level resolution and then must not occupy any width.
bool isFormattingChar(uint32_t cp);

/// True when `utf8` contains no character that could possibly resolve to a
/// non-zero level — no RTL character and no bidi formatting character. This is
/// a cheap, conservative scan whose whole purpose is to let the 99% of text
/// that is plain Latin skip bidi resolution entirely.
bool isTriviallyLtr(std::string_view utf8);

/// Strip the bidi formatting characters from `utf8`. Level resolution must see
/// them; shaping and measurement must not, because a font may map an unknown
/// control to a visible .notdef box. Returns a reference to `utf8` itself when
/// there was nothing to strip (the overwhelmingly common case), else fills and
/// returns `scratch`.
std::string_view stripFormatting(std::string_view utf8, std::string& scratch);

} // namespace bidi
} // namespace bro::render
