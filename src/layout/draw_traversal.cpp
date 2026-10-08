// The DrawTraversal driver: draw() builds the stacking-context tree (CSS 2.1
// Appendix E) and paints it in the seven-step order, handing each box to
// drawElementContent (draw_traversal_element.cpp). The per-feature painters
// live in the sibling draw_traversal_*.cpp files.

#include "layout/draw_traversal_internal.h"
#include "dom/document.h"
#include "dom/element_geometry.h"
#include "dom/node.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace bro::layout {

DrawTraversal::DrawTraversal(render::Renderer* renderer)
    : renderer_(renderer) {}

// ---------------------------------------------------------------------------
// Stacking-context helpers (CSS 2.1 Appendix E)
// ---------------------------------------------------------------------------
namespace {

const std::string& styleProp(const htmlayout::css::ComputedStyle& s,
                             const char* name, const std::string& fallback) {
    auto it = s.find(name);
    return (it != s.end()) ? it->second : fallback;
}

bool isPositioned(const htmlayout::css::ComputedStyle& s) {
    auto it = s.find("position");
    if (it == s.end()) return false;
    const std::string& p = it->second;
    return p == "relative" || p == "absolute" || p == "fixed" || p == "sticky";
}

using dom::establishesFixedContainingBlock;

// The containing block an absolutely positioned box is laid out and clipped
// against: its nearest positioned ancestor. Null means the initial containing
// block, i.e. nothing between it and the document clips it.
dom::Element* absoluteContainingBlock(dom::Element* elem) {
    for (auto* e = elem ? elem->parentElement() : nullptr; e; e = e->parentElement()) {
        if (isPositioned(e->computedStyle())) return e;
        // A transform (and friends) makes an element the containing block for
        // *all* positioned descendants, not just fixed ones.
        if (establishesFixedContainingBlock(e)) return e;
    }
    return nullptr;
}

// The containing block a fixed box is laid out and clipped against: the
// viewport, unless an ancestor took the job with a transform, a filter, a
// perspective, or a containment that implies one. Null means the viewport,
// which is the ordinary case and the reason a fixed box escapes every
// scrolling ancestor above it.
dom::Element* fixedContainingBlock(dom::Element* elem) {
    for (auto* e = elem ? elem->parentElement() : nullptr; e; e = e->parentElement())
        if (establishesFixedContainingBlock(e)) return e;
    return nullptr;
}

// Returns true if z-index is the literal keyword 'auto' (or absent).
// Out-parameter outZ holds the parsed integer (0 when auto/absent/invalid).
bool getZIndex(const htmlayout::css::ComputedStyle& s, int& outZ) {
    auto it = s.find("z-index");
    if (it == s.end() || it->second.empty() || it->second == "auto") {
        outZ = 0;
        return true; // is auto
    }
    char* end = nullptr;
    long v = std::strtol(it->second.c_str(), &end, 10);
    if (end == it->second.c_str()) {
        outZ = 0;
        return true;
    }
    outZ = static_cast<int>(v);
    return false;
}

// Returns true if this element creates a new stacking context.
// CSS 2.1 + commonly-implemented CSS3 triggers, restricted to what parity
// tests exercise. Does not yet trigger on: will-change, contain:layout/paint,
// container-type.
bool createsStackingContext(dom::Element* elem, bool isRoot) {
    if (isRoot) return true; // root element always creates SC
    auto& s = elem->computedStyle();

    // position: fixed / sticky → always SC
    auto posIt = s.find("position");
    std::string pos = (posIt != s.end()) ? posIt->second : "static";
    if (pos == "fixed" || pos == "sticky") return true;

    // position: relative/absolute with z-index != auto → SC
    if (pos == "relative" || pos == "absolute") {
        int z; bool isAuto = getZIndex(s, z);
        if (!isAuto) return true;
    }

    // opacity < 1
    auto opIt = s.find("opacity");
    if (opIt != s.end() && !opIt->second.empty()) {
        float op = std::strtof(opIt->second.c_str(), nullptr);
        if (op < 1.0f) return true;
    }

    // transform != none
    auto trIt = s.find("transform");
    if (trIt != s.end() && !trIt->second.empty() && trIt->second != "none")
        return true;

    // filter != none
    auto fIt = s.find("filter");
    if (fIt != s.end() && !fIt->second.empty() && fIt->second != "none")
        return true;

    // backdrop-filter != none (CSS Filter Effects 2): its backdrop is drawn
    // as the root's first paint, so the element has to be one.
    auto bfIt = s.find("backdrop-filter");
    if (bfIt != s.end() && !bfIt->second.empty() && bfIt->second != "none")
        return true;

    // isolation: isolate
    auto isoIt = s.find("isolation");
    if (isoIt != s.end() && isoIt->second == "isolate") return true;

    // mix-blend-mode != normal
    auto mbIt = s.find("mix-blend-mode");
    if (mbIt != s.end() && !mbIt->second.empty() && mbIt->second != "normal")
        return true;

    return false;
}

} // namespace

