#include "render/shaped_run.h"

#include "render/font_fallback.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include <include/core/SkTextBlob.h>

#if BRO_WITH_TEXT_SHAPING
#include <modules/skshaper/include/SkShaper.h>
#include <modules/skshaper/include/SkShaper_harfbuzz.h>
#include <modules/skshaper/include/SkShaper_skunicode.h>
#include <modules/skunicode/include/SkUnicode.h>
#include <modules/skunicode/include/SkUnicode_bidi.h>
#endif

namespace bro::render {

// ===========================================================================
// ShapedRun
// ===========================================================================

void ShapedRun::reorderRunsVisually() {
    hasRtl_ = false;
    for (const auto& r : runs_) {
        if (r.bidiLevel & 1) { hasRtl_ = true; break; }
    }
    // With no odd level anywhere, L2 has nothing to reverse — it reverses from
    // the highest level down to the lowest ODD one — so the answer is the
    // identity and there is no reason to ask for it.
    if (!hasRtl_ || runs_.size() < 2) return;

    std::vector<bidi::Level> levels;
    levels.reserve(runs_.size());
    for (const auto& r : runs_) levels.push_back(r.bidiLevel);

    const std::vector<int32_t> order = bidi::reorderVisual(levels);
    if (order.size() != runs_.size()) return;
    bool identity = true;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] != static_cast<int32_t>(i)) { identity = false; break; }
    }
    if (identity) return;

    // Rebuild the flat arrays in visual run order, re-running the pen so each
    // run starts where the previous one ended. A run's glyph positions are
    // relative to its own origin (its first glyph sits at the run's pen), so
    // rebasing is a single subtract-then-add per glyph.
    const std::size_t n = glyphs_.size();
    std::vector<SkGlyphID> g;   g.reserve(n);
    std::vector<SkPoint>   p;   p.reserve(n);
    std::vector<SkPoint>   o;   o.reserve(n);
    std::vector<float>     a;   a.reserve(n);
    std::vector<uint32_t>  c;   c.reserve(n);
    std::vector<GlyphRun>  nr;  nr.reserve(runs_.size());

    float pen = 0.0f;
    for (const int32_t logical : order) {
        if (logical < 0 || static_cast<std::size_t>(logical) >= runs_.size()) continue;
        const GlyphRun& r = runs_[static_cast<std::size_t>(logical)];
        GlyphRun placed = r;
        placed.first = g.size();
        const float origin = r.count ? positions_[r.first].fX : 0.0f;
        float advance = 0.0f;
        for (std::size_t k = 0; k < r.count; ++k) {
            const std::size_t s = r.first + k;
            g.push_back(glyphs_[s]);
            p.push_back(SkPoint{positions_[s].fX - origin + pen, positions_[s].fY});
            o.push_back(offsets_[s]);
            a.push_back(advances_[s]);
            c.push_back(glyphClusters_[s]);
            advance += advances_[s];
        }
        nr.push_back(placed);
        pen += advance;
    }

    glyphs_        = std::move(g);
    positions_     = std::move(p);
    offsets_       = std::move(o);
    advances_      = std::move(a);
    glyphClusters_ = std::move(c);
    runs_          = std::move(nr);
}

