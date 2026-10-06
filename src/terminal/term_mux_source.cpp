#include "terminal/term_mux_source.h"

namespace bro::terminal {

namespace {

bool sameRgb(const bropty::Rgb& a, const bropty::Rgb& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

bool samePalette(const bropty::Palette& a, const bropty::Palette& b) {
    if (!sameRgb(a.foreground, b.foreground) || !sameRgb(a.background, b.background) || !sameRgb(a.cursor, b.cursor))
        return false;
    for (size_t i = 0; i < a.colors.size(); ++i)
        if (!sameRgb(a.colors[i], b.colors[i])) return false;
    return true;
}

} // namespace

MuxSource::MuxSource(bropty::RowSource& inner)
    : inner_(&inner), base_(bropty::Palette::standard()), standard_(bropty::Palette::standard()) {
    inner_->add_observer(this);
}

MuxSource::~MuxSource() {
    if (inner_) inner_->remove_observer(this);
}

void MuxSource::setBase(const bropty::Palette& base) {
    if (samePalette(base, base_)) return;
    base_ = base;
    ++themeChanges_;
}

const bropty::Palette& MuxSource::palette() const noexcept {
    const bropty::Palette& server = inner_->palette();
    if (composedTheme_ == themeChanges_ && samePalette(server, composedFrom_)) return composed_;
    composedFrom_ = server;
    composedTheme_ = themeChanges_;
    auto pick = [](const bropty::Rgb& s, const bropty::Rgb& standard, const bropty::Rgb& theme) {
        return sameRgb(s, standard) ? theme : s;
    };
    composed_.foreground = pick(server.foreground, standard_.foreground, base_.foreground);
    composed_.background = pick(server.background, standard_.background, base_.background);
    composed_.cursor = pick(server.cursor, standard_.cursor, base_.cursor);
    for (size_t i = 0; i < server.colors.size(); ++i)
        composed_.colors[i] = pick(server.colors[i], standard_.colors[i], base_.colors[i]);
    return composed_;
}

} // namespace bro::terminal
