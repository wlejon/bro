#pragma once

// -----------------------------------------------------------------------------
// ShapedRun — the result of shaping one string with one font descriptor.
//
// Everything about glyphs lives behind this type. Glyph ids, glyph indices and
// HarfBuzz cluster values are implementation detail of bro::render and MUST NOT
// escape it: the layout, DOM and JS layers speak UTF-8 byte offsets (JS speaks
// UTF-16, converted at the binding boundary). The three query functions below
// are the whole public contract for turning bytes into geometry and back.
//
// Why that matters: with shaping, "the width of the first N bytes" is no longer
// the sum of the first N per-character advances — a ligature is one glyph for
// several bytes, kerning depends on the neighbour, and an Arabic letter's
// advance depends on whether it joins. Prefix measurement is therefore wrong by
// construction, and the cluster map is the only correct source of caret
// geometry.
//
// byteOffsetToX returns a *pair* of positions even though only one is ever
// filled today. Under bidi (chunk 4) a single logical offset at a direction
// boundary has two visual x positions — the trailing edge of the LTR run and
// the leading edge of the RTL run. Baking that into the signature now means
// chunk 4 changes the bodies of these functions, not every caller of them.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <include/core/SkFont.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkFontStyle.h>
#include <include/core/SkPoint.h>
#include <include/core/SkRect.h>
#include <include/core/SkRefCnt.h>
#include <include/core/SkTextBlob.h>

#include "render/bidi.h"

class SkShaper;
class SkUnicode;

namespace bro::render {

class FontFallbackCache;

// TextDirection lives in bidi.h — it is the base direction a paragraph is
// resolved against, not a property of the shaper.

// CSS letter-spacing / word-spacing. Deliberately NOT part of the shaping
// cache key: spacing does not change which glyphs the shaper produces, only
// where they are placed. Shape once, then position with whatever spacing the
// caller has. (Applying letter-spacing before shaping would also be wrong —
// it must go between clusters, not between codepoints.)
struct Spacing {
    float letter = 0.0f;
    float word   = 0.0f;
    bool isZero() const { return letter == 0.0f && word == 0.0f; }
    bool operator==(const Spacing& o) const {
        return letter == o.letter && word == o.word;
    }
};

class ShapedRun {
public:
    // A caret position: an x offset from the run origin, plus which side of a
    // cluster it names. `isLeadingEdge` is the side the caret "belongs to" —
    // it is what disambiguates the two answers at a direction boundary and
    // what tells a renderer which way to draw a split caret.
    struct Caret {
        float x = 0.0f;
        bool  isLeadingEdge = true;
    };

    // One offset, up to two visual positions. `hasSecondary` is always false
    // until bidi reordering lands; callers must already handle it being true.
    struct CaretPositions {
        Caret primary;
        Caret secondary;
        bool  hasSecondary = false;
    };

    // The byte extent of one cluster — the atomic unit for caret movement and
    // selection. A caret may sit at byteStart or byteEnd but never inside.
    struct ClusterSpan {
        std::size_t byteStart = 0;
        std::size_t byteEnd   = 0;
    };

    ShapedRun() = default;

    bool        empty()      const { return clusters_.empty(); }
    std::size_t glyphCount() const { return glyphs_.size(); }
    std::string_view text()  const { return text_; }

    // The base direction this run was shaped against, and whether anything in
    // it resolved to a right-to-left level. `hasRtl` is false for the
    // overwhelming majority of runs and is what lets callers skip every
    // bidi-aware branch below.
    TextDirection baseDirection() const { return baseDirection_; }
    bool          hasRtl()        const { return hasRtl_; }

    // Total advance width. With spacing applied, letter-spacing lands between
    // clusters (n-1 gaps, never trailing) so the drawn extent matches the
    // layout box and centered text is not dragged leftward.
    float width(Spacing spacing = {}) const;

    // Union of glyph bounding boxes at natural positions (no spacing) —
    // the ink extent, used for TextMetrics::height.
    const SkRect& bounds() const { return bounds_; }

    // --- byte-domain queries: the only sanctioned way out of glyph space ---

    /// Visual x for a caret sitting at `byteOffset`. Offsets inside a cluster
    /// snap to the cluster's leading edge; an offset past the end lands on the
    /// trailing edge of the last cluster in LOGICAL order, which for a
    /// right-to-left run is its left side.
    ///
    /// Snapping to the cluster edge is where this differs from Chromium, which
    /// steps by GRAPHEME and divides a ligature's advance among the graphemes
    /// inside it, so a caret can sit between the f and the i of an "fi". The
    /// difference is reachable: measured over ordinary English, Arial, Cambria,
    /// Georgia and Times New Roman produce no multi-character clusters at all,
    /// but Calibri turns "office fluffy first" into 14 clusters where the others
    /// make 19. Arabic lam-alef is the other everyday case.
    ///
    /// Not closed here, and the reason is availability rather than cost.
    /// Grapheme boundaries are UAX #29 data, and this build has none: the
    /// bidi-only SkUnicode has no break iterator, and libgrapheme is
    /// deliberately not vendored because its tables are generated at build time
    /// by host tools run over the UCD. Stepping by codepoint instead is not an
    /// approximation of grapheme stepping but a regression past it — it puts a
    /// caret between a base character and its combining mark, which is exactly
    /// what cluster stepping was introduced to stop. Getting a segmenter is
    /// build plumbing of the same shape as vendoring HarfBuzz was, and belongs
    /// with that kind of work rather than inside a change to text ordering.
    CaretPositions byteOffsetToX(std::size_t byteOffset, Spacing spacing = {}) const;