void ShapedRun::finalize() {
    clusters_.clear();
    bounds_ = SkRect::MakeEmpty();
    if (glyphs_.empty()) return;

    // Group consecutive glyphs sharing a cluster value. Glyphs arrive in
    // VISUAL order (for an RTL run the cluster values descend), so this walk
    // produces clusters in visual order too.
    for (std::size_t i = 0; i < glyphs_.size();) {
        std::size_t j = i;
        const uint32_t cv = glyphClusters_[i];
        float adv = 0.0f;
        while (j < glyphs_.size() && glyphClusters_[j] == cv) {
            adv += advances_[j];
            ++j;
        }
        Cluster c;
        c.byteStart  = cv;
        c.byteEnd    = cv;  // patched below
        c.glyphFirst = static_cast<uint32_t>(i);
        c.glyphCount = static_cast<uint32_t>(j - i);
        c.advance    = adv;
        // The bidi level of the run this cluster's glyphs came from.
        for (const auto& r : runs_) {
            if (i >= r.first && i < r.first + r.count) { c.rtl = (r.bidiLevel & 1) != 0; break; }
        }
        clusters_.push_back(c);
        i = j;
    }

    // A cluster's byte extent runs to the next cluster start in LOGICAL order,
    // which is not the order they are stored in once anything is RTL. Sort a
    // copy of the starts and look each one up.
    std::vector<uint32_t> starts;
    starts.reserve(clusters_.size());
    for (const auto& c : clusters_) starts.push_back(c.byteStart);
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    const uint32_t textEnd = static_cast<uint32_t>(text_.size());
    for (auto& c : clusters_) {
        auto it = std::upper_bound(starts.begin(), starts.end(), c.byteStart);
        c.byteEnd = (it == starts.end()) ? textEnd : *it;
        c.isWordSep = (c.byteEnd - c.byteStart == 1) && text_[c.byteStart] == ' ';
    }

    // Ink bounds: union of glyph boxes at their natural draw positions.
    std::vector<SkRect> rects;
    for (const auto& r : runs_) {
        if (r.count == 0) continue;
        rects.resize(r.count);
        r.font.getBounds(SkSpan<const SkGlyphID>(glyphs_.data() + r.first, r.count),
                         SkSpan<SkRect>(rects.data(), r.count), nullptr);
        for (std::size_t k = 0; k < r.count; ++k) {
            const SkPoint p = positions_[r.first + k] + offsets_[r.first + k];
            SkRect b = rects[k].makeOffset(p.fX, p.fY);
            bounds_.join(b);
        }
    }
}

// Visit clusters in visual order with the pen x that `spacing` puts them at.
// Letter-spacing goes between clusters (never after the last, so the drawn
// extent matches the layout box and centered text is not dragged left);
// word-spacing widens a space cluster's own advance, matching CSS.
template <typename Fn>
void ShapedRun::forEachClusterPen(Spacing spacing, Fn&& fn) const {
    float extra = 0.0f;
    const std::size_t n = clusters_.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Cluster& c = clusters_[i];
        const float penX = positions_[c.glyphFirst].fX + extra;
        fn(i, penX, extra);
        if (c.isWordSep) extra += spacing.word;
        if (i + 1 != n)  extra += spacing.letter;
    }
}

float ShapedRun::width(Spacing spacing) const {
    if (clusters_.empty()) return 0.0f;
    float extra = 0.0f;
    for (std::size_t i = 0; i < clusters_.size(); ++i) {
        if (clusters_[i].isWordSep) extra += spacing.word;
        if (i + 1 != clusters_.size()) extra += spacing.letter;
    }
    return naturalWidth_ + extra;
}

