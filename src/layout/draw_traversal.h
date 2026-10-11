#pragma once

#include "layout/paint_cache.h"
#include "layout/scroll_layer.h"
#include "render/layer_source.h"
#include "render/renderer.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bro::canvas { class CanvasScene; }
namespace bro::render { struct DecodedImage; class ImageRequest; class RecordingRenderer; }
namespace bro::dom { class Element; class Node; }
namespace htmlayout::css { class ComputedStyle; }
namespace htmlayout::layout { struct LayoutBox; }

namespace bro::layout {

// Border-box clip recorded for an ancestor between an SC root and one of its
// step-2/6/7 paintees. paintStackingContext re-applies these around each such
// paint so that overflow clips set by non-SC ancestors during the in-flow walk
// (which save/restore within drawElementContent) still cover positioned
// descendants painted out-of-line.
struct ClipRect {
    float bx, by, bw, bh;
    render::Radii radii;
    // The element whose `overflow` produced this clip. An out-of-flow box is
    // only clipped by ancestors of its containing block, so a positioned
    // descendant has to be able to ask which ancestor each clip came from and
    // drop the ones it escapes — that is how a `position: fixed` submenu gets
    // to hang outside the `overflow: auto` panel it is nested in.
    dom::Element* owner = nullptr;
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

// Walks the DOM tree with layout boxes and issues Renderer draw calls.
class DrawTraversal {
public:
    explicit DrawTraversal(render::Renderer* renderer);

    // Draw the full document tree. `viewportTop` is the Y position in the
    // output surface where the content area begins. The app document draws
    // into content-sized surfaces in content space, so the engine always
    // passes 0 — engine-reserved insets are applied by the compositor when
    // placing the layers, never inside the traversal.
    void draw(dom::Element* root, float scrollX, float scrollY,
              int viewportW, int viewportH, int viewportTop = 0);

    // Draw a single element and its subtree (used for overlays)
    void drawElement(dom::Element* elem, float offsetX, float offsetY);

    // The decoded picture behind an image URL (a CSS url(), an <img> src)
    // for painting: the shared store's (render/image_store.h), decoded off
    // the page thread. Null while it decodes — the gap paints nothing, and
    // the settle repaints (layout/image_loading.h) — or when it is broken.
    // `oriented`: EXIF orientation applied (image-orientation: from-image).
    // An animated image is the frame showing now (render/animated_image.h);
    // `forElem` is the element it paints, whose box on screen is what lets
    // the animation ask for its next frame (null: assume it is on screen).
    std::shared_ptr<const render::DecodedImage> paintImage(const std::string& url, bool oriented = true,
                                                           dom::Element* forElem = nullptr);

    // Set base path for resolving relative image URLs
    void setBasePath(const std::string& path) { basePath_ = path; }

    // A layer break: the traversal reached an element whose content is
    // composited as its own layer (a 2D canvas, WebGL, a 3D scene, an iframe
    // sub-document) rather than painted through the renderer. The callback
    // splits the HTML around it so the layers interleave in document order.
    // `quad` is the element's content box projected through its ancestor
    // transforms (axis-aligned), in the pass's surface space, with the
    // overflow/scroll clip active there. `element` is the element itself,
    // for callers that draw the content inline instead (system panels,
    // sub-documents).
    struct LayerBreak {
        dom::Element* element = nullptr;
        render::LayerSource source;
        render::LayerQuad quad;
    };
    using LayerBreakCallback = std::function<void(const LayerBreak&)>;
    void setLayerBreakCallback(LayerBreakCallback cb) { layerBreakCb_ = std::move(cb); }
    // A <terminal> as its own layer (render::TerminalLayerSource) instead of
    // drawn inline: the app document's pass, whose compositor places it.
    void setTerminalLayers(bool on) { terminalLayers_ = on; }
    void setShellClientWindowsLayer(bool on) { shellClientWindows_ = on; }