    /// Byte offset nearest to visual position `x`, snapped to a cluster
    /// boundary (never lands mid-ligature).
    std::size_t xToByteOffset(float x, Spacing spacing = {}) const;

    /// The cluster containing `byteOffset`. For a byte in the middle of a
    /// ligature this returns the whole ligature's extent.
    ClusterSpan clusterRange(std::size_t byteOffset) const;

    // The whole cluster map, in visual order. Glyph COUNT is reported (how
    // many glyphs a cluster fused into is exactly what a ligature assertion
    // needs); glyph ids and indices still do not escape.
    struct ClusterInfo {
        std::size_t byteStart = 0;
        std::size_t byteEnd   = 0;
        float       x         = 0.0f;  // pen x with `spacing` applied
        float       advance   = 0.0f;
        std::size_t glyphCount = 0;
        bool        rtl       = false;
    };
    std::vector<ClusterInfo> clusterList(Spacing spacing = {}) const;

    // --- rendering ---

    /// Build an immutable, refcounted text blob positioned relative to the run
    /// origin. Safe to hand to another thread (this is how RecordingRenderer
    /// gets shaping off the raster thread entirely). Returns null when empty.
    sk_sp<SkTextBlob> makeBlob(Spacing spacing = {}) const;

    // Write access for the shaping code only. Everything a shaping backend
    // needs to fill in, and nothing a consumer can reach — glyph arrays stay
    // private even inside bro::render.
    class Builder {
    public:
        explicit Builder(ShapedRun& r) : r_(r) {}
        void setText(std::string_view t) { r_.text_.assign(t); }
        void setNaturalWidth(float w) { r_.naturalWidth_ = w; }
        void grow(std::size_t n) {
            const std::size_t sz = r_.glyphs_.size() + n;
            r_.glyphs_.resize(sz);
            r_.positions_.resize(sz);
            r_.offsets_.resize(sz);
            r_.advances_.resize(sz);
            r_.glyphClusters_.resize(sz);
        }
        void addRun(const SkFont& f, std::size_t first, std::size_t count, uint8_t level) {
            r_.runs_.push_back(GlyphRun{f, first, count, level});
        }
        std::vector<SkGlyphID>& glyphs()    { return r_.glyphs_; }
        std::vector<SkPoint>&   positions() { return r_.positions_; }
        std::vector<SkPoint>&   offsets()   { return r_.offsets_; }
        std::vector<float>&     advances()  { return r_.advances_; }
        std::vector<uint32_t>&  clusters()  { return r_.glyphClusters_; }
        void setBaseDirection(TextDirection d) { r_.baseDirection_ = d; }
        // Rule L2, then derive the cluster table. Reordering must happen
        // FIRST: finalize() groups clusters by walking the glyph arrays in
        // storage order and calling that visual order, which is only true once
        // the runs have been permuted into it.
        void finish() { r_.reorderRunsVisually(); r_.finalize(); }

    private:
        ShapedRun& r_;
    };

private:
    friend class Builder;
    friend class TextShapingEngine;

    // A maximal span of glyphs sharing one typeface and one bidi level —
    // the unit SkTextBlobBuilder wants.
    struct GlyphRun {
        SkFont      font;
        std::size_t first = 0;
        std::size_t count = 0;
        uint8_t     bidiLevel = 0;
    };

    // A shaper cluster: one or more glyphs produced by one or more source
    // bytes. Stored in visual order.
    struct Cluster {
        uint32_t byteStart  = 0;
        uint32_t byteEnd    = 0;
        uint32_t glyphFirst = 0;
        uint32_t glyphCount = 0;
        float    advance    = 0.0f;  // natural, unspaced
        bool     isWordSep  = false; // exactly one U+0020 — CSS word-spacing
        bool     rtl        = false;
    };

    // Walk clusters in visual order, handing each its pen x with `spacing`
    // applied. `fn(clusterIndex, penX)`.
    template <typename Fn>
    void forEachClusterPen(Spacing spacing, Fn&& fn) const;

    // Rule L2 over this run's glyph runs. The shaper emits runs in LOGICAL
    // order (SkShapers::HB::ShapeDontWrapOrReorder is named for exactly this)
    // and lays them out left to right, so an RTL sequence spanning more than
    // one run — two scripts, two fonts, a number inside Arabic — comes out
    // backwards until this permutes them and re-runs the pen.
    //
    // Runs here are finer than level runs (they also split on font and script),
    // which is harmless: L2 only reverses contiguous spans of equal-or-higher
    // level, so splitting a level run into adjacent equal-level entries gives
    // the same result as reordering the unsplit run.
    void reorderRunsVisually();