ShapedRun::CaretPositions ShapedRun::byteOffsetToX(std::size_t byteOffset,
                                                   Spacing spacing) const {
    CaretPositions out;
    if (clusters_.empty()) return out;

    // Pen x of cluster `i`, and its spaced advance.
    auto geometry = [&](std::size_t i, float& penX, float& advance) {
        penX = 0.0f;
        forEachClusterPen(spacing, [&](std::size_t idx, float x, float) {
            if (idx == i) penX = x;
        });
        advance = clusters_[i].advance +
                  (clusters_[i].isWordSep ? spacing.word : 0.0f);
    };

    // Edge helpers. A cluster's LEADING edge is the side the reader arrives
    // from — its left for LTR, its right for RTL — and the trailing edge is
    // the other one.
    auto leadingEdge = [&](std::size_t i) {
        float penX = 0.0f, advance = 0.0f;
        geometry(i, penX, advance);
        return clusters_[i].rtl ? penX + advance : penX;
    };
    auto trailingEdge = [&](std::size_t i) {
        float penX = 0.0f, advance = 0.0f;
        geometry(i, penX, advance);
        return clusters_[i].rtl ? penX : penX + advance;
    };

    // A caret at `byteOffset` is named by up to two clusters: the one that
    // ENDS there (caret on its trailing edge) and the one that STARTS there
    // (caret on its leading edge). In unidirectional text both land on the
    // same x and there is nothing to choose between them. At a direction
    // boundary they are different places on the line and BOTH are real — that
    // is the whole reason this returns a pair. Looking only for the cluster
    // that *contains* the offset, as this used to, can only ever produce
    // leading edges, which leaves the trailing edge of an RTL run reachable
    // from no offset at all and collapses any range that ends there.
    bool haveBefore = false, haveAfter = false;
    std::size_t before = 0, after = 0;
    for (std::size_t i = 0; i < clusters_.size(); ++i) {
        if (clusters_[i].byteEnd == byteOffset)   { before = i; haveBefore = true; }
        if (clusters_[i].byteStart == byteOffset) { after  = i; haveAfter  = true; }
    }

    if (!haveBefore && !haveAfter) {
        // Strictly inside a cluster (a ligature, a combining sequence): snap to
        // its leading edge — there is no glyph geometry inside a ligature for a
        // caret to point at. Past the end of the text falls through to the
        // run's trailing edge.
        for (std::size_t i = 0; i < clusters_.size(); ++i) {
            const Cluster& c = clusters_[i];
            if (byteOffset <= c.byteStart || byteOffset >= c.byteEnd) continue;
            out.primary = {leadingEdge(i), true};
            return out;
        }
        out.primary = {width(spacing), false};
        return out;
    }

    Caret fromAfter{haveAfter ? leadingEdge(after) : 0.0f, true};
    Caret fromBefore{haveBefore ? trailingEdge(before) : 0.0f, false};

    if (!haveBefore) { out.primary = fromAfter;  return out; }
    if (!haveAfter)  { out.primary = fromBefore; return out; }

    // Both exist. Same x means unidirectional text and one answer.
    if (fromAfter.x == fromBefore.x) { out.primary = fromAfter; return out; }

    // A genuine direction boundary. `primary` stays the leading edge of the
    // run starting here — selection geometry derives its rect edges from
    // primary, and a boundary offset there means "the extent of the text that
    // follows", not "where a caret with upstream affinity blinks". The newly
    // reachable answer is the trailing edge of the run that ends here, which
    // is what had no offset at all before; it goes in secondary, where a
    // caret-drawing caller can pick it up via affinity.
    out.primary      = fromAfter;
    out.secondary    = fromBefore;
    out.hasSecondary = true;
    return out;
}

std::size_t ShapedRun::xToByteOffset(float x, Spacing spacing) const {
    if (clusters_.empty()) return 0;
    std::size_t best = 0;
    float bestDist = std::numeric_limits<float>::max();
    auto consider = [&](float edgeX, std::size_t byteOff) {
        const float d = std::fabs(x - edgeX);
        if (d < bestDist) { bestDist = d; best = byteOff; }
    };
    forEachClusterPen(spacing, [&](std::size_t idx, float penX, float) {
        const Cluster& c = clusters_[idx];
        const float advance = c.advance + (c.isWordSep ? spacing.word : 0.0f);
        // Both edges of every cluster are candidate caret sites; snapping to
        // the nearest one is what keeps a caret out of the middle of a
        // ligature.
        consider(penX, c.rtl ? c.byteEnd : c.byteStart);
        consider(penX + advance, c.rtl ? c.byteStart : c.byteEnd);
    });
    return best;
}

ShapedRun::ClusterSpan ShapedRun::clusterRange(std::size_t byteOffset) const {
    if (byteOffset >= text_.size()) return {text_.size(), text_.size()};
    for (const auto& c : clusters_) {
        if (byteOffset >= c.byteStart && byteOffset < c.byteEnd) {
            return {c.byteStart, c.byteEnd};
        }
    }
    return {byteOffset, byteOffset};
}

