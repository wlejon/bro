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

    // docOffsetX/Y: the draw pass's document→screen translation (see
    // DrawTraversal::rootOffsetX_) — applied to the absoluteBorderBox()
    // projection so lastDrawPos() is true screen space, the contract the
    // engine's overlay anchoring and mouse handling rely on.
    void draw(render::Renderer* renderer,
              const htmlayout::layout::LayoutBox& box,
              const htmlayout::css::ComputedStyle& style,
              float offsetX, float offsetY,
              float docOffsetX = 0, float docOffsetY = 0);

    int selectedIndex() const { return selectedIndex_; }
    void setSelectedIndex(int idx) { selectedIndex_ = idx; }

    void setElement(dom::Element* el) { elem_ = el; }
    dom::Element* element() const { return elem_; }

    struct DrawPos { float x, y, w, h; };
    DrawPos lastDrawPos() const { return lastDrawPos_; }

    void getContentSize(float& w, float& h);

private:
    render::FontRef getFontRef() const;

    render::Renderer* renderer_;
    dom::Element* elem_ = nullptr;
    int selectedIndex_ = 0;
    mutable DrawPos lastDrawPos_ = {0, 0, 0, 0};
};

} // namespace bro::layout
