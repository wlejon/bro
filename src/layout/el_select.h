#pragma once

#include <litehtml.h>
#include <string>
#include <vector>

namespace bro::render { class Renderer; }

namespace bro::layout {

/// Custom litehtml element for <select> — dropdown option picker.
/// Acts as a replaced element that shows the selected option and
/// renders a dropdown list when open.
class ElSelect : public litehtml::html_tag {
public:
    ElSelect(const std::shared_ptr<litehtml::document>& doc,
             render::Renderer* renderer);

    bool is_replaced() const override { return true; }
    void parse_attributes() override;
    void compute_styles(bool recursive) override;
    void get_content_size(litehtml::size& sz, litehtml::pixel_t max_width) override;
    void draw(litehtml::uint_ptr hdc, litehtml::pixel_t x, litehtml::pixel_t y,
              const litehtml::position* clip,
              const std::shared_ptr<litehtml::render_item>& ri) override;
    std::shared_ptr<litehtml::render_item> create_render_item(
        const std::shared_ptr<litehtml::render_item>& parent_ri) override;

    // Option data extracted from <option> children
    struct Option {
        std::string value;
        std::string text;
    };

    // Collect options from litehtml children
    std::vector<Option> getOptions() const;

    // Initialize selectedIndex from the "selected" attribute on options
    void initSelectedIndex();

    // Selected index (managed by engine)
    int selectedIndex() const { return selectedIndex_; }
    void setSelectedIndex(int idx) { selectedIndex_ = idx; }

    // Dropdown open state
    bool isOpen() const { return open_; }
    void setOpen(bool o) { open_ = o; }

    // Highlighted index (for keyboard/hover in dropdown)
    int highlightedIndex() const { return highlightedIndex_; }
    void setHighlightedIndex(int idx) { highlightedIndex_ = idx; }

    // Store last drawn position for dropdown hit testing
    struct DrawPos { float x, y, w, h; };
    DrawPos lastDrawPos() const { return lastDrawPos_; }

private:
    render::Renderer* renderer_;
    int selectedIndex_ = 0;
    int highlightedIndex_ = -1;
    bool open_ = false;
    mutable DrawPos lastDrawPos_ = {0, 0, 0, 0};
};

} // namespace bro::layout
