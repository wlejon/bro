#pragma once

#include "layout/box.h"
#include "layout/cluster_caret.h"
#include "render/renderer.h"
#include "render/shaped_run.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <cmath>
#include <cstdlib>
#include <cstdint>

namespace bro::layout {

// Implements htmlayout::layout::TextMetrics by building a render::FontRef from
// the layout-supplied family/size/weight and forwarding to the Renderer.
// The renderer caches by descriptor internally — no separate handle table.
class SkiaTextMetrics : public htmlayout::layout::TextMetrics {
public:
    explicit SkiaTextMetrics(render::Renderer* renderer)
        : renderer_(renderer) {}

    float measureWidth(std::string_view text,
                       std::string_view fontFamily,
                       float fontSize,
                       std::string_view fontWeight) override {
        auto tm = measure(text, fontFamily, fontSize, fontWeight);
        return tm.width;
    }

    float lineHeight(std::string_view fontFamily,
                     float fontSize,
                     std::string_view fontWeight) override {
        // Empty-text measureText returns just the font's vertical metrics —
        // the renderer pulls these straight from SkFontMetrics, no glyph
        // shaping needed.
        auto tm = measure("", fontFamily, fontSize, fontWeight);
        // CSS line-height: normal = ascent + descent + leading, with each
        // component rounded to an integer independently — Blink rounds
        // SkFontMetrics fAscent/fDescent/fLeading per component in
        // SimpleFontData::PlatformInit, so 16px Arial is 14 + 3 + 1 = 18,
        // not round(14.48 + 3.39 + 0.52) (which happens to agree) — the
        // per-component form is exact for all sizes.
        float h = std::round(tm.ascent) + std::round(tm.descent) + std::round(tm.leading);
        return h > 0 ? h : fontSize * 1.2f;
    }

    float naturalHeight(std::string_view fontFamily,
                        float fontSize,
                        std::string_view fontWeight) override {
        auto tm = measure("", fontFamily, fontSize, fontWeight);
        // Text-run rect height is the font box without line gap: Blink
        // reports round(ascent) + round(descent) (17 for 16px Arial, where
        // line-height: normal is 18 with the gap).
        float h = std::round(tm.ascent) + std::round(tm.descent);
        return h > 0 ? h : lineHeight(fontFamily, fontSize, fontWeight);
    }

    float ascent(std::string_view fontFamily,
                 float fontSize,
                 std::string_view fontWeight) override {
        auto tm = measure("", fontFamily, fontSize, fontWeight);
        // Blink rounds the font ascent to an integer (SkScalarRoundToScalar
        // on -fAscent); baselines land on integral offsets from the line top.
        float a = std::round(tm.ascent);
        // Fall back to the interface default's 80% heuristic if the backend
        // gave nothing usable.
        if (a <= 0) return 0.8f * lineHeight(fontFamily, fontSize, fontWeight);
        return a;
    }

    float xHeight(std::string_view fontFamily,
                  float fontSize,
                  std::string_view fontWeight) override {
        auto tm = measure("", fontFamily, fontSize, fontWeight);
        // Real x-height from SkFontMetrics (unrounded, matching Blink).
        // Fonts that don't report one fall back to the CSS 0.5em ratio.
        return tm.xHeight > 0 ? tm.xHeight : 0.5f * fontSize;
    }

    // --- caret geometry, answered from the shaper's cluster map -------------
    //
    // Each of these falls back to the interface default — prefix measurement,
    // the pre-shaping behaviour — when the shaped run is unavailable, which is
    // the case for a backend with no shaper as well as when the cluster path
    // is switched off. See layout/cluster_caret.h.

    bool clusterAware() const override { return clusterCaretEnabled(); }

    CaretXPair caretXAtOffset(std::string_view text, int byteOffset,
                              std::string_view fontFamily, float fontSize,
                              std::string_view fontWeight) override {
        const render::ShapedRun* run = shaped(text, fontFamily, fontSize, fontWeight);
        if (!run) {
            return TextMetrics::caretXAtOffset(text, byteOffset, fontFamily,
                                               fontSize, fontWeight);
        }
        const int n = static_cast<int>(text.size());
        const std::size_t off =
            static_cast<std::size_t>(byteOffset < 0 ? 0 : (byteOffset > n ? n : byteOffset));
        auto cp = run->byteOffsetToX(off);
        CaretXPair out;
        out.primary = {cp.primary.x, cp.primary.isLeadingEdge};
        out.secondary = {cp.secondary.x, cp.secondary.isLeadingEdge};
        out.hasSecondary = cp.hasSecondary;
        return out;
    }