    // The shell host's client windows, bottom to top, each with the element
    // the shell draws as its frame (null: none). A frame element leaves the
    // document's stacking order: it is painted, with its subtree, just below
    // its window, where the windows interleave with the shell (the
    // ClientWindowsLayerSource breaks). An element inside a frame carrying
    // data-window-overlay leaves the frame too: it is painted just above
    // the window (controls floated over the client, e.g. a borderless
    // window's buttons), under the windows above it. `insetLeft/Top` is where the client
    // sits inside the frame's border box. The runs the pass recorded are
    // appended to `out`; `listId` names them in the breaks.
    struct ClientWindowSlot {
        uint64_t windowId = 0;
        dom::Element* frame = nullptr;
        float insetLeft = 0, insetTop = 0, insetRight = 0, insetBottom = 0;
    };
    void setClientWindows(const std::vector<ClientWindowSlot>* slots,
                          std::vector<render::ClientWindowRef>* out, uint32_t listId) {
        clientSlots_ = slots;
        clientRefs_ = out;
        clientListId_ = listId;
    }

    // Viewport. `top` is the Y position in the output surface where the
    // content area begins — used for the html/body background paint rect.
    // 0 for the app document (content-space surfaces) and system panels.
    void setViewport(int w, int h, int top = 0) {
        viewportW_ = w; viewportH_ = h; viewportTop_ = top;
    }

    render::Renderer* renderer() { return renderer_; }

    // Paint-mode filter (used by the compositor to record the static UI and the
    // promoted / compositor-layer elements into SEPARATE command buffers via two
    // passes over the SAME stacking-context walk, so geometry is identical).
    //   All              : paint everything (default — no promoted logic runs).
    //   BaseSkipPromoted : paint everything EXCEPT promoted SC subtrees (leaving
    //                      transparent holes where they'd be).
    //   PromotedOnly     : paint ONLY promoted SC subtrees, at their correct
    //                      absolute position with their current transform baked in.
    // BaseSkipPromoted ∪ PromotedOnly reproduces All exactly (each SC painted in
    // exactly one pass). Promoted elements are always stacking-context roots.
    enum class PaintMode { All, BaseSkipPromoted, PromotedOnly };
    void setPaintMode(PaintMode m) { paintMode_ = m; }
    void setPromotedElements(const std::unordered_set<dom::Element*>* set) {
        promotedElements_ = set;
    }

    // The span cache (layout/paint_cache.h): on for draw() passes into a
    // recording renderer. setPaintCacheEnabled(false) turns it off for every
    // traversal (headless A/B: a pass with it off records everything and
    // leaves the cache as it was).
    static bool paintCacheEnabled();
    static void setPaintCacheEnabled(bool on);
    const PaintCacheStats& paintCacheStats() const { return cacheStats_; }

    // Scroll layers (layout/scroll_layer.h): the pass's host, null to paint
    // every scroller in place. The engine sets it around a pass's draw().
    void setScrollLayerHost(ScrollLayerHost* host) { scrollLayerHost_ = host; }
    // The render scale the pass's surfaces are drawn at: an overflow clip
    // (not a rounded one) is put on whole device pixels at it, where no
    // transform is on, so painting in place and a scroll layer's compositor
    // clip cut the same pixels (an edge on half a device pixel is one Skia
    // cuts per primitive). 0: as laid out (system panels).
    void setSnapScale(float scale) { snapScale_ = scale; }
    struct ScrollLayerStats {
        uint64_t layers = 0;  // scrollers composited as a layer, pass by pass
        uint64_t reused = 0;  // of those, whose recording stood: nothing in them recorded
        uint64_t pieces = 0;        // runs of their positioned descendants, pieces of them
        uint64_t piecesReused = 0;  // of those, whose recording stood
        uint64_t vetoes[static_cast<size_t>(ScrollLayerVeto::Count)] = {};
    };
    const ScrollLayerStats& scrollLayerStats() const { return scrollLayerStats_; }

private:
    // The box a decoration painter paints: an element's, or a ::before /
    // ::after pseudo-element's. A generated box is painted by the same
    // painter as an element (shadows, backgrounds and gradients, borders of
    // every style and radius, outline), so `elem` is null for a pseudo and
    // the element-only cases (the viewport background of html/body, a
    // fieldset's legend gap, collapsed tables) do not apply to it. `host` is
    // the element itself, or the pseudo's originating element: what em/rem
    // and the document resolve against.
    struct PaintBox {
        const htmlayout::css::ComputedStyle& style;
        const htmlayout::layout::LayoutBox& box;
        dom::Element* elem;
        dom::Element* host;
    };
    static PaintBox paintBoxOf(dom::Element* elem);

