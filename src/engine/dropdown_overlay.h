#pragma once

#include "engine/overlay.h"
#include <functional>
#include <string>
#include <vector>

namespace bro::engine {

/// Dropdown list overlay, replacing the old per-`<select>` draw + hover paths.
/// Rendered anchored below the select element.
class DropdownOverlay : public Overlay {
public:
    struct Option {
        std::string value;
        std::string text;
    };

    /// onSelect: fired when a value is committed (click or Enter).
    /// The argument is the index into the options list.
    using SelectCallback = std::function<void(int index)>;

    DropdownOverlay(float anchorX, float anchorY, float anchorW, float anchorH,
                    float viewportW, float viewportH,
                    std::vector<Option> options,
                    int selectedIndex,
                    std::string fontFamily, float fontSize,
                    SelectCallback onSelect);

    void getBounds(float& x, float& y, float& w, float& h) const override;
    void draw(render::Renderer* r) override;
    bool onMouseDown(float x, float y, int button) override;
    bool onMouseMove(float x, float y) override;
    bool onKeyDown(int keycode, int mod) override;

private:
    float lineHeight() const;
    int indexAt(float x, float y) const;

    float anchorX_, anchorY_, anchorW_, anchorH_;
    float viewportH_ = 0.0f;
    std::vector<Option> options_;
    int highlightedIndex_;
    std::string fontFamily_;
    float fontSize_;
    SelectCallback onSelect_;

    bool metricsResolved_ = false;
    float lineH_ = 20.0f;
    float ascent_ = 16.0f;
};

} // namespace bro::engine