void DrawTraversal::pushClipRect(float x, float y, float w, float h) {
    // Intersect with the current effective clip so the new top stays the
    // running intersection (empty intersections collapse to zero area).
    if (!clipRectStack_.empty()) {
        const ClipBox& t = clipRectStack_.back();
        float nx = std::max(x, t.x);
        float ny = std::max(y, t.y);
        float nr = std::min(x + w, t.x + t.w);
        float nb = std::min(y + h, t.y + t.h);
        w = std::max(0.0f, nr - nx);
        h = std::max(0.0f, nb - ny);
        x = nx; y = ny;
    }
    clipRectStack_.push_back({x, y, w, h});
}

bool DrawTraversal::currentClipRect(float& x, float& y, float& w, float& h) const {
    if (clipRectStack_.empty()) return false;
    const ClipBox& t = clipRectStack_.back();
    x = t.x; y = t.y; w = t.w; h = t.h;
    return true;
}

void DrawTraversal::draw(dom::Element* root, float scrollX, float scrollY,
                         int viewportW, int viewportH, int viewportTop) {
    if (!root || !renderer_) return;
    viewportW_ = viewportW;
    viewportH_ = viewportH;
    viewportTop_ = viewportTop;
    // The root offset args double as the document→surface translation for
    // this pass. The app document draws into content-sized layer surfaces in
    // content space, so the engine passes (0, −scrollY); system panels pass
    // (0, 0) in window space. The engine-reserved inset never enters here —
    // the compositor applies it once when placing app layers.
    rootOffsetX_ = scrollX;
    rootOffsetY_ = scrollY;

    // skipSet_ is rebuilt per draw() — buildStackingContextTree inserts every
    // element that's an SC root or positioned-non-SC for the *current* frame.
    // Without clearing, entries from prior frames stick: an element that was
    // an SC last frame (e.g. .tile-inner with an active scale animation) but
    // isn't this frame (animation completed, transform fell back to none)
    // would remain in skipSet_ and be skipped by the in-flow walker even
    // though it's no longer in the SC tree — its content would vanish.
    skipSet_.clear();
    clipRectStack_.clear();

    // Build the stacking-context tree (CSS 2.1 Appendix E), then paint in the
    // seven-step order. Each SC root paints its own box first, then recurses
    // into negative-z children, then in-flow descendants (via the normal
    // walker, with positioned/SC descendants skipped via skipSet_), then
    // positioned-non-SC descendants and z:auto SC children in tree order,
    // then positive-z child SCs.
    auto rootSC = buildStackingContextTree(root, scrollX, scrollY);
    if (rootSC) paintStackingContext(rootSC.get(), false, true);
    // Then the top layer, above everything, each entry over its ::backdrop.
    if (rootSC) paintTopLayer(root);
    topLayerSCs_.clear();
    frameSCs_.clear();
    frameOverlaySCs_.clear();
}

void DrawTraversal::drawElement(dom::Element* elem, float offsetX, float offsetY) {
    if (!elem) return;
    drawElementContent(elem, offsetX, offsetY);
}

void DrawTraversal::drawNode(dom::Node* node, float offsetX, float offsetY) {
    if (!node) return;

    if (node->nodeType() == dom::NodeType::Element) {
        // CSS 2.1 Appendix E: subtrees rooted at a child stacking context or at
        // a positioned non-SC descendant are painted out-of-order by the SC
        // walker — skip them when reached via the normal in-flow walk so they
        // don't paint twice (or at the wrong z order).
        auto* el = static_cast<dom::Element*>(node);
        if (skipSet_.count(el)) return;
        drawElementContent(el, offsetX, offsetY);
    } else if (node->nodeType() == dom::NodeType::Text) {
        auto* parent = node->parentNode();
        if (parent && parent->nodeType() == dom::NodeType::Element) {
            drawText(node, static_cast<dom::Element*>(parent), offsetX, offsetY);
        }
    }
}

// ---------------------------------------------------------------------------
// CSS 2.1 Appendix E painting: stacking-context tree construction + paint walk
// ---------------------------------------------------------------------------

