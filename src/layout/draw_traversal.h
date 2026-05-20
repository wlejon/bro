#pragma once

#include "render/renderer.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bro::canvas { class CanvasScene; }
namespace bro::dom { class Element; class Node; }

namespace bro::layout {

// Border-box clip recorded for an ancestor between an SC root and one of its
// step-2/6/7 paintees. paintStackingContext re-applies these around each such
// paint so that overflow clips set by non-SC ancestors during the in-flow walk
// (which save/restore within drawElementContent) still cover positioned
// descendants painted out-of-line.
struct ClipRect {
    float bx, by, bw, bh;
    render::Radii radii;
};

// One entry in the stacking-context tree (CSS 2.1 Appendix E).
// Each SC root collects its descendant elements partitioned by the seven
// painting steps. Offsets are absolute in the output surface.
struct StackingContext {
    dom::Element* root = nullptr;
    float offsetX = 0;       // absolute offset to be passed to drawElementContent
    float offsetY = 0;
    int   zIndex = 0;        // resolved z-index (auto -> 0 for ordering)
    bool  zIsAuto = true;    // true when z-index is 'auto' (treated as 0 but participates in step 6)
    // Clip chain (outermost-first) inherited from the parent SC root down to
    // but not including this SC root. Re-applied around the paintStackingContext
    // call for this child SC.
    std::vector<ClipRect> ancestorClips;

    // Step 3+5 merged: in-flow, non-positioned descendants painted in tree order
    // alongside step 4 (floats). These are descendants whose nearest SC ancestor is
    // this SC and which are not themselves SC roots, not positioned, not floats.
    // We use a single "in-flow" list painted in tree order via the normal
    // drawElementContent walk, with a stop-set for nested SC roots and positioned
    // non-SC descendants so those subtrees are skipped during the normal walk.
    // (Floats are not separately tracked; they'll fall into in-flow tree order.)

    // Step 6: positioned descendants that are NOT themselves stacking-context roots
    // (i.e. position:relative/absolute with z-index:auto, or sticky without SC trigger).
    // Painted in tree order alongside z-index:auto child SCs. Each entry records
    // the absolute offset to draw at.
    struct PositionedEntry {
        dom::Element* elem;
        float offsetX;
        float offsetY;
        int   tieBreaker; // tree-order tie-breaker (DFS index)
        // Clip chain from the SC root (exclusive) down to this entry (exclusive),
        // outermost-first. Re-applied around the entry's drawElementContent call.
        std::vector<ClipRect> ancestorClips;
    };
    std::vector<PositionedEntry> positionedNonSC;

    // Child stacking contexts (each painted recursively). Sorted by z-index then
    // tree order at paint time:
    //   step 2: zIndex < 0
    //   step 6: zIsAuto (interleaved with positionedNonSC by tree order)
    //   step 7: zIndex > 0  (and zIndex == 0 with !zIsAuto)
    std::vector<std::unique_ptr<StackingContext>> children;
    int treeOrder = 0; // DFS index assigned when first encountered (for stable sort)
};

// Image cache entry for background-image / <img> rendering
struct CachedImage {
    std::vector<uint8_t> data;  // raster bytes (PNG/JPG/etc) OR raw SVG markup when isSvg
    int width = 0;
    int height = 0;
    bool isSvg = false;
    // Process-unique id for the decoded-image cache in the renderer. Assigned
    // when the entry is created; 0 means "uncacheable" (e.g. a failed load).
    uint64_t id = 0;
};

// Walks the DOM tree with layout boxes and issues Renderer draw calls.
class DrawTraversal {
public:
    explicit DrawTraversal(render::Renderer* renderer);

    // Draw the full document tree. `viewportTop` is the Y position in the
    // output surface where the content area begins (0 for apps without a
    // reserved top inset).
    void draw(dom::Element* root, float scrollX, float scrollY,
              int viewportW, int viewportH, int viewportTop = 0);

    // Draw a single element and its subtree (used for overlays)
    void drawElement(dom::Element* elem, float offsetX, float offsetY);

    // Load an image from disk into the cache
    void loadImage(const std::string& url, const std::string& basePath);

    // Set base path for resolving relative image URLs
    void setBasePath(const std::string& path) { basePath_ = path; }

    // Layer break callback: invoked when a canvas or WebGL element is encountered
    // during traversal. The compositor uses this to split HTML rendering
    // into separate layers around canvas/WebGL elements.
    // For Canvas2D: scene is non-null, directTexture is 0.
    // For WebGL: scene is null, directTexture is the FBO color texture.
    using LayerBreakCallback = std::function<void(canvas::CanvasScene* scene,
                                                   unsigned int directTexture,
                                                   float x, float y, float w, float h)>;
    void setLayerBreakCallback(LayerBreakCallback cb) { layerBreakCb_ = std::move(cb); }

    // Viewport. `top` is the Y position in the output surface where the
    // content area begins — used for the html/body background paint rect so
    // engine-reserved insets (e.g. menu bar) aren't painted over.
    void setViewport(int w, int h, int top = 0) {
        viewportW_ = w; viewportH_ = h; viewportTop_ = top;
    }

    render::Renderer* renderer() { return renderer_; }

private:
    void drawNode(dom::Node* node, float offsetX, float offsetY);
    void drawElementContent(dom::Element* elem, float offsetX, float offsetY);
    void drawBackground(dom::Element* elem, float x, float y, float w, float h);
    void drawBorders(dom::Element* elem, float x, float y, float w, float h);
    void drawText(dom::Node* textNode, dom::Element* parent, float offsetX, float offsetY);
    void drawPseudo(dom::Element* host, const std::string& which,
                    float offsetX, float offsetY);

    // CSS 2.1 Appendix E painting (stacking contexts + z-index order).
    // buildStackingContextTree walks the DOM accumulating absolute offsets and
    // partitions descendants into the nearest SC ancestor's buckets.
    // paintStackingContext does the seven-step paint order recursively.
    std::unique_ptr<StackingContext> buildStackingContextTree(
        dom::Element* root, float scrollX, float scrollY);
    void paintStackingContext(StackingContext* sc);

    // While drawElementContent walks children, it consults skipSet_ to avoid
    // descending into elements that will be painted separately by the SC walker
    // (their own SC, or a positioned non-SC descendant in some ancestor SC's
    // step-6 bucket).
    std::unordered_set<dom::Element*> skipSet_;

    // Stacking-context roots whose transform/opacity/filter wrappers have
    // already been applied by paintStackingContext so all descendants (not
    // only step-1 in-flow content) inherit them. drawElementContent skips
    // re-applying them for these roots.
    std::unordered_set<dom::Element*> scRootSkipWrap_;

public:
    // Color parsing helper (public for shared use by element controls)
    static bromath::Color parseColor(const std::string& color);
    static bool tryParseColor(const std::string& color, bromath::Color& out);

private:

    // FontRef for an element's computed style. The returned ref's `family`
    // string_view borrows from the element's computedStyle map — valid for
    // the duration of the draw pass.
    render::FontRef getFontRef(dom::Element* elem);

    render::Renderer* renderer_;
    std::string basePath_;
    int viewportW_ = 0;
    int viewportH_ = 0;
    int viewportTop_ = 0;

    std::unordered_map<std::string, CachedImage> imageCache_;
    LayerBreakCallback layerBreakCb_;
};

} // namespace bro::layout