    void drawNode(dom::Node* node, float offsetX, float offsetY);
    // Through the span cache when it is on for this pass, else paintElement.
    void drawElementContent(dom::Element* elem, float offsetX, float offsetY);
    void paintElement(dom::Element* elem, float offsetX, float offsetY);
    // An element's content: ::before, its children, ::after, a line-clamp
    // ellipsis, drawn at the content origin (childOffsetX, childOffsetY).
    void paintContent(dom::Element* elem, const htmlayout::css::ComputedStyle& style,
                      const htmlayout::layout::LayoutBox& box, bool visible, float childOffsetX,
                      float childOffsetY);

    // Scroll layers. paintScrollLayer: if `elem` (whose box and clip the
    // caller has drawn and pushed, its content box at (x, y)) is a scroller
    // composited as a layer, records its content into the layer and the
    // layer break into the pass, and is true; false: paint the content in place.
    ScrollLayerHost* scrollLayerHost_ = nullptr;
    bool inScrollLayer_ = false;
    ScrollLayerStats scrollLayerStats_;
    // The scrollers this pass made layers of, for their pieces: where the
    // layer is (its break's quad), its scroll and window, whether its content
    // stood, and how many pieces it has had so far.
    struct PassScrollLayer {
        render::LayerQuad quad;
        float scrollX = 0, scrollY = 0;
        float winX = 0, winY = 0, winW = 0, winH = 0;
        bool reused = false;
        uint32_t pieces = 0;
    };
    std::unordered_map<const dom::Element*, PassScrollLayer> passScrollLayers_;
    // The layer a positioned box (or stacking context) of the walk is a piece
    // of: the nearest scroller above it this pass made a layer (null: none,
    // or the walk is already inside one).
    dom::Element* pieceScrollerOf(dom::Element* elem) const;
    // Clip snapping (setSnapScale). The origin is what the recording's
    // places are less the pass's: a scroll layer's content is recorded in its
    // content space, which the pass puts at its content box less its scroll,
    // and snapped where it lands. No snapping under a transform.
    float snapScale_ = 0.0f;
    float snapOriginX_ = 0.0f, snapOriginY_ = 0.0f;
    int transformDepth_ = 0;
    struct TransformScope {
        int& depth;
        bool on;
        TransformScope(int& d, bool o) : depth(d), on(o) { if (on) ++depth; }
        ~TransformScope() { if (on) --depth; }
        TransformScope(const TransformScope&) = delete;
        TransformScope& operator=(const TransformScope&) = delete;
    };
    // An overflow clip rect on whole device pixels (when snapping is on); a
    // span recording one notes it (spanSnapped_).
    void snapClip(float& x, float& y, float& w, float& h) const;
    // The span being recorded holds a snapped clip: reused only where it
    // lands on the same device-pixel grid (SpanKey::snapX). Carried up.
    mutable bool spanSnapped_ = false;
    // While a piece is recorded: its scroller, whose clips (and those above
    // it) the compositor applies, so the walk leaves them out (clipInPiece).
    const dom::Element* pieceScroller_ = nullptr;
    bool clipInPiece(const ClipRect& c) const;
    // Paint the walk's items in order; a run of them that is a piece of one
    // scroll layer is recorded into it, its layer break where the run is.
    struct WalkItem {
        dom::Element* elem;
        std::function<void()> paint;
    };
    void paintWalkItems(std::vector<WalkItem>& items);
    void paintScrollPiece(dom::Element* scroller, std::vector<WalkItem>& items, size_t from, size_t to);
    bool paintScrollLayer(dom::Element* elem, const htmlayout::css::ComputedStyle& style,
                          const htmlayout::layout::LayoutBox& box, bool visible, float x, float y, float bw,
                          float bh);
    ScrollLayerVeto scrollLayerVeto(dom::Element* elem, const htmlayout::css::ComputedStyle& style, float bw,
                                    float bh);