std::unique_ptr<StackingContext> DrawTraversal::buildStackingContextTree(
    dom::Element* root, float scrollX, float scrollY) {
    auto rootSC = std::make_unique<StackingContext>();
    rootSC->root = root;
    rootSC->offsetX = scrollX;
    rootSC->offsetY = scrollY;
    rootSC->zIndex = 0;
    rootSC->zIsAuto = true;
    rootSC->treeOrder = 0;

    int dfsCounter = 1;

    // The document's top layer, when this pass paints the whole document.
    std::unordered_set<const dom::Element*> topLayerSet;
    topLayerSCs_.clear();
    if (dom::Document* doc = root->document(); doc && doc->documentElement() == root) {
        for (const auto& e : doc->topLayer()) topLayerSet.insert(e.element);
    }
    // The shell's window frames, painted with their windows instead.
    std::unordered_set<const dom::Element*> frameSet;
    frameSCs_.clear();
    frameOverlaySCs_.clear();
    if (shellClientWindows_ && clientSlots_)
        for (const auto& s : *clientSlots_)
            if (s.frame) frameSet.insert(s.frame);

    // Compute the border-box clip rect contributed by `elem` if it has overflow
    // clipping on either axis. Mirrors the overflow clip drawElementContent
    // applies in the in-flow walk.
    auto elementClipRect = [](dom::Element* elem, float offX, float offY,
                              ClipRect& out) -> bool {
        auto& style = elem->computedStyle();
        std::string ox = getOverflowX(style);
        std::string oy = getOverflowY(style);
        if (!overflowAxisClips(ox) && !overflowAxisClips(oy)) return false;
        // Same viewport-propagation rule as the in-flow walker: the donor's
        // clip is the viewport's, so it contributes no rect here either.
        if (overflowBelongsToViewport(elem)) return false;
        auto& box = elem->layoutBox();
        out.bx = box.contentRect.x + offX - box.padding.left - box.border.left;
        out.by = box.contentRect.y + offY - box.padding.top - box.border.top;
        out.bw = box.fullWidth();
        out.bh = box.fullHeight();
        out.radii = getRadii(style, out.bw, out.bh);
        out.owner = elem;
        return true;
    };

    // Drop the ancestor clips an out-of-flow box escapes.
    //
    // CSS: an ancestor's `overflow` clips a descendant only when it is the
    // descendant's containing block or an ancestor of it. An absolutely
    // positioned box's containing block is its nearest positioned ancestor, and
    // a fixed one's is the viewport — so both routinely hang outside a
    // scrolling ancestor they happen to be nested in. That is not an edge case:
    // it is how every menu with a submenu is built, and clipping it means the
    // submenu never appears.
    //
    // The clip chain is outermost-first and every owner is an ancestor of
    // `elem`, so "keep the clips owned by the containing block or above" is a
    // prefix — but the set test is cheap and does not assume that.
    auto clipsFor = [](dom::Element* elem, const std::string& position,
                       const std::vector<ClipRect>& chain) -> std::vector<ClipRect> {
        if (position != "absolute" && position != "fixed") return chain;
        if (chain.empty()) return chain;

        dom::Element* cb = (position == "fixed") ? fixedContainingBlock(elem)
                                                 : absoluteContainingBlock(elem);
        // No containing block below the viewport: nothing in the chain clips.
        if (!cb) return {};

        std::unordered_set<const dom::Element*> allowed;
        for (auto* e = cb; e; e = e->parentElement()) allowed.insert(e);

        std::vector<ClipRect> kept;
        kept.reserve(chain.size());
        for (const auto& c : chain)
            if (c.owner && allowed.count(c.owner)) kept.push_back(c);
        return kept;
    };

    // Recursive collector. `currentSC` is the nearest SC ancestor whose buckets
    // descendants populate. (offsetX, offsetY) is the absolute draw offset for
    // `elem`. `ancestorClips` is the clip chain accumulated from the current SC
    // root (exclusive) down to (but not including) `elem` — outermost-first.
    std::function<void(dom::Element*, StackingContext*, float, float,
                       const std::vector<ClipRect>&)> visit;
    visit = [&](dom::Element* elem, StackingContext* currentSC,
                float offX, float offY,
                const std::vector<ClipRect>& ancestorClips) {
        if (!elem) return;
        auto& style = elem->computedStyle();

        // display:none → don't paint, don't descend
        auto dispIt = style.find("display");
        if (dispIt != style.end() && dispIt->second == "none") return;

        // Flow-collapsed content (closed <details> body) never paints —
        // don't descend either, so positioned/SC descendants can't leak
        // into the stacking-context buckets.
        auto fcIt = style.find("-x-flow-collapse");
        if (fcIt != style.end() && fcIt->second == "collapse") return;
        // Clamped away by an ancestor's line-clamp: the same, for the same
        // reason (a positioned descendant must not leak into an SC bucket).
        if (elem->layoutBox().clampHidden) return;

        // A top-layer element leaves every ancestor stacking context and clip:
        // it becomes a root-level SC of its own, collected into topLayerSCs_
        // and painted after the whole document by paintTopLayer. It sits where
        // layout put it — no ancestor scroll offset — and a fixed one ignores
        // the document scroll too (draw space is viewport space).
        static const std::vector<ClipRect> kNoClips;
        StackingContext topHolder;
        const bool inTopLayer = elem != root && topLayerSet.count(elem) != 0;
        // A window frame likewise leaves every ancestor stacking context and
        // clip; emitClientWindows paints it under its window.
        StackingContext frameHolder;
        const bool isFrame = elem != root && !inTopLayer && frameSet.count(elem) != 0;
        if (isFrame) currentSC = &frameHolder;
        // An overlay inside a frame leaves the frame: emitClientWindows
        // paints it over the frame's window.
        const dom::Element* overlayFrame = nullptr;
        if (!isFrame && !inTopLayer && !frameSet.empty() && elem != root &&
            elem->hasAttribute("data-window-overlay")) {
            for (dom::Element* p = elem->parentElement(); p; p = p->parentElement())
                if (frameSet.count(p)) {
                    overlayFrame = p;
                    break;
                }
        }
        const bool isOverlay = overlayFrame != nullptr;
        StackingContext overlayHolder;
        if (isOverlay) currentSC = &overlayHolder;
        if (inTopLayer) {
            topLayerOffset(elem, offX, offY);
            currentSC = &topHolder;
        } else if (elem != root) {
            // A fixed box against the viewport does not scroll: not with its
            // scrolling ancestors, not with the document. It sits where
            // layout put it, in draw space (viewport space) — the offset a
            // fixed top-layer box gets.
            auto posIt = style.find("position");
            if (posIt != style.end() && posIt->second == "fixed" &&
                !fixedContainingBlock(elem))
                topLayerOffset(elem, offX, offY);
        }
        const std::vector<ClipRect>& inClips = (inTopLayer || isFrame || isOverlay) ? kNoClips : ancestorClips;

        // Compute child offset using the same logic as drawElementContent
        auto& box = elem->layoutBox();
        float x = box.contentRect.x + offX;
        float y = box.contentRect.y + offY;
        float maxST = std::max(0.0f, box.naturalHeight - box.contentRect.height);
        float scrollTop = std::clamp(elem->scrollTopValue(), 0.0f, maxST);
        float childOffX = x;
        float childOffY = y - scrollTop;

        bool isThisRoot = (elem == root);
        bool isSC = inTopLayer || isFrame || isOverlay || createsStackingContext(elem, isThisRoot);
        bool positioned = isPositioned(style);

        // Clips this element is actually subject to: an out-of-flow box drops
        // the ones belonging to ancestors below its containing block.
        static const std::string kStatic = "static";
        const std::string& elemPosition = styleProp(style, "position", kStatic);
        std::vector<ClipRect> ownClips = clipsFor(elem, elemPosition, inClips);

        StackingContext* descendantSC = currentSC;

        if (isSC && !isThisRoot) {
            // Create a child SC entry on currentSC.
            auto sc = std::make_unique<StackingContext>();
            sc->root = elem;
            sc->offsetX = offX;
            sc->offsetY = offY;
            sc->zIsAuto = getZIndex(style, sc->zIndex);
            sc->treeOrder = dfsCounter++;
            sc->ancestorClips = ownClips;
            descendantSC = sc.get();
            currentSC->children.push_back(std::move(sc));
            // The SC root itself is painted by paintStackingContext (its own
            // drawElementContent call), and the legacy walker will skip it.
            skipSet_.insert(elem);
        } else if (positioned && !isThisRoot) {
            // Positioned but does not create an SC (z-index:auto on
            // relative/absolute). Goes into step 6 of nearest SC.
            StackingContext::PositionedEntry pe;
            pe.elem = elem;
            pe.offsetX = offX;
            pe.offsetY = offY;
            pe.tieBreaker = dfsCounter++;
            pe.ancestorClips = ownClips;
            currentSC->positionedNonSC.push_back(std::move(pe));
            skipSet_.insert(elem);
        }

        // Build the clip chain to pass to descendants. At an SC boundary the
        // chain resets — descendants of a child SC paint inside that SC's own
        // paintStackingContext call, which separately re-applies the SC's
        // captured ancestorClips. Within an SC's subtree the chain propagates
        // and accumulates each ancestor's overflow clip.
        std::vector<ClipRect> childClips;
        ClipRect selfClip;
        bool selfClips = elementClipRect(elem, offX, offY, selfClip);
        if (isSC && !isThisRoot) {
            // SC root's own clip still applies to its step-6 descendants
            // (drawElementContent's step-1 save/restore is balanced and gone
            // by the time step 6 runs).
            if (selfClips) childClips.push_back(selfClip);
        } else {
            // Descendants inherit the chain this element is itself subject to:
            // once a positioned box has escaped an ancestor's clip, everything
            // inside it has escaped it too.
            childClips = ownClips;
            if (selfClips) childClips.push_back(selfClip);
        }

        // Recurse into composed children. If this element became an SC root,
        // descendants accumulate into IT; otherwise they accumulate into the
        // same currentSC. For positioned non-SC, descendants ALSO go to the
        // same currentSC (positioning doesn't open a new SC), but we still
        // skip the element in the normal walker — the positioned-entry paint
        // path will descend into it through drawElementContent.
        for (auto* child : elem->composedChildNodes()) {
            if (child && child->nodeType() == dom::NodeType::Element) {
                visit(static_cast<dom::Element*>(child),
                      descendantSC, childOffX, childOffY, childClips);
            }
        }

        if (inTopLayer && !topHolder.children.empty()) {
            topLayerSCs_[elem] = std::move(topHolder.children.back());
        }
        if (isFrame && !frameHolder.children.empty()) {
            frameSCs_[elem] = std::move(frameHolder.children.back());
        }
        if (isOverlay && !overlayHolder.children.empty()) {
            frameOverlaySCs_[overlayFrame].push_back(std::move(overlayHolder.children.back()));
        }
    };

    // Root: descendants belong to rootSC.
    auto& rootStyle = root->computedStyle();
    auto rDisp = rootStyle.find("display");
    if (rDisp != rootStyle.end() && rDisp->second == "none") return nullptr;

    // The root itself is the SC root (painted by paintStackingContext); we
    // recurse into its children directly so the root isn't double-skipped.
    auto& rbox = root->layoutBox();
    float rx = rbox.contentRect.x + scrollX;
    float ry = rbox.contentRect.y + scrollY;
    float rMaxST = std::max(0.0f, rbox.naturalHeight - rbox.contentRect.height);
    float rScrollTop = std::clamp(root->scrollTopValue(), 0.0f, rMaxST);
    float rChildOffX = rx;
    float rChildOffY = ry - rScrollTop;
    std::vector<ClipRect> rootClips;
    {
        ClipRect cr;
        if (elementClipRect(root, scrollX, scrollY, cr)) rootClips.push_back(cr);
    }
    for (auto* child : root->composedChildNodes()) {
        if (child && child->nodeType() == dom::NodeType::Element) {
            visit(static_cast<dom::Element*>(child),
                  rootSC.get(), rChildOffX, rChildOffY, rootClips);
        }
    }

    return rootSC;
}

