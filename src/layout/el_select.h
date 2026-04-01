#pragma once

#include "layout/box.h"
#include "css/cascade.h"
#include "render/renderer.h"
#include <string>
#include <vector>

namespace bro::dom { class Element; }

namespace bro::layout {

// Standalone select dropdown renderer.
class ElSelect {
public:
    explicit ElSelect(render::Renderer* renderer);

    struct Option {
        std::string value;
        std::string text;
    };

    // Collect options from DOM <option> children
    std::vector<Option> getOptions() const;

    void initSelectedIndex();

    void draw(render::Renderer* renderer,
              const htmlayout::layout::LayoutBox& box,
              const htmlayout::css::ComputedStyle& style,
              float offsetX, float offsetY);

    void drawDropdown();

    int selectedIndex() const { return selectedIndex_; }
    void setSelectedIndex(int idx) { selectedIndex_ = idx; }

    bool isOpen() const { return open_; }
    void setOpen(bool o) { open_ = o; }

    int highlightedIndex() const { return highlightedIndex_; }
    void setHighlightedIndex(int idx) { highlightedIndex_ = idx; }

    void setElement(dom::Element* el) { elem_ = el; }
    dom::Element* element() const { return elem_; }

    struct DrawPos { float x, y, w, h; };
    DrawPos lastDrawPos() const { return lastDrawPos_; }

    void getContentSize(float& w, float& h);

    // Returns the line height used for dropdown items (for hover hit testing)
    float dropdownLineHeight() const;

private:
    uint64_t getFontHandle() const;

    render::Renderer* renderer_;
    dom::Element* elem_ = nullptr;
    int selectedIndex_ = 0;
    int highlightedIndex_ = -1;
    bool open_ = false;
    mutable DrawPos lastDrawPos_ = {0, 0, 0, 0};
};

} // namespace bro::layout
