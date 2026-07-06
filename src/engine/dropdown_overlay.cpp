#include "engine/dropdown_overlay.h"

#include <SDL3/SDL_keycode.h>
#include <algorithm>

namespace bro::engine {

DropdownOverlay::DropdownOverlay(float anchorX, float anchorY, float anchorW, float anchorH,
                                 float /*viewportW*/, float viewportH,
                                 std::vector<Option> options,
                                 int selectedIndex,
                                 std::string fontFamily, float fontSize,
                                 SelectCallback onSelect)
    : anchorX_(anchorX), anchorY_(anchorY), anchorW_(anchorW), anchorH_(anchorH),
      viewportH_(viewportH),
      options_(std::move(options)),
      highlightedIndex_(selectedIndex),
      fontFamily_(std::move(fontFamily)),
      fontSize_(fontSize),
      onSelect_(std::move(onSelect)) {
    if (highlightedIndex_ < 0 ||
        highlightedIndex_ >= static_cast<int>(options_.size())) {
        highlightedIndex_ = options_.empty() ? -1 : 0;
    }
}

float DropdownOverlay::lineHeight() const { return lineH_; }

int DropdownOverlay::indexAt(float x, float y) const {
    if (options_.empty()) return -1;
    if (x < anchorX_ || x >= anchorX_ + anchorW_) return -1;
    float dropY = anchorY_ + anchorH_;
    if (y < dropY) return -1;
    int idx = static_cast<int>((y - dropY - 1.0f) / lineH_);
    if (idx < 0 || idx >= static_cast<int>(options_.size())) return -1;
    return idx;
}

void DropdownOverlay::getBounds(float& x, float& y, float& w, float& h) const {
    // Bounds include the anchor rect itself so clicks on the select toggle
    // close the dropdown instead of re-opening it via focusNewControl.
    x = anchorX_;
    y = anchorY_;
    w = anchorW_;
    float listH = lineH_ * static_cast<float>(options_.size()) + 2.0f;
    h = anchorH_ + listH;
}

void DropdownOverlay::draw(render::Renderer* r) {
    if (!r || options_.empty()) return;

    render::FontRef font{fontFamily_, fontSize_, 400, false};

    // Lazy-resolve line metrics on the first paint — measureText("",...) gives
    // us ascent/descent/leading without shaping any glyphs.
    if (!metricsResolved_) {
        auto lm = render::LineMetrics::from(r->measureText("M", font));
        lineH_ = lm.lineHeight() + 8.0f; // line height + vertical padding
        ascent_ = lm.ascent;
        metricsResolved_ = true;
    }

    float dropX = anchorX_;
    float dropY = anchorY_ + anchorH_;
    float dropW = anchorW_;
    float dropH = lineH_ * static_cast<float>(options_.size()) + 2.0f;
    float padX = 6.0f;
    float padY = 4.0f;

    // Renderer colors are linear-float bromath::Color — 8-bit values must go
    // through cfromColor8 or anything > 1 saturates (the gray border rendered
    // white and the selection blue rendered cyan).
    r->fillRect(dropX, dropY, dropW, dropH, bromath::cfromColor8({255, 255, 255, 255}));
    r->drawRect(dropX, dropY, dropW, dropH, bromath::cfromColor8({118, 118, 118, 255}));

    for (int i = 0; i < static_cast<int>(options_.size()); ++i) {
        float itemY = dropY + 1.0f + i * lineH_;
        if (i == highlightedIndex_) {
            r->fillRect(dropX + 1, itemY, dropW - 2, lineH_,
                        bromath::cfromColor8({0, 120, 215, 255}));
            r->drawText(options_[i].text, dropX + padX,
                        itemY + padY + ascent_, font,
                        bromath::cfromColor8({255, 255, 255, 255}));
        } else {
            r->drawText(options_[i].text, dropX + padX,
                        itemY + padY + ascent_, font,
                        bromath::cfromColor8({0, 0, 0, 255}));
        }
    }
}

bool DropdownOverlay::onMouseDown(float x, float y, int /*button*/) {
    int idx = indexAt(x, y);
    if (idx >= 0) {
        highlightedIndex_ = idx;
        if (onSelect_) onSelect_(idx);
        requestDismiss();
        return true;
    }

    // Click was inside the anchor (select input) bounds — toggle-close.
    if (x >= anchorX_ && x < anchorX_ + anchorW_ &&
        y >= anchorY_ && y < anchorY_ + anchorH_) {
        requestDismiss();
        return true;
    }

    return true; // consume
}

bool DropdownOverlay::onMouseMove(float x, float y) {
    int idx = indexAt(x, y);
    if (idx < 0 || idx == highlightedIndex_) return false;
    highlightedIndex_ = idx;
    return true;
}

bool DropdownOverlay::onKeyDown(int keycode, int /*mod*/) {
    if (options_.empty()) return false;
    int n = static_cast<int>(options_.size());
    if (keycode == SDLK_DOWN) {
        highlightedIndex_ = std::min(n - 1, highlightedIndex_ + 1);
        return true;
    }
    if (keycode == SDLK_UP) {
        highlightedIndex_ = std::max(0, highlightedIndex_ - 1);
        return true;
    }
    if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
        if (highlightedIndex_ >= 0 && highlightedIndex_ < n) {
            if (onSelect_) onSelect_(highlightedIndex_);
        }
        requestDismiss();
        return true;
    }
    return false;
}

} // namespace bro::engine