std::vector<ShapedRun::ClusterInfo> ShapedRun::clusterList(Spacing spacing) const {
    std::vector<ClusterInfo> out;
    out.reserve(clusters_.size());
    forEachClusterPen(spacing, [&](std::size_t idx, float penX, float) {
        const Cluster& c = clusters_[idx];
        out.push_back(ClusterInfo{c.byteStart, c.byteEnd, penX,
                                  c.advance + (c.isWordSep ? spacing.word : 0.0f),
                                  c.glyphCount, c.rtl});
    });
    return out;
}

sk_sp<SkTextBlob> ShapedRun::makeBlob(Spacing spacing) const {
    if (glyphs_.empty()) return nullptr;

    // Per-glyph x shift induced by spacing: constant within a cluster.
    std::vector<float> shift(glyphs_.size(), 0.0f);
    if (!spacing.isZero()) {
        forEachClusterPen(spacing, [&](std::size_t idx, float, float extra) {
            const Cluster& c = clusters_[idx];
            for (uint32_t g = 0; g < c.glyphCount; ++g) shift[c.glyphFirst + g] = extra;
        });
    }

    SkTextBlobBuilder builder;
    for (const auto& r : runs_) {
        if (r.count == 0) continue;
        const auto& buf = builder.allocRunPos(r.font, static_cast<int>(r.count));
        std::memcpy(buf.glyphs, glyphs_.data() + r.first, r.count * sizeof(SkGlyphID));
        for (std::size_t k = 0; k < r.count; ++k) {
            const std::size_t g = r.first + k;
            buf.points()[k] = SkPoint{positions_[g].fX + shift[g] + offsets_[g].fX,
                                      positions_[g].fY + offsets_[g].fY};
        }
    }
    return builder.make();
}

// ===========================================================================
// Shaping
// ===========================================================================

namespace {

#if BRO_WITH_TEXT_SHAPING
// Collects SkShaper output straight into a ShapedRun's flat arrays. The
// alternative (SkTextBlobBuilderRunHandler) throws away the cluster map, which
// is the entire reason this layer exists.
class ShapedRunHandler final : public SkShaper::RunHandler {
public:
    explicit ShapedRunHandler(ShapedRun::Builder& out) : out_(out) {}

    void beginLine() override {}
    void runInfo(const RunInfo&) override {}
    void commitRunInfo() override {}

    Buffer runBuffer(const RunInfo& info) override {
        first_ = out_.glyphs().size();
        count_ = info.glyphCount;
        out_.grow(count_);
        out_.addRun(info.fFont, first_, count_, info.fBidiLevel);
        return Buffer{out_.glyphs().data() + first_,
                      out_.positions().data() + first_,
                      out_.offsets().data() + first_,
                      out_.clusters().data() + first_,
                      pen_};
    }

    void commitRunBuffer(const RunInfo& info) override {
        // With `offsets` supplied, positions[] hold pen positions and the
        // difference between consecutive ones is the advance — see
        // SkShaper_harfbuzz.cpp's emit(), which branches on buffer.offsets.
        auto& pos = out_.positions();
        auto& adv = out_.advances();
        const float runEndX = pen_.fX + info.fAdvance.fX;
        for (std::size_t k = 0; k < count_; ++k) {
            const std::size_t g = first_ + k;
            const float next = (k + 1 < count_) ? pos[g + 1].fX : runEndX;
            adv[g] = next - pos[g].fX;
        }
        pen_ += info.fAdvance;
    }

    void commitLine() override {}

    SkPoint pen() const { return pen_; }

private:
    ShapedRun::Builder& out_;
    SkPoint     pen_{0, 0};
    std::size_t first_ = 0;
    std::size_t count_ = 0;
};
#endif  // BRO_WITH_TEXT_SHAPING

}  // namespace

// ===========================================================================
// TextShapingEngine
// ===========================================================================