    // Span cache state for the current draw() pass (null: off).
    render::RecordingRenderer* recorder_ = nullptr;
    PaintCache* activeCache_ = nullptr;
    bool contextFlushed_ = false;
    // Per paint mode, so the base and promoted passes over one document (which
    // lift different boxes out of the walk) keep a cache each.
    std::unordered_map<const dom::Element*, std::unique_ptr<PaintCache>> caches_[3];
    PaintCacheStats cacheStats_;
    // The first reason the span being recorded cannot be reused (Count: none
    // yet). Painters note it (noteVolatile); it carries up to every span
    // around the one it was noted in.
    mutable SpanMiss spanVolatile_ = SpanMiss::Count;
    void noteVolatile(SpanMiss why) const {
        if (spanVolatile_ == SpanMiss::Count) spanVolatile_ = why;
    }
    // The span being recorded paints at a place of the viewport (SpanEntry::
    // pinned); it carries up like spanVolatile_.
    mutable bool spanPinned_ = false;
    // The span being recorded paints something its parent element decides
    // (a numbered list marker counts its siblings: SpanEntry::readsParent).
    // Not carried up: the spans around it are the parent's, stamped anyway.
    bool spanReadsParent_ = false;
    PaintCache* beginPaintCache(dom::Element* root);
    void prunePaintCache(PaintCache& cache);
    // Box shadows (outset, then inset over the background), background,
    // borders and outline of the border box (bx, by, bw, bh), in CSS paint
    // order. One painter for elements and pseudo-elements alike.
    void paintBoxDecorations(const PaintBox& pb, float bx, float by, float bw, float bh);
    void drawBackground(const PaintBox& pb, float x, float y, float w, float h);
    void drawBorders(const PaintBox& pb, float x, float y, float w, float h);
    // CSS border-image (Backgrounds-3 §6): draws the nine-slice over the
    // border area when border-image-source names a loaded raster image.
    // Returns true when border-image took over painting (normal border
    // painting must then be skipped); false → fall back to normal borders
    // (source absent, `none`, failed to load, SVG, or gradient).
    bool drawBorderImage(const PaintBox& pb, float x, float y, float w, float h);
    // <fieldset> paints its block-start border and background from the
    // vertical center of its <legend>, which straddles the border (HTML
    // rendering spec). Returns the downward shift of the painted top edge
    // (0 for anything that isn't a fieldset with an in-flow legend) and,
    // when gapX0/gapX1 are given, the legend's horizontal extent where the
    // top border is skipped.
    float fieldsetTopShift(dom::Element* elem, float x, float y,
                           float* gapX0 = nullptr, float* gapX1 = nullptr);
    void drawText(dom::Node* textNode, dom::Element* parent, float offsetX, float offsetY);
    void drawPseudo(dom::Element* host, const std::string& which,
                    float offsetX, float offsetY);

    // CSS 2.1 Appendix E painting (stacking contexts + z-index order).
    // buildStackingContextTree walks the DOM accumulating absolute offsets and
    // partitions descendants into the nearest SC ancestor's buckets.
    // paintStackingContext does the seven-step paint order recursively.
    std::unique_ptr<StackingContext> buildStackingContextTree(
        dom::Element* root, float scrollX, float scrollY);
    // withinPromoted tracks (PromotedOnly mode only) whether the recursion is
    // already inside a promoted SC subtree; false at the top. Ignored in All /
    // BaseSkipPromoted modes.
    void paintStackingContext(StackingContext* sc, bool withinPromoted = false, bool isRoot = false);

    // The top layer (dom::Document::topLayer, draw_traversal_top_layer.cpp).
    // buildStackingContextTree lifts each entry's subtree out of its ancestors
    // into a root-level stacking context here, keyed by element; draw() paints
    // them after the root SC in top-layer order, each over its ::backdrop.
    std::unordered_map<const dom::Element*, std::unique_ptr<StackingContext>> topLayerSCs_;
    void paintTopLayer(dom::Element* root);
    void paintBackdrop(dom::Element* elem);
    // Where a top-layer element's parent content box is in draw space: its
    // layout position, ignoring ancestor scroll, and the document scroll too
    // when the element is position:fixed. Also where any fixed box whose
    // containing block is the viewport is drawn from.
    void topLayerOffset(dom::Element* elem, float& offX, float& offY) const;

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