    // Finish construction: derive cluster table + bounds from the flat glyph
    // arrays the shaping callback filled in.
    void finalize();

    std::string            text_;
    std::vector<SkGlyphID> glyphs_;
    std::vector<SkPoint>   positions_;  // per-glyph pen position (no spacing)
    std::vector<SkPoint>   offsets_;    // per-glyph draw offset from the pen
    std::vector<float>     advances_;   // per-glyph x advance
    std::vector<uint32_t>  glyphClusters_;  // per-glyph source byte offset
    std::vector<GlyphRun>  runs_;
    std::vector<Cluster>   clusters_;
    SkRect                 bounds_ = SkRect::MakeEmpty();
    float                  naturalWidth_ = 0.0f;
    TextDirection          baseDirection_ = TextDirection::LTR;
    bool                   hasRtl_ = false;
};

// -----------------------------------------------------------------------------
// TextShapingEngine — owns the shaper and a thread-affine shaped-run cache.
//
// Thread-affine by ownership, exactly like the existing fonts_/fallbackCache_:
// each renderer holds its own engine and never shares it, so the data plane
// stays lock-free by not being shared rather than by being synchronized.
//
// Caching here is frame-budget correctness, not optimization. Text measurement
// was already the dominant cost of a layout pass before shaping existed (see
// layout/skia_text_metrics.h) and a HarfBuzz call is an order of magnitude
// more expensive than the 1:1 codepoint mapping it replaces.
// -----------------------------------------------------------------------------
class TextShapingEngine {
public:
    TextShapingEngine();
    ~TextShapingEngine();

    /// Shape (or fetch from cache) `utf8` with `primary`. `family` and `style`
    /// describe what was *asked for*, which the shaper needs to drive font
    /// fallback for codepoints the primary face lacks.
    ///
    /// The returned pointer is owned by the cache and is invalidated by the
    /// next call that misses (the cache clears rather than evicts). Use it and
    /// drop it; never store it across another shape().
    /// Returns nullptr only for empty input.
    ///
    /// `disableLigatures` turns off the `liga`/`clig` features. Callers set it
    /// when letter-spacing is in play: CSS letter-spacing inserts space
    /// *between characters*, which a ligature has fused into one indivisible
    /// glyph, so browsers suppress ligatures whenever letter-spacing is
    /// non-zero. It is a shaping input (it changes which glyphs come out) and
    /// so it belongs in the cache key — the spacing *amount* does not, and is
    /// applied to the positioned output instead.
    const ShapedRun* shape(std::string_view utf8,
                           const SkFont& primary,
                           std::string_view family,
                           SkFontStyle style,
                           SkFontMgr* fontMgr,
                           FontFallbackCache& fallback,
                           TextDirection direction = TextDirection::LTR,
                           bool disableLigatures = false);

    /// Drop everything. Renderers call this when a custom font is registered,
    /// since the same descriptor can resolve to a different face across that.
    void clear();

    // Instrumentation — cheap counters, read by benchmarks.
    uint64_t hits()   const { return hits_; }
    uint64_t misses() const { return misses_; }

private:
    struct Key {
        std::string   text;
        std::string   family;
        float         size;
        int           weight;
        bool          italic;
        TextDirection direction;
        bool          noLigatures;   // the only feature toggle callers have
        // Script is derived from the text itself and language is the process
        // locale, so neither adds information to this key today. Note what is
        // NOT here: letter-spacing and word-spacing. Spacing does not change
        // which glyphs the shaper produces, so it must not multiply the cache.
        bool operator==(const Key& o) const {
            return size == o.size && weight == o.weight && italic == o.italic &&
                   direction == o.direction && noLigatures == o.noLigatures &&
                   text == o.text && family == o.family;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept;
    };

    // Same clear-don't-evict policy as the layout metrics cache: a document
    // that measures unboundedly many distinct strings (a log, a text field
    // being typed into) must not grow this forever, and LRU bookkeeping on
    // every hit would cost more than the occasional refill.
    static constexpr std::size_t kMaxEntries = 1 << 15;

    // 1:1 codepoint→glyph with coverage-based fallback — the pre-shaping
    // behaviour, kept as the BRO_WITH_TEXT_SHAPING=OFF path so the renderers
    // have exactly one text code path either way.
    static void buildUnshaped(ShapedRun& run, ShapedRun::Builder& b,
                              std::string_view utf8, const SkFont& primary,
                              SkFontStyle style, SkFontMgr* fontMgr,
                              FontFallbackCache& fallback);

    std::unordered_map<Key, std::unique_ptr<ShapedRun>, KeyHash> cache_;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;

#if BRO_WITH_TEXT_SHAPING
    sk_sp<SkUnicode>          unicode_;
    std::unique_ptr<SkShaper> shaper_;
    bool                      shaperTried_ = false;
    bool ensureShaper();
#endif
};

} // namespace bro::render
