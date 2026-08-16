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

    // docOffsetX/Y: the draw pass's document→surface translation (see
    // DrawTraversal::rootOffsetX_) — applied to the absoluteBorderBox()
    // projection so lastDrawPos() lands in the pass's surface space: app
    // *content space* for the app document (the dropdown overlay anchors
    // there and the engine translates mouse input into it once at the input
    // boundary), window space for system panels.
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
    // Resolved live from the layout tree, not read back from the last paint:
    // a select can be clicked before it has ever been painted (a panel
    // revealed and driven in one turn), and a paint-time anchor is
    // {0,0,0,0} then, which opens the dropdown in the window corner. Layout
    // is current at every click — hit-testing just used it. See the same
    // note on ElInput::lastDrawPos().
    DrawPos lastDrawPos() const;

    void getContentSize(float& w, float& h);

private:
    render::FontRef getFontRef() const;

    render::Renderer* renderer_;
    dom::Element* elem_ = nullptr;
    int selectedIndex_ = 0;
    mutable DrawPos lastDrawPos_ = {0, 0, 0, 0};
    // The draw pass's document→surface translation, captured in draw(). Zero
    // before the first frame, which is also the correct answer then (nothing
    // has scrolled yet), so lastDrawPos() works from the very first click.
    float docOffsetX_ = 0.0f;
    float docOffsetY_ = 0.0f;
};

} // namespace bro::layout
