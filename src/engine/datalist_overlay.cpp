#include "engine/datalist_overlay.h"

#include "platform/keys.h"

#include <algorithm>

namespace bro::engine {

DatalistOverlay::DatalistOverlay(const void* owner, AnchorFn anchor, float viewportH,
                                 std::vector<Option> options,
                                 std::string fontFamily, float fontSize, PickFn onPick)
    : owner_(owner), anchor_(std::move(anchor)), viewportH_(viewportH),
      options_(std::move(options)), fontFamily_(std::move(fontFamily)),
      fontSize_(fontSize), onPick_(std::move(onPick)) {}

void DatalistOverlay::setOptions(std::vector<Option> options) {
    options_ = std::move(options);
    highlighted_ = -1;
    scrollTop_ = 0;
}

void DatalistOverlay::resolveMetrics(render::Renderer* r) const {
    if (metricsResolved_) return;
    if (!r) r = measureRenderer_;
    if (!r) {
        // Nothing to measure with yet: a guess from the font size, replaced
        // at the first draw.
        rowH_ = fontSize_ * 1.25f + 8.0f;
        ascent_ = fontSize_ * 0.9f;
        return;
    }
    render::FontRef font{fontFamily_, fontSize_, 400, false};
    auto lm = render::LineMetrics::from(r->measureText("M", font));
    rowH_ = lm.lineHeight() + 8.0f;
    ascent_ = lm.ascent;
    metricsResolved_ = true;
}

int DatalistOverlay::visibleRows() const {
    return std::min(static_cast<int>(options_.size()), kMaxRows);
}

bool DatalistOverlay::listRect(float& x, float& y, float& w, float& h) const {
    float ax = 0, ay = 0, aw = 0, ah = 0;
    if (!anchor_ || !anchor_(ax, ay, aw, ah)) return false;
    resolveMetrics(nullptr);
    x = ax;
    w = std::max(aw, 120.0f);
    h = rowH_ * static_cast<float>(visibleRows()) + 2.0f;
    y = ay + ah;
    // No room below and more above: open upward, as browsers do.
    if (viewportH_ > 0 && y + h > viewportH_ && ay - h >= 0) y = ay - h;
    return true;
}

void DatalistOverlay::getBounds(float& x, float& y, float& w, float& h) const {
    if (options_.empty() || !listRect(x, y, w, h)) { x = y = w = h = 0; }
}

int DatalistOverlay::rowAt(float px, float py) const {
    float x, y, w, h;
    if (options_.empty() || !listRect(x, y, w, h)) return -1;
    if (px < x || px >= x + w || py < y + 1.0f || py >= y + h - 1.0f) return -1;
    int row = static_cast<int>((py - y - 1.0f) / rowH_);
    int idx = scrollTop_ + row;
    if (row < 0 || row >= visibleRows() || idx >= static_cast<int>(options_.size())) return -1;
    return idx;
}

void DatalistOverlay::highlight(int index) {
    const int n = static_cast<int>(options_.size());
    highlighted_ = (index < 0 || index >= n) ? -1 : index;
    if (highlighted_ < 0) return;
    if (highlighted_ < scrollTop_) scrollTop_ = highlighted_;
    if (highlighted_ >= scrollTop_ + kMaxRows) scrollTop_ = highlighted_ - kMaxRows + 1;
}

void DatalistOverlay::pick(int index) {
    if (index < 0 || index >= static_cast<int>(options_.size())) return;
    // Dismissed first: the pick dispatches `input`, and what the page does
    // there must find the list already on its way out.
    requestDismiss();
    if (onPick_) onPick_(options_[index].value);
}

void DatalistOverlay::draw(render::Renderer* r) {
    if (!r || options_.empty()) return;
    resolveMetrics(r);
    float x, y, w, h;
    if (!listRect(x, y, w, h)) return;

    render::FontRef font{fontFamily_, fontSize_, 400, false};
    const float padX = 6.0f, padY = 4.0f;
    const auto bg = bromath::cfromColor8({255, 255, 255, 255});
    const auto border = bromath::cfromColor8({118, 118, 118, 255});
    const auto selBg = bromath::cfromColor8({0, 120, 215, 255});
    const auto text = bromath::cfromColor8({0, 0, 0, 255});
    const auto selText = bromath::cfromColor8({255, 255, 255, 255});
    const auto dim = bromath::cfromColor8({110, 110, 110, 255});
    const auto selDim = bromath::cfromColor8({220, 230, 245, 255});

    r->fillRect(x, y, w, h, bg);
    r->drawRect(x, y, w, h, border);

    const int rows = visibleRows();
    const bool scrolls = static_cast<int>(options_.size()) > rows;
    const float barW = scrolls ? 4.0f : 0.0f;
    for (int row = 0; row < rows; ++row) {
        const int i = scrollTop_ + row;
        if (i >= static_cast<int>(options_.size())) break;
        const auto& o = options_[i];
        const float ry = y + 1.0f + static_cast<float>(row) * rowH_;
        const bool on = i == highlighted_;
        if (on) r->fillRect(x + 1, ry, w - 2 - barW, rowH_, selBg);
        r->save();
        r->setClip(x + 1, ry, w - 2 - barW, rowH_);
        const float base = ry + padY + ascent_;
        r->drawText(o.value, x + padX, base, font, on ? selText : text);
        if (!o.label.empty() && o.label != o.value) {
            // The label right-aligned, as far as it does not run into the value.
            const float vw = r->measureText(o.value, font).width;
            const float lw = r->measureText(o.label, font).width;
            const float lx = std::max(x + padX + vw + 2 * padX, x + w - barW - padX - lw);
            r->drawText(o.label, lx, base, font, on ? selDim : dim);
        }
        r->restore();
    }
    if (scrolls) {
        const float n = static_cast<float>(options_.size());
        const float trackH = h - 2.0f;
        const float thumbH = std::max(12.0f, trackH * static_cast<float>(rows) / n);
        const float thumbY = y + 1.0f + (trackH - thumbH) * static_cast<float>(scrollTop_) /
                                 std::max(1.0f, n - static_cast<float>(rows));
        r->fillRect(x + w - 1 - barW, thumbY, barW, thumbH,
                    bromath::cfromColor8({160, 160, 160, 255}));
    }
}

bool DatalistOverlay::onMouseDown(float x, float y, int button) {
    if (button == 0) pick(rowAt(x, y));
    return true;  // a press inside the list is the list's, picked or not
}

bool DatalistOverlay::onMouseMove(float x, float y) {
    int idx = rowAt(x, y);
    if (idx < 0 || idx == highlighted_) return false;
    highlighted_ = idx;
    return true;
}

bool DatalistOverlay::onWheel(float, float, float, float dy) {
    const int n = static_cast<int>(options_.size());
    const int maxTop = std::max(0, n - kMaxRows);
    const int step = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    scrollTop_ = std::clamp(scrollTop_ + step, 0, maxTop);
    return true;
}

bool DatalistOverlay::onKeyDown(int keycode, int mod) {
    const int n = static_cast<int>(options_.size());
    if (n == 0) return false;
    // Keys with a command modifier are the page's (select all, undo, ...).
    if (platform::hasPrimaryMod(mod)) return false;
    if (keycode == platform::kc::Down) {
        // From the last row, back to the text as typed.
        highlight(highlighted_ + 1 >= n ? -1 : highlighted_ + 1);
        return true;
    }
    if (keycode == platform::kc::Up) {
        highlight(highlighted_ < 0 ? n - 1 : highlighted_ - 1);
        return true;
    }
    if (keycode == platform::kc::PageDown) {
        highlight(std::min(n - 1, std::max(0, highlighted_) + kMaxRows));
        return true;
    }
    if (keycode == platform::kc::PageUp) {
        highlight(std::max(0, highlighted_ - kMaxRows));
        return true;
    }
    if (keycode == platform::kc::Return || keycode == platform::kc::KpEnter) {
        if (highlighted_ >= 0) {
            pick(highlighted_);
            return true;
        }
        requestDismiss();
        return false;  // Enter on the text as typed is the page's
    }
    if (keycode == platform::kc::Tab) {
        requestDismiss();
        return false;
    }
    return false;
}

} // namespace bro::engine
