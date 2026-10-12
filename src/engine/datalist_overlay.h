#pragma once

#include "engine/overlay.h"
#include <functional>
#include <string>
#include <vector>

namespace bro::engine {

/// The suggestions of an `<input list=...>`: the `<datalist>` options that
/// match what is typed, in a list under the field (above it when there is no
/// room below).
///
/// Unlike a `<select>`'s dropdown it is passive: the field keeps the focus,
/// the typing, IME and the page's hover; only a press on a row and the keys
/// that move through the list are its own. Up and Down move the highlight
/// (none highlighted is the text as typed), Page Up / Page Down move a page,
/// Enter takes the highlighted row (with none, the list closes and Enter goes
/// on to the page), Tab closes it and moves on, Escape closes it. A press on a
/// row takes it; a press anywhere else closes the list and goes on to what was
/// pressed.
class DatalistOverlay : public Overlay {
public:
    struct Option {
        std::string value;
        std::string label;  // shown beside the value when it says more
    };

    /// The field's box in the overlay's space, read again at each use so the
    /// list follows the field as the page scrolls or reflows. False: no box.
    using AnchorFn = std::function<bool(float& x, float& y, float& w, float& h)>;
    /// A row was taken: the value to put in the field.
    using PickFn = std::function<void(const std::string& value)>;

    DatalistOverlay(const void* owner, AnchorFn anchor, float viewportH,
                    std::vector<Option> options,
                    std::string fontFamily, float fontSize, PickFn onPick);

    /// The field the list belongs to (identity only, never dereferenced).
    const void* owner() const { return owner_; }

    /// The rows after the text changed: the highlight and scroll start over.
    void setOptions(std::vector<Option> options);
    const std::vector<Option>& options() const { return options_; }
    int highlighted() const { return highlighted_; }

    static constexpr int kMaxRows = 8;

    void getBounds(float& x, float& y, float& w, float& h) const override;
    void draw(render::Renderer* r) override;
    bool onMouseDown(float x, float y, int button) override;
    bool onMouseMove(float x, float y) override;
    bool onWheel(float x, float y, float dx, float dy) override;
    bool onKeyDown(int keycode, int mod) override;
    bool passive() const override { return true; }

private:
    int visibleRows() const;
    bool listRect(float& x, float& y, float& w, float& h) const;
    int rowAt(float x, float y) const;
    void highlight(int index);
    void pick(int index);

    const void* owner_;
    AnchorFn anchor_;
    float viewportH_;
    std::vector<Option> options_;
    std::string fontFamily_;
    float fontSize_;
    PickFn onPick_;

    int highlighted_ = -1;
    int scrollTop_ = 0;     // first visible row

    // Row metrics, from the font on first use (the measure renderer the
    // manager hands over at open, or the draw renderer).
    void resolveMetrics(render::Renderer* r) const;
    mutable bool metricsResolved_ = false;
    mutable float rowH_ = 0.0f, ascent_ = 0.0f;
};

} // namespace bro::engine
