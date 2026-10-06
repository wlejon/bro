#pragma once
// MuxSource (private to src/terminal): a bromux session's ScreenSource as
// the RowSource a <terminal> views, with the element's theme laid over the
// server's palette.
//
// The server's terminal starts from bropty's standard palette and the
// program may change entries (OSC 4 / 10 / 11 / 12); the theme is the
// element's, and differs per element attached to the same session. So each
// entry the server still has at bropty's standard value is the theme's, and
// each one the program changed is the program's -- what a local session
// shows with the same theme as its base palette.
//
// Everything else forwards to the ScreenSource, observers included (resize
// renumbering, alternate screen), so a TerminalView over this behaves as one
// over the ScreenSource. Single-threaded, like the ScreenSource: the
// session's lock.

#include <bropty/row_source.h>

#include <cstdint>

namespace bro::terminal {

class MuxSource final : public bropty::RowSource, private bropty::TerminalObserver {
public:
    explicit MuxSource(bropty::RowSource& inner);
    ~MuxSource() override;

    // The ScreenSource is gone (the client dropped the session's mirror):
    // stop forwarding to it. Nothing may read this source afterwards.
    void orphan() noexcept { inner_ = nullptr; }
    [[nodiscard]] bool orphaned() const noexcept { return inner_ == nullptr; }

    // The theme's palette: what entries left at the standard values show.
    void setBase(const bropty::Palette& base);

    // ---- bropty::RowSource
    [[nodiscard]] int cols() const noexcept override { return inner_->cols(); }
    [[nodiscard]] int rows() const noexcept override { return inner_->rows(); }
    [[nodiscard]] int64_t first_row() const noexcept override { return inner_->first_row(); }
    [[nodiscard]] int64_t screen_top_row() const noexcept override { return inner_->screen_top_row(); }
    [[nodiscard]] bool alt_screen_active() const noexcept override { return inner_->alt_screen_active(); }
    [[nodiscard]] bropty::RowView row_at(int64_t abs) const override { return inner_->row_at(abs); }
    [[nodiscard]] uint64_t row_serial(int64_t abs) const noexcept override { return inner_->row_serial(abs); }
    [[nodiscard]] const std::string* hyperlink_uri(int64_t row, uint32_t id) const noexcept override {
        return inner_->hyperlink_uri(row, id);
    }
    [[nodiscard]] uint64_t change_count() const noexcept override { return inner_->change_count() + themeChanges_; }
    [[nodiscard]] bropty::CursorState cursor() const noexcept override { return inner_->cursor(); }
    [[nodiscard]] const bropty::Modes& modes() const noexcept override { return inner_->modes(); }
    // The theme over the server's palette (noexcept: copies of fixed-size arrays).
    [[nodiscard]] const bropty::Palette& palette() const noexcept override;
    void advance_generation() noexcept override { inner_->advance_generation(); }
    void request_rows(int64_t first, int64_t end) const override { inner_->request_rows(first, end); }

private:
    void before_resize() override { notify_before_resize(); }
    void after_resize() override { notify_after_resize(); }
    void screen_switched() override { notify_screen_switched(); }

    bropty::RowSource* inner_;
    bropty::Palette base_;
    const bropty::Palette standard_;
    uint64_t themeChanges_ = 0;
    // The composed palette, rebuilt when the server's or the theme's changes.
    mutable bropty::Palette composed_;
    mutable bropty::Palette composedFrom_;
    mutable uint64_t composedTheme_ = UINT64_MAX;
};

} // namespace bro::terminal