void DrawTraversal::paintStackingContext(StackingContext* sc, bool withinPromoted, bool isRoot) {
    if (!sc || !sc->root) return;

    // Paint-mode filter. In the default PaintMode::All (or with no promoted set
    // registered) none of this runs and the remainder is the original single-pass
    // walk, byte-for-byte. Promoted elements are always SC roots, so the only
    // subtree that can be a promoted target is one reached through here.
    if (paintMode_ != PaintMode::All && promotedElements_) {
        bool isPromoted = promotedElements_->count(sc->root) != 0;
        if (paintMode_ == PaintMode::BaseSkipPromoted && isPromoted) {
            // Skip this SC and its ENTIRE subtree (like the canvas early-return),
            // leaving a transparent hole; keep painting siblings / other SCs.
            return;
        }
        if (paintMode_ == PaintMode::PromotedOnly && isPromoted) {
            // Entered a promoted SC root: paint everything from here down (its
            // own transform/opacity wrappers included, so the promoted subtree
            // renders with its current animated transform baked in).
            withinPromoted = true;
        }
    }
    // PromotedOnly, not yet inside a promoted subtree: suppress THIS SC's own
    // painting (wrappers, box/background/borders/text, positioned-non-SC
    // content) but still recurse into child/positioned SCs to reach deeper
    // promoted roots. In All / BaseSkipPromoted this is always false.
    const bool suppressSelf =
        (paintMode_ == PaintMode::PromotedOnly && promotedElements_ && !withinPromoted);

    // The SC root's transform/opacity/filter must wrap ALL of its descendants'
    // painting — not just step 1 (the in-flow walk). Positioned descendants
    // and nested stacking contexts are part of the same SC subtree and must
    // inherit the SC root's transform. drawElementContent applies these on its
    // own around step 1 only; we need them active for steps 2–7 as well.
    //
    // Strategy: pre-apply the SC root's transform/opacity/filter here, tell
    // drawElementContent to skip its own application, and tear them down after
    // all steps complete.
    auto& rootStyle = sc->root->computedStyle();
    auto& rootBox = sc->root->layoutBox();
    float rbx = rootBox.contentRect.x + sc->offsetX -
                rootBox.padding.left - rootBox.border.left;
    float rby = rootBox.contentRect.y + sc->offsetY -
                rootBox.padding.top - rootBox.border.top;
    float rbw = rootBox.fullWidth();
    float rbh = rootBox.fullHeight();

    // mix-blend-mode composites the ENTIRE stacking context against the
    // backdrop. Push it as the outermost layer (before transform/opacity/
    // filter) so the blended group includes all of the SC's painting, and tear
    // it down last.
    bool wrappedBlend = false;
    bool wrappedTransform = false;
    bool wrappedOpacity = false;
    bool wrappedFilter = false;
    // suppressSelf (PromotedOnly, above a promoted root) skips every wrapper +
    // the SC's own content paint; recursion into children still happens below.
    if (!suppressSelf) {
    {
        auto mbIt = rootStyle.find("mix-blend-mode");
        if (mbIt != rootStyle.end() && !mbIt->second.empty() && mbIt->second != "normal") {
            render::BlendMode bm = parseBlendMode(mbIt->second);
            if (bm != render::BlendMode::Normal) {
                wrappedBlend = true;
                renderer_->saveLayerWithBlend(bm);
            }
        }
    }

    {
        auto trIt = rootStyle.find("transform");
        bool hasT = (trIt != rootStyle.end() && !trIt->second.empty()
                     && trIt->second != "none");
        float persp = parentPerspective(sc->root);
        bool wants3D = (persp > 0) || (hasT && transformHas3D(trIt->second));

        if (wants3D) {
            float pbx = 0, pby = 0, pbw = 0, pbh = 0;
            const htmlayout::css::ComputedStyle* perspStyle = nullptr;
            if (auto* parent = sc->root->layoutParent()) {
                auto& pb = parent->layoutBox();
                // sc->offsetX/Y is the parent's content-area absolute origin.
                pbx = sc->offsetX - pb.padding.left - pb.border.left;
                pby = sc->offsetY - pb.padding.top - pb.border.top;
                pbw = pb.fullWidth();
                pbh = pb.fullHeight();
                perspStyle = &parent->computedStyle();
            }
            bool is3D = false;
            auto m4 = buildElementTransform4x4(rootStyle, rbx, rby, rbw, rbh,
                                               persp, pbx, pby, pbw, pbh,
                                               perspStyle, is3D);
            if (!m4.isIdentity()) {
                wrappedTransform = true;
                renderer_->save();
                if (is3D)
                    renderer_->concat4x4(m4.m);
                else {
                    auto m2 = m4.to2D();
                    renderer_->concat(m2.a, m2.b, m2.c, m2.d, m2.e, m2.f);
                }
            }
        } else if (hasT) {
            auto mat = htmlayout::css::parseTransform(trIt->second, rbw, rbh);
            if (!mat.isIdentity()) {
                wrappedTransform = true;
                float ox, oy;
                auto toIt = rootStyle.find("transform-origin");
                std::string_view originVal = (toIt != rootStyle.end())
                    ? std::string_view(toIt->second) : std::string_view();
                htmlayout::css::parseTransformOrigin(originVal, rbw, rbh, ox, oy);
                renderer_->save();
                renderer_->translate(rbx + ox, rby + oy);
                renderer_->concat(mat.a, mat.b, mat.c, mat.d, mat.e, mat.f);
                renderer_->translate(-(rbx + ox), -(rby + oy));
            }
        }
    }
    // backdrop-filter: the root's backdrop is what is on the surface under
    // it now, inside its transform but before its opacity/filter layers open
    // (inside them the surface would be an empty layer). Opacity mixes the
    // filtered backdrop over the plain one, as it would the whole element.
    {
        auto bfIt = rootStyle.find("backdrop-filter");
        if (bfIt != rootStyle.end() && !bfIt->second.empty() && bfIt->second != "none") {
            auto filters = parseCSSFilter(bfIt->second, styleCurrentColor(rootStyle),
                shadowLengthContext(sc->root, rootStyle, viewportW_, viewportH_));
            if (!filters.empty()) {
                float opacity = 1.0f;
                auto opIt = rootStyle.find("opacity");
                if (opIt != rootStyle.end())
                    opacity = std::clamp(std::strtof(opIt->second.c_str(), nullptr), 0.0f, 1.0f);
                renderer_->drawBackdropFilter(filters, rbx, rby, rbw, rbh,
                                              getRadii(rootStyle, rbw, rbh), opacity);
            }
        }
    }
    {
        auto opIt = rootStyle.find("opacity");
        if (opIt != rootStyle.end()) {
            float opacity = std::clamp(std::strtof(opIt->second.c_str(), nullptr),
                                       0.0f, 1.0f);
            if (opacity < 1.0f) {
                wrappedOpacity = true;
                renderer_->saveLayerAlpha(static_cast<uint8_t>(opacity * 255));
            }
        }
    }
    {
        auto fIt = rootStyle.find("filter");
        if (fIt != rootStyle.end() && !fIt->second.empty() && fIt->second != "none") {
            auto filters = parseCSSFilter(fIt->second, styleCurrentColor(rootStyle),
                shadowLengthContext(sc->root, rootStyle, viewportW_, viewportH_));
            if (!filters.empty()) {
                wrappedFilter = true;
                renderer_->saveLayerWithFilter(filters,
                    rbx - 50, rby - 50, rbw + 100, rbh + 100);
            }
        }
    }
    } // if (!suppressSelf)
    bool didWrap = wrappedBlend || wrappedTransform || wrappedOpacity || wrappedFilter;
    if (didWrap) scRootSkipWrap_.insert(sc->root);

    // Step 1: paint the SC root itself — its background, borders, and in-flow
    // non-positioned non-SC descendants — via the normal walker. The walker
    // consults skipSet_ to avoid descending into SC roots and positioned
    // non-SC descendants (they paint separately below).
    // In PromotedOnly above a promoted root this is suppressed (recursion into
    // children below still runs to reach deeper promoted subtrees).
    if (!suppressSelf)
        drawElementContent(sc->root, sc->offsetX, sc->offsetY);

    // Step 2: child SCs with z-index < 0, sorted by zIndex then tree order.
    std::vector<StackingContext*> negSCs, autoSCs, posSCs;
    for (auto& c : sc->children) {
        if (!c->zIsAuto && c->zIndex < 0) negSCs.push_back(c.get());
        else if (c->zIsAuto || c->zIndex == 0) autoSCs.push_back(c.get());
        else posSCs.push_back(c.get());
    }
    std::sort(negSCs.begin(), negSCs.end(), [](auto* a, auto* b) {
        if (a->zIndex != b->zIndex) return a->zIndex < b->zIndex;
        return a->treeOrder < b->treeOrder;
    });
    auto pushClips = [this](const std::vector<ClipRect>& clips) {
        for (auto& c : clips) {
            renderer_->save();
            if (!c.radii.isZero())
                renderer_->setClipRRect(c.bx, c.by, c.bw, c.bh, c.radii);
            else
                renderer_->setClip(c.bx, c.by, c.bw, c.bh);
            // Mirror onto the layer-break clip stack so a canvas/WebGL break in
            // an out-of-line (positioned / nested-SC) subtree still scissors to
            // the ancestor overflow clip the compositor would otherwise ignore.
            pushClipRect(c.bx, c.by, c.bw, c.bh);
        }
    };
    auto popClips = [this](const std::vector<ClipRect>& clips) {
        for (size_t i = 0; i < clips.size(); ++i) { renderer_->restore(); popClipRect(); }
    };
    for (auto* c : negSCs) {
        pushClips(c->ancestorClips);
        paintStackingContext(c, withinPromoted);
        popClips(c->ancestorClips);
    }

    // Steps 3-5 are folded into step 1 above (in-flow descendants paint via
    // drawElementContent in tree order — this gives correct ordering for the
    // common case; perfect block-then-float-then-inline separation would need
    // a deeper layout-tree split that bro doesn't currently materialize).

    // Step 6: positioned non-SC descendants AND child SCs with z-index:auto/0,
    // interleaved in tree order.
    struct Step6Item {
        int tieBreaker;
        std::function<void()> paint;
    };
    std::vector<Step6Item> step6;
    // positioned-non-SC entries are never promoted (promoted elements are SC
    // roots), so PromotedOnly above a promoted root suppresses them; any nested
    // promoted SC inside such an entry's subtree is a child SC of THIS sc and is
    // reached via the autoSCs/posSCs recursion below regardless.
    if (!suppressSelf) {
        for (auto& pe : sc->positionedNonSC) {
            step6.push_back({pe.tieBreaker, [this, &pe, &pushClips, &popClips]() {
                pushClips(pe.ancestorClips);
                drawElementContent(pe.elem, pe.offsetX, pe.offsetY);
                popClips(pe.ancestorClips);
            }});
        }
    }
    for (auto* c : autoSCs) {
        step6.push_back({c->treeOrder, [this, c, withinPromoted, &pushClips, &popClips]() {
            pushClips(c->ancestorClips);
            paintStackingContext(c, withinPromoted);
            popClips(c->ancestorClips);
        }});
    }
    std::sort(step6.begin(), step6.end(), [](const Step6Item& a, const Step6Item& b) {
        return a.tieBreaker < b.tieBreaker;
    });
    for (auto& item : step6) item.paint();

    // Step 7: child SCs with positive z-index.
    std::sort(posSCs.begin(), posSCs.end(), [](auto* a, auto* b) {
        if (a->zIndex != b->zIndex) return a->zIndex < b->zIndex;
        return a->treeOrder < b->treeOrder;
    });
    bool emittedWindows = false;
    for (auto* c : posSCs) {
        if (isRoot && shellClientWindows_ && !emittedWindows && c->zIndex >= 1000) {
            emittedWindows = true;
            emitClientWindows();
        }
        pushClips(c->ancestorClips);
        paintStackingContext(c, withinPromoted);
        popClips(c->ancestorClips);
    }
    if (isRoot && shellClientWindows_ && !emittedWindows) emitClientWindows();

    if (didWrap) {
        scRootSkipWrap_.erase(sc->root);
        if (wrappedFilter) renderer_->restore();
        if (wrappedOpacity) renderer_->restore();
        if (wrappedTransform) renderer_->restore();
        if (wrappedBlend) renderer_->restore();
    }
}