std::size_t TextShapingEngine::KeyHash::operator()(const Key& k) const noexcept {
    std::size_t h = std::hash<std::string_view>{}(k.text);
    auto mix = [&h](std::size_t v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    mix(std::hash<std::string_view>{}(k.family));
    mix(std::hash<float>{}(k.size));
    mix(static_cast<std::size_t>(k.weight));
    mix(static_cast<std::size_t>(k.italic));
    mix(static_cast<std::size_t>(k.direction));
    mix(static_cast<std::size_t>(k.noLigatures));
    return h;
}

TextShapingEngine::TextShapingEngine() = default;
TextShapingEngine::~TextShapingEngine() = default;

void TextShapingEngine::clear() { cache_.clear(); }

#if BRO_WITH_TEXT_SHAPING
bool TextShapingEngine::ensureShaper() {
    if (shaperTried_) return shaper_ != nullptr;
    shaperTried_ = true;
    // Bidi-only SkUnicode: ShapeDontWrapOrReorder never asks for a break
    // iterator (it neither wraps nor reorders), so the hardcoded property
    // tables plus the ICU bidi subset are all it needs.
    unicode_ = SkUnicodes::Bidi::Make();
    if (!unicode_) {
        LOG_WARN("SkUnicode bidi factory unavailable — text falls back to unshaped glyphs");
        return false;
    }
    shaper_ = SkShapers::HB::ShapeDontWrapOrReorder(unicode_, nullptr);
    if (!shaper_) {
        LOG_WARN("HarfBuzz shaper unavailable — text falls back to unshaped glyphs");
        return false;
    }
    return true;
}
#endif

const ShapedRun* TextShapingEngine::shape(std::string_view utf8,
                                          const SkFont& primary,
                                          std::string_view family,
                                          SkFontStyle style,
                                          SkFontMgr* fontMgr,
                                          FontFallbackCache& fallback,
                                          TextDirection direction,
                                          bool disableLigatures) {
    if (utf8.empty()) return nullptr;

    std::string scratch;
    utf8 = ensureValidUtf8(utf8, scratch);

    Key key{std::string(utf8), std::string(family), primary.getSize(),
            style.weight(), style.slant() != SkFontStyle::kUpright_Slant,
            direction, disableLigatures};
    if (auto it = cache_.find(key); it != cache_.end()) {
        ++hits_;
        return it->second.get();
    }
    ++misses_;

    auto run = std::make_unique<ShapedRun>();
    ShapedRun::Builder b(*run);
    b.setText(utf8);
    b.setBaseDirection(direction);

    bool shaped = false;
#if BRO_WITH_TEXT_SHAPING
    if (ensureShaper()) {
        // MakeFontMgrRunIterator does the font fallback the shaper needs —
        // it splits on coverage the same way splitTextForFallback does, but
        // per shaping run rather than per codepoint span, and it hands each
        // sub-run's real typeface to HarfBuzz so kerning and ligatures come
        // from the face that actually draws them.
        std::string familyZ(family);
        auto language = SkShaper::MakeStdLanguageRunIterator(utf8.data(), utf8.size());
        auto fontRuns = SkShaper::MakeFontMgrRunIterator(
            utf8.data(), utf8.size(), primary, sk_ref_sp(fontMgr),
            familyZ.c_str(), style, language.get());
        auto scriptRuns = SkShapers::HB::ScriptRunIterator(utf8.data(), utf8.size());
        auto bidiRuns = SkShapers::unicode::BidiRunIterator(
            unicode_, utf8.data(), utf8.size(),
            direction == TextDirection::RTL ? 1 : 0);
        // CSS letter-spacing separates characters; a ligature has fused
        // several characters into one glyph with no seam to put space in.
        // Browsers resolve that by turning ligatures off, which also keeps the
        // drawn extent equal to the layout box (which counts characters).
        SkShaper::Feature features[2];
        std::size_t featureCount = 0;
        if (disableLigatures) {
            features[0] = {SkSetFourByteTag('l','i','g','a'), 0, 0, utf8.size()};
            features[1] = {SkSetFourByteTag('c','l','i','g'), 0, 0, utf8.size()};
            featureCount = 2;
        }
        if (fontRuns && language && scriptRuns && bidiRuns) {
            ShapedRunHandler handler(b);
            shaper_->shape(utf8.data(), utf8.size(), *fontRuns, *bidiRuns, *scriptRuns,
                           *language, featureCount ? features : nullptr, featureCount,
                           std::numeric_limits<SkScalar>::max(), &handler);
            b.setNaturalWidth(handler.pen().fX);
            shaped = true;
        }
    }
#endif
    if (!shaped) {
        // No shaper (BRO_WITH_TEXT_SHAPING=OFF, or the factory failed): 1:1
        // codepoint→glyph with per-run coverage fallback — exactly the
        // behaviour every renderer had before shaping existed, expressed
        // through the same ShapedRun surface so there is only one draw path.
        buildUnshaped(*run, b, utf8, primary, style, fontMgr, fallback);
    }

    b.finish();
    ShapedRun* raw = run.get();
    if (cache_.size() >= kMaxEntries) cache_.clear();
    cache_.emplace(std::move(key), std::move(run));
    return raw;
}

void TextShapingEngine::buildUnshaped(ShapedRun& run, ShapedRun::Builder& b,
                                      std::string_view utf8,
                                      const SkFont& primary,
                                      SkFontStyle style,
                                      SkFontMgr* fontMgr,
                                      FontFallbackCache& fallback) {
    (void)run;
    auto textRuns = splitTextForFallback(utf8, primary, fontMgr, style, fallback);
    float pen = 0.0f;
    for (const auto& tr : textRuns) {
        // Walk codepoints so each glyph keeps a byte-accurate cluster value.
        std::vector<SkGlyphID> glyphs;
        std::vector<uint32_t>  clusters;
        std::size_t i = tr.start;
        const std::size_t end = tr.start + tr.length;
        while (i < end) {
            const unsigned char c0 = static_cast<unsigned char>(utf8[i]);
            std::size_t n = 1;
            if      ((c0 & 0x80) == 0x00) n = 1;
            else if ((c0 & 0xE0) == 0xC0) n = 2;
            else if ((c0 & 0xF0) == 0xE0) n = 3;
            else if ((c0 & 0xF8) == 0xF0) n = 4;
            if (i + n > end) n = end - i;
            SkUnichar cp = 0;
            switch (n) {
                case 1: cp = c0; break;
                case 2: cp = ((c0 & 0x1F) << 6) | (utf8[i + 1] & 0x3F); break;
                case 3: cp = ((c0 & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6) |
                             (utf8[i + 2] & 0x3F); break;
                default: cp = ((c0 & 0x07) << 18) | ((utf8[i + 1] & 0x3F) << 12) |
                              ((utf8[i + 2] & 0x3F) << 6) | (utf8[i + 3] & 0x3F); break;
            }
            // Bidi formatting characters (LRM/RLE/PDF/RLI/...) drive level
            // resolution and are never drawn. HarfBuzz drops them for us as
            // default-ignorables; this path has to be told, or a font that
            // does not map them contributes a .notdef box per control.
            if (!bidi::isFormattingChar(static_cast<uint32_t>(cp))) {
                glyphs.push_back(tr.font.unicharToGlyph(cp));
                clusters.push_back(static_cast<uint32_t>(i));
            }
            i += n;
        }
        if (glyphs.empty()) continue;

        std::vector<SkScalar> widths(glyphs.size());
        tr.font.getWidths(SkSpan<const SkGlyphID>(glyphs.data(), glyphs.size()),
                          SkSpan<SkScalar>(widths.data(), widths.size()));

        const std::size_t first = b.glyphs().size();
        b.grow(glyphs.size());
        b.addRun(tr.font, first, glyphs.size(), 0);
        for (std::size_t k = 0; k < glyphs.size(); ++k) {
            const std::size_t g = first + k;
            b.glyphs()[g]    = glyphs[k];
            b.clusters()[g]  = clusters[k];
            b.positions()[g] = SkPoint{pen, 0.0f};
            b.offsets()[g]   = SkPoint{0.0f, 0.0f};
            b.advances()[g]  = widths[k];
            pen += widths[k];
        }
    }
    b.setNaturalWidth(pen);
}

} // namespace bro::render