    // Paint-mode filter state. In the default (All + null set) neither is
    // consulted and paintStackingContext takes exactly the original single-pass
    // path. The engine sets these before each draw() and they persist across
    // draw() calls (draw() does not reset them).
    PaintMode paintMode_ = PaintMode::All;
    const std::unordered_set<dom::Element*>* promotedElements_ = nullptr;

public:
    // Color parsing helper (public for shared use by element controls)
    static bromath::Color parseColor(const std::string& color);
    static bool tryParseColor(const std::string& color, bromath::Color& out);
    // CSS text-transform applied to a string. Public because the replaced
    // controls draw their own text and have to apply it themselves — a
    // `<select>` under `text-transform: uppercase` has to render its label
    // uppercase like every other run of text on the page does.
    static std::string applyTextTransform(const std::string& text,
                                          const std::string& transform);

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

    // Legend gap for the fieldset border being painted by drawBorders: the
    // top border skips [fieldsetGapX0_, fieldsetGapX1_]. Only set while
    // drawBorders runs on a fieldset with an in-flow legend.
    bool fieldsetGapActive_ = false;
    float fieldsetGapX0_ = 0;
    float fieldsetGapX1_ = 0;

    // Root draw offset of the current draw() pass — the translation from
    // document space to the output surface (the app doc draws in content
    // space at (0, −scrollY); system panel docs at (0, 0) in window space).
    // Geometry computed via dom::absolute*Box() is document-space and must
    // add this to land in surface space (layer-break quads, control anchors).
    // Pure translation, so it composes with the ancestor-transform
    // projection: surface = project_doc(rect) + rootOffset.
    float rootOffsetX_ = 0;
    float rootOffsetY_ = 0;

    // paintImage's per-traversal memo: URL (and orientation) to the store's
    // request. Weak, so the store's memory bound decides what stays decoded.
    std::unordered_map<std::string, std::weak_ptr<render::ImageRequest>> imageRequests_;
    // The pixels for <img> `elem` as painted under its image-orientation.
    std::shared_ptr<const render::DecodedImage> elementPaintImage(dom::Element* elem, bool oriented);
    // The picture to paint for `img` now: itself, or an animation's current
    // frame — scheduling the next one only when `elem`'s box is on screen.
    std::shared_ptr<const render::DecodedImage> currentFrame(std::shared_ptr<const render::DecodedImage> img,
                                                             dom::Element* elem) const;
    // Whether `elem`'s border box meets the viewport and the active clip.
    bool boxOnScreen(dom::Element* elem) const;
    LayerBreakCallback layerBreakCb_;
    bool terminalLayers_ = false;
    bool shellClientWindows_ = false;
    const std::vector<ClientWindowSlot>* clientSlots_ = nullptr;
    std::vector<render::ClientWindowRef>* clientRefs_ = nullptr;
    uint32_t clientListId_ = 0;
    // Frame elements' stacking contexts, lifted out of the tree, by element.
    std::unordered_map<const dom::Element*, std::unique_ptr<StackingContext>> frameSCs_;
    // A frame's overlays ([data-window-overlay] inside it), lifted out of the
    // frame and painted over its window, by frame element, document order.
    std::unordered_map<const dom::Element*, std::vector<std::unique_ptr<StackingContext>>> frameOverlaySCs_;
    void emitClientWindows();

    // Running stack of axis-aligned overflow/scroll clip rects (each already
    // intersected with the one below it, so the top is the effective clip).
    // Mirrors the renderer_ clip saves contributed by ancestor `overflow`
    // boxes and the SC walker's pushClips, so a canvas/WebGL layer break can
    // report the clip the compositor must scissor to. Rounded corners and
    // clip-path polygons are not tracked (scissor can't express them).
    struct ClipBox { float x, y, w, h; };
    std::vector<ClipBox> clipRectStack_;
    void pushClipRect(float x, float y, float w, float h);
    void popClipRect() { if (!clipRectStack_.empty()) clipRectStack_.pop_back(); }
    // Effective clip for the current point. Returns false when unclipped.
    bool currentClipRect(float& x, float& y, float& w, float& h) const;
};

} // namespace bro::layout