// The shell host's client windows, where the shell's desktop level ends:
// runs of windows separated by the frames the shell draws, each frame painted
// into the HTML just below its window. The runs go to clientRefs_; each break
// names its slice of them.
void DrawTraversal::emitClientWindows() {
    if (!layerBreakCb_) return;
    std::vector<render::ClientWindowRef> scratch;
    std::vector<render::ClientWindowRef>& refs = clientRefs_ ? *clientRefs_ : scratch;
    size_t first = refs.size();
    bool firstRun = true;
    auto flush = [&](uint32_t parts) {
        LayerBreak lb;
        render::ClientWindowsLayerSource src;
        src.list = clientListId_;
        src.first = static_cast<uint32_t>(first);
        src.count = static_cast<uint32_t>(refs.size() - first);
        src.parts = parts | (firstRun ? render::kClientLayersBelow : 0u);
        lb.source = src;
        lb.quad.w = static_cast<float>(viewportW_);
        lb.quad.h = static_cast<float>(viewportH_);
        layerBreakCb_(lb);
        first = refs.size();
        firstRun = false;
    };
    if (clientSlots_) {
        for (const auto& slot : *clientSlots_) {
            auto it = slot.frame ? frameSCs_.find(slot.frame) : frameSCs_.end();
            if (it == frameSCs_.end() || !it->second) {
                refs.push_back(render::ClientWindowRef{slot.windowId, false, 0.0f, 0.0f});
                continue;
            }
            // The windows below go under this frame.
            if (refs.size() > first || firstRun) flush(0);
            StackingContext* sc = it->second.get();
            paintStackingContext(sc, false);
            // The client origin: the frame's border box (where layout put it
            // this pass) plus the inset the frame was sized with.
            const auto& box = slot.frame->layoutBox();
            const float bx = box.contentRect.x + sc->offsetX - box.padding.left - box.border.left;
            const float by = box.contentRect.y + sc->offsetY - box.padding.top - box.border.top;
            refs.push_back(render::ClientWindowRef{slot.windowId, true, bx + slot.insetLeft, by + slot.insetTop});
            // The frame's overlays go over its window: end the run here.
            auto ov = frameOverlaySCs_.find(slot.frame);
            if (ov != frameOverlaySCs_.end() && !ov->second.empty()) {
                flush(0);
                for (auto& osc : ov->second)
                    if (osc) paintStackingContext(osc.get(), false);
            }
        }
    }
    flush(render::kClientLayersAbove);
}

} // namespace bro::layout