    int offsetAtCaretX(std::string_view text, float x,
                       std::string_view fontFamily, float fontSize,
                       std::string_view fontWeight) override {
        const render::ShapedRun* run = shaped(text, fontFamily, fontSize, fontWeight);
        if (!run) {
            return TextMetrics::offsetAtCaretX(text, x, fontFamily, fontSize, fontWeight);
        }
        return static_cast<int>(run->xToByteOffset(x));
    }

    ClusterSpan clusterRangeAt(std::string_view text, int byteOffset,
                               std::string_view fontFamily, float fontSize,
                               std::string_view fontWeight) override {
        const render::ShapedRun* run = shaped(text, fontFamily, fontSize, fontWeight);
        if (!run) {
            return TextMetrics::clusterRangeAt(text, byteOffset, fontFamily,
                                               fontSize, fontWeight);
        }
        const int n = static_cast<int>(text.size());
        const std::size_t off =
            static_cast<std::size_t>(byteOffset < 0 ? 0 : (byteOffset > n ? n : byteOffset));
        auto cr = run->clusterRange(off);
        return {static_cast<int>(cr.byteStart), static_cast<int>(cr.byteEnd)};
    }

private:
    // Shape through the renderer's own cache. The returned run is owned by that
    // cache and dies at the next shape() that misses, so every use is
    // immediate and nothing here holds on to one.
    const render::ShapedRun* shaped(std::string_view text, std::string_view family,
                                    float size, std::string_view weight) {
        if (!clusterCaretEnabled() || text.empty()) return nullptr;
        measureCalls++;
        return renderer_->shapeText(text, makeRef(family, size, weight));
    }

    // The one place every metric this class serves reaches the renderer — so the
    // one place worth caching.
    //
    // Layout asks the same question over and over: the width of the same word in
    // the same font on every pass, the width of " " for every inter-word gap, the
    // vertical metrics of a font (an empty-string measure) for every line box it
    // builds. Shaping is a few microseconds a call and one pass over a busy
    // document makes tens of thousands of them, which is most of what a slow
    // layout pass is actually made of — measured on a 4k-element app, a single
    // relayout spent 15 of its 16ms here.
    //
    // Keyed on the exact inputs the interface takes, so a hit is exact, not an
    // approximation. Dropped whole when a custom font arrives, since the same
    // family can resolve to a different face across that.
    render::TextMetrics measure(std::string_view text, std::string_view family,
                                float size, std::string_view weight) {
        measureCalls++;
        render::FontRef ref = makeRef(family, size, weight);
        if (renderer_->fontGeneration() != fontGeneration_) {
            cache_.clear();
            fontGeneration_ = renderer_->fontGeneration();
        }
        Key key{std::string{text}, std::string{ref.family}, ref.size, ref.weight};
        if (auto it = cache_.find(key); it != cache_.end()) return it->second;

        render::TextMetrics tm = renderer_->measureText(text, ref);
        // A document that measures unboundedly many distinct strings (a running
        // log, a text field being typed into) must not grow this forever. Start
        // over rather than evict: the next pass re-measures only what it still
        // needs, and LRU bookkeeping on every hit would cost more than the
        // occasional refill.
        if (cache_.size() >= kMaxEntries) cache_.clear();
        cache_.emplace(std::move(key), tm);
        return tm;
    }

    struct Key {
        std::string text;
        std::string family;
        float size;
        int weight;
        bool operator==(const Key& o) const {
            return size == o.size && weight == o.weight &&
                   text == o.text && family == o.family;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            size_t h = std::hash<std::string_view>{}(k.text);
            auto mix = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
            mix(std::hash<std::string_view>{}(k.family));
            mix(std::hash<float>{}(k.size));
            mix(static_cast<size_t>(k.weight));
            return h;
        }
    };
    static constexpr size_t kMaxEntries = 1 << 16;
    std::unordered_map<Key, render::TextMetrics, KeyHash> cache_;
    uint64_t fontGeneration_ = 0;

    render::FontRef makeRef(std::string_view family, float size,
                            std::string_view weight) {
        int w = 400;
        if (weight == "bold" || weight == "700") w = 700;
        else if (weight == "lighter" || weight == "100") w = 100;
        else if (weight == "200") w = 200;
        else if (weight == "300") w = 300;
        else if (weight == "500") w = 500;
        else if (weight == "600") w = 600;
        else if (weight == "800") w = 800;
        else if (weight == "900") w = 900;
        else if (weight == "normal" || weight.empty()) w = 400;
        else {
            std::string ws{weight};
            char* end = nullptr;
            long v = std::strtol(ws.c_str(), &end, 10);
            if (end != ws.c_str() && v > 0) w = static_cast<int>(v);
        }
        return render::FontRef{
            family.empty() ? std::string_view{"Arial"} : family,
            size > 0 ? size : 16.0f, w, false};
    }

    render::Renderer* renderer_;
};

} // namespace bro::layout
