// drawElementContent: paints one element box — transform / opacity / filter /
// clip-path wrappers, box shadows, background, borders, outline, column rules,
// list markers, the overflow clip, layer breaks, children and pseudo-elements,
// and replaced content (form controls, <video>, <img>).

#include "layout/draw_traversal_internal.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "layout/el_svg.h"
#include "layout/el_video.h"
#include "layout/el_terminal.h"
#include "layout/el_remote_view.h"
#include "layout/formatting_context.h"
#include "layout/line_clamp.h"
#include "canvas/canvas_scene.h"
#include "dom/element_geometry.h"
#include "dom/element_scroll.h"
#include "dom/node.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace bro::layout {

void DrawTraversal::drawElementContent(dom::Element* elem, float offsetX, float offsetY) {
    if (!elem) return;

    auto& style = elem->computedStyle();

    // Check display:none
    auto dispIt = style.find("display");
    if (dispIt != style.end() && dispIt->second == "none") return;

    // Flow-collapsed content (closed <details> body — UA -x-flow-collapse):
    // laid out with real geometry but never painted, whole subtree.
    auto fcIt = style.find("-x-flow-collapse");
    if (fcIt != style.end() && fcIt->second == "collapse") return;

    // Past a line-clamp container's clamp point (htmlayout line_clamp.h):
    // laid out, not hit-testable, and not painted — background, border and
    // subtree alike. The clamped text needs no check; its runs are gone.
    if (elem->layoutBox().clampHidden) return;

    // Fully transparent (a stacking-context root's was checked, and its
    // wrappers applied, by paintStackingContext): nothing of it shows.
    if (!scRootSkipWrap_.count(elem) && opacityHidesAll(style)) return;

    // Check visibility:hidden (still occupies space but not drawn)
    bool visible = true;
    auto visIt = style.find("visibility");
    if (visIt != style.end() && visIt->second == "hidden") visible = false;

    auto& box = elem->layoutBox();
    float x = box.contentRect.x + offsetX;
    float y = box.contentRect.y + offsetY;
    float w = box.contentRect.width;
    float h = box.contentRect.height;

    // Border box for background/border drawing
    float bx = x - box.padding.left - box.border.left;
    float by = y - box.padding.top - box.border.top;
    float bw = box.fullWidth();
    float bh = box.fullHeight();

    // CSS Transform / opacity / filter: wrap entire element drawing.
    // For stacking-context roots, paintStackingContext has already wrapped
    // these around the full SC subtree (so positioned descendants inherit the
    // transform); skip re-applying them here in that case.
    bool skipWrap = scRootSkipWrap_.count(elem) > 0;

    // Off-screen culling. When an ancestor clips its overflow (a scroll
    // container), any in-flow descendant whose border box lies entirely outside
    // that clip paints nothing, so skip recording it and its whole subtree. This
    // is what keeps the base re-record O(visible) instead of O(total DOM): a long
    // list (e.g. a 300-row model picker) re-records on every hover/scroll frame,
    // and without this every row is recorded even when only ~15 are on screen —
    // the source of hover/scroll lag on big lists.
    //
    // Kept conservative to never drop visible paint:
    //   * only inside an active clip (clipRectStack_ non-empty) — the clip
    //     guarantees in-flow descendants can't paint beyond it;
    //   * only in-flow, untransformed boxes. absolute/fixed can escape the clip,
    //     sticky is repositioned at paint time, and a transform can map the box
    //     back on screen — layoutBox already bakes in relative/sticky offsets;
    //   * overscan the clip by a slack margin so a just-off-screen element's
    //     box-shadow/outline spilling toward the viewport still records.
    if (!clipRectStack_.empty() && !skipWrap) {
        const ClipBox& clip = clipRectStack_.back();
        constexpr float kCullSlack = 256.0f;  // covers typical shadow/outline spill
        bool outside = by + bh < clip.y - kCullSlack ||
                       by      > clip.y + clip.h + kCullSlack ||
                       bx + bw < clip.x - kCullSlack ||
                       bx      > clip.x + clip.w + kCullSlack;
        if (outside) {
            auto posIt = style.find("position");
            bool inFlow = posIt == style.end() ||
                          posIt->second == "static" ||
                          posIt->second == "relative";
            auto trIt = style.find("transform");
            bool noTransform = trIt == style.end() || trIt->second.empty() ||
                               trIt->second == "none";
            if (inFlow && noTransform) return;
        }
    }

    bool hasTransform = false;
    if (!skipWrap) {
        auto trIt = style.find("transform");
        bool hasT = (trIt != style.end() && !trIt->second.empty()
                     && trIt->second != "none");
        float persp = parentPerspective(elem);
        bool wants3D = (persp > 0) || (hasT && transformHas3D(trIt->second));

        if (wants3D) {
            // 4x4 path (3D transforms or ancestor perspective).
            // Parent border box in absolute coords:
            float pbx = 0, pby = 0, pbw = 0, pbh = 0;
            const htmlayout::css::ComputedStyle* perspStyle = nullptr;
            if (auto* parent = elem->layoutParent()) {
                auto& pb = parent->layoutBox();
                pbx = offsetX - pb.padding.left - pb.border.left;
                pby = offsetY - pb.padding.top - pb.border.top;
                pbw = pb.fullWidth();
                pbh = pb.fullHeight();
                perspStyle = &parent->computedStyle();
            }
            bool is3D = false;
            auto m4 = buildElementTransform4x4(style, bx, by, bw, bh,
                                               persp, pbx, pby, pbw, pbh,
                                               perspStyle, is3D);
            if (!m4.isIdentity()) {
                hasTransform = true;
                renderer_->save();
                if (is3D) {
                    renderer_->concat4x4(m4.m);
                } else {
                    auto m2 = m4.to2D();
                    renderer_->concat(m2.a, m2.b, m2.c, m2.d, m2.e, m2.f);
                }
            }
        } else if (hasT) {
            auto mat = htmlayout::css::parseTransform(trIt->second, bw, bh);
            if (!mat.isIdentity()) {
                hasTransform = true;
                float ox, oy;
                auto toIt = style.find("transform-origin");
                std::string_view originVal =
                    (toIt != style.end()) ? std::string_view(toIt->second)
                                          : std::string_view();
                htmlayout::css::parseTransformOrigin(originVal, bw, bh, ox, oy);
                // Apply: translate to origin, concat matrix, translate back
                renderer_->save();
                renderer_->translate(bx + ox, by + oy);
                renderer_->concat(mat.a, mat.b, mat.c, mat.d, mat.e, mat.f);
                renderer_->translate(-(bx + ox), -(by + oy));
            }
        }
    }

    // Opacity: wrap entire element in a layer
    bool hasOpacity = false;
    if (!skipWrap) {
        auto opIt = style.find("opacity");
        if (opIt != style.end()) {
            float opacity = std::clamp(std::strtof(opIt->second.c_str(), nullptr), 0.0f, 1.0f);
            if (opacity < 1.0f) {
                hasOpacity = true;
                renderer_->saveLayerAlpha(static_cast<uint8_t>(opacity * 255));
            }
        }
    }

    // CSS filter: wrap element drawing in a filter layer
    bool hasFilter = false;
    if (!skipWrap) {
        auto fIt = style.find("filter");
        if (fIt != style.end() && !fIt->second.empty() && fIt->second != "none") {
            auto filters = parseCSSFilter(fIt->second, styleCurrentColor(style),
                shadowLengthContext(elem, style, viewportW_, viewportH_));
            if (!filters.empty()) {
                hasFilter = true;
                // Use a generous bounds that includes blur/shadow overflow
                renderer_->saveLayerWithFilter(filters,
                    bx - 50, by - 50, bw + 100, bh + 100);
            }
        }
    }

    // CSS clip-path: clip the entire element (background, border, content,
    // descendants) to the specified shape. Restored after children paint.
    bool hasClipPath = false;
    auto cpIt = style.find("clip-path");
    if (cpIt != style.end() && !cpIt->second.empty() && cpIt->second != "none") {
        float cpFontSize = 16.0f;
        auto fsIt = style.find("font-size");
        if (fsIt != style.end())
            cpFontSize = htmlayout::layout::resolveLength(fsIt->second, 16.0f, 16.0f);
        auto pts = parseClipPathPolygon(cpIt->second, bw, bh, cpFontSize);
        if (!pts.empty()) {
            for (auto& pt : pts) { pt.x += bx; pt.y += by; }
            hasClipPath = true;
            renderer_->save();
            renderer_->setClipPolygon(pts);
        }
    }

    if (visible) {
        // Box shadows. CSS paint order:
        //   1. outset shadows (drawn before background, behind the element)
        //   2. background
        //   3. inset shadows (drawn over background, under content/border)
        // Within outset/inset groups, the first shadow in the list paints on
        // top of later ones, so we draw in reverse list order.
        auto bsIt = style.find("box-shadow");
        std::vector<CssShadow> shadows;
        render::Radii shadowRadii = {{0, 0, 0, 0}, {0, 0, 0, 0}};
        bool hasShadows = (bsIt != style.end() && !bsIt->second.empty() && bsIt->second != "none");
        if (hasShadows) {
            shadowRadii = getRadii(style, bw, bh);
            // A shadow with no colour is currentcolor.
            shadows = parseCssShadowList(bsIt->second, styleCurrentColor(style), 4,
                                         shadowLengthContext(elem, style, viewportW_, viewportH_));
        }

        auto drawShadows = [&](bool wantInset) {
            for (int si = static_cast<int>(shadows.size()) - 1; si >= 0; --si) {
                const CssShadow& s = shadows[si];
                if (s.inset != wantInset) continue;
                renderer_->drawBoxShadowRadii(bx, by, bw, bh, shadowRadii,
                                              s.dx, s.dy, s.blur, s.spread, s.color, s.inset);
            }
        };

        // Outset shadows first (behind background).
        if (hasShadows) drawShadows(false);

        // For html/body elements, background covers the entire viewport (CSS2.1 spec).
        // viewportTop_ offsets for engine-reserved insets (e.g. menu bar).
        std::string tag = elem->tagName();
        if ((tag == "html" || tag == "HTML" || tag == "body" || tag == "BODY") &&
            viewportW_ > 0 && viewportH_ > 0) {
            drawBackground(elem, 0, static_cast<float>(viewportTop_),
                           static_cast<float>(viewportW_), static_cast<float>(viewportH_));
        } else {
            // A fieldset's background starts at its painted border-box top
            // (the legend's vertical center), not the layout box top.
            float fsShift = fieldsetTopShift(elem, bx, by);
            drawBackground(elem, bx, by + fsShift, bw, bh - fsShift);
        }

        // Inset shadows after background (so they're visible on top of it).
        if (hasShadows) drawShadows(true);

        // Draw borders (skipped here for border-collapse tables — they
        // repaint after children so the table border wins on the gridline)
        if (!isCollapsedTable(elem)) {
            drawBorders(elem, bx, by, bw, bh);
        }

        // Draw outline (outside the border box)
        auto olwIt = style.find("outline-width");
        auto olsIt = style.find("outline-style");
        if (olwIt != style.end() && olsIt != style.end() && olsIt->second != "none") {
            float olw = parseLengthPx(olwIt->second);
            if (olw > 0) {
                bromath::Color olc = cfromColor8({0, 0, 0, 255});
                auto olcIt = style.find("outline-color");
                if (olcIt != style.end()) tryParseColor(olcIt->second, olc);
                float olOff = 0;
                auto oloIt = style.find("outline-offset");
                if (oloIt != style.end()) olOff = parseLengthPx(oloIt->second);
                float ox = bx - olw - olOff;
                float oy = by - olw - olOff;
                float ow = bw + 2 * (olw + olOff);
                float oh = bh + 2 * (olw + olOff);
                // Top, Bottom, Left, Right as filled rects
                renderer_->fillRect(ox, oy, ow, olw, olc);
                renderer_->fillRect(ox, oy + oh - olw, ow, olw, olc);
                renderer_->fillRect(ox, oy + olw, olw, oh - 2*olw, olc);
                renderer_->fillRect(ox + ow - olw, oy + olw, olw, oh - 2*olw, olc);
            }
        }

        // Column rules for multicol containers: one rule centered in each
        // column gap, spanning the content height. Geometry mirrors the
        // layout's column computation (block.cpp multicol path) so rules land
        // exactly between the laid-out columns. All rule styles render solid.
        {
            auto ccIt = style.find("column-count");
            auto cwIt = style.find("column-width");
            bool hasCount = ccIt != style.end() && !ccIt->second.empty() &&
                            ccIt->second != "auto";
            bool hasWidth = cwIt != style.end() && !cwIt->second.empty() &&
                            cwIt->second != "auto";
            auto crsIt = style.find("column-rule-style");
            if ((hasCount || hasWidth) && crsIt != style.end() &&
                crsIt->second != "none" && crsIt->second != "hidden" &&
                w > 0 && h > 0) {
                float rw = 3.0f;  // medium
                auto crwIt = style.find("column-rule-width");
                if (crwIt != style.end()) {
                    const std::string& v = crwIt->second;
                    if (v == "thin") rw = 1.0f;
                    else if (v == "medium") rw = 3.0f;
                    else if (v == "thick") rw = 5.0f;
                    else rw = parseLengthPx(v);
                }
                if (rw > 0) {
                    // column-rule-color defaults to currentColor.
                    bromath::Color rc = cfromColor8({0, 0, 0, 255});
                    auto crcIt = style.find("column-rule-color");
                    if (crcIt == style.end()) crcIt = style.find("color");
                    if (crcIt != style.end()) tryParseColor(crcIt->second, rc);

                    float gap = 0.0f;
                    auto cgIt = style.find("column-gap");
                    if (cgIt != style.end() && !cgIt->second.empty() &&
                        cgIt->second != "normal") {
                        gap = parseLengthPx(cgIt->second, w);
                    }
                    int count = 1;
                    if (hasCount) {
                        count = std::max(1, std::atoi(ccIt->second.c_str()));
                    } else {
                        float colW = parseLengthPx(cwIt->second, w);
                        if (colW > 0) {
                            count = std::max(1, static_cast<int>(
                                (w + gap) / (colW + gap)));
                        }
                    }
                    float colW = (w - gap * (count - 1)) / count;
                    if (colW < 0) colW = 0;
                    for (int i = 1; i < count; ++i) {
                        float gapLeft = x + colW * i + gap * (i - 1);
                        float rx = gapLeft + gap * 0.5f - rw * 0.5f;
                        renderer_->fillRect(rx, y, rw, h, rc);
                    }
                }
            }
        }

        // Draw list marker for display:list-item boxes (<li>, <summary>…).
        // Outside markers hang left of the content box, aligned to the first
        // line's baseline. Inside markers are inline content at the start of
        // the first line: layout reserves their inline size (htmlayout's
        // insideMarkerInlineSize — Blink geometry: symbol box + 1em margin
        // for symbolic bullets, text + one space for ordinals) and the
        // painter fills the reserved box here.
        {
            auto dispIt = style.find("display");
            auto lstIt = style.find("list-style-type");
            std::string listType = (lstIt != style.end()) ? lstIt->second : "disc";
            auto lspIt = style.find("list-style-position");
            bool outside = (lspIt == style.end() || lspIt->second != "inside");
            if (dispIt != style.end() && dispIt->second == "list-item" &&
                listType != "none") {
                bromath::Color mc = cfromColor8({0, 0, 0, 255});
                auto mcIt = style.find("color");
                if (mcIt != style.end()) tryParseColor(mcIt->second, mc);

                render::FontRef font = getFontRef(elem);
                // First-line baseline: content top + font ascent (half-leading
                // at UA line heights is sub-pixel; close enough for markers).
                auto am = renderer_->measureText("0", font);
                float baselineY = by + am.ascent;
                float gap = 7.0f;   // Blink's marker padding

                if (listType == "disclosure-open" ||
                    listType == "disclosure-closed") {
                    // <summary> disclosure triangle. Blink: symbol box is
                    // 0.66em (DisclosureSymbolSize) with a 0.4em end margin;
                    // closed points right, open points down.
                    float fs = 16.0f;
                    auto fsIt = style.find("font-size");
                    if (fsIt != style.end()) {
                        float v = parseLengthPx(fsIt->second);
                        if (v > 0) fs = v;
                    }
                    float s = 0.66f * fs;
                    float x0 = outside ? (bx - 0.4f * fs - s) : bx;
                    float top = baselineY - am.ascent * 0.35f - s * 0.5f;
                    bromath::Color none = cfromColor8({0, 0, 0, 0});
                    if (listType == "disclosure-closed") {
                        render::PointF pts[3] = {
                            {x0, top}, {x0 + s, top + s * 0.5f}, {x0, top + s}};
                        renderer_->drawPolygon(pts, mc, none, 0);
                    } else {
                        render::PointF pts[3] = {
                            {x0, top}, {x0 + s, top}, {x0 + s * 0.5f, top + s}};
                        renderer_->drawPolygon(pts, mc, none, 0);
                    }
                } else if (listType == "disc" || listType == "circle" ||
                    listType == "square") {
                    // Bullet centered on roughly half the x-height above the
                    // baseline. Outside: right edge gap px left of the content
                    // box. Inside: at the content-box left, within the space
                    // layout reserved.
                    float r = 3.0f;
                    float cy = baselineY - am.ascent * 0.30f;
                    float cx = outside ? (bx - gap - r) : (bx + r);
                    if (listType == "disc") {
                        renderer_->drawCircle(cx, cy, r, mc, mc, 0);
                    } else if (listType == "circle") {
                        bromath::Color none = cfromColor8({0, 0, 0, 0});
                        renderer_->drawCircle(cx, cy, r, none, mc, 1.0f);
                    } else {
                        renderer_->fillRect(cx - r, cy - r, 2 * r, 2 * r, mc);
                    }
                } else {
                    // Ordinal marker: position among list-item siblings,
                    // honoring <ol start> and <li value>.
                    auto isListItem = [](dom::Node* n) {
                        if (n->nodeType() != dom::NodeType::Element) return false;
                        auto* e = static_cast<dom::Element*>(n);
                        auto& st = e->computedStyle();
                        auto dIt = st.find("display");
                        return dIt != st.end() && dIt->second == "list-item";
                    };
                    int idx = 1;
                    auto* parent = elem->parentNode();
                    if (parent && parent->nodeType() == dom::NodeType::Element) {
                        auto* pe = static_cast<dom::Element*>(parent);
                        const std::string& startAttr = pe->getAttribute("start");
                        if (!startAttr.empty()) idx = std::atoi(startAttr.c_str());
                        for (auto* sib : parent->childNodes()) {
                            if (!isListItem(sib)) continue;
                            auto* se = static_cast<dom::Element*>(sib);
                            const std::string& valAttr = se->getAttribute("value");
                            if (!valAttr.empty()) idx = std::atoi(valAttr.c_str());
                            if (sib == elem) break;
                            ++idx;
                        }
                    }

                    auto toAlpha = [](int n) {
                        std::string s;
                        while (n > 0) {
                            int rem = (n - 1) % 26;
                            s.insert(s.begin(), static_cast<char>('a' + rem));
                            n = (n - 1) / 26;
                        }
                        return s.empty() ? std::string("a") : s;
                    };
                    auto toRoman = [](int n) {
                        if (n <= 0 || n >= 4000) return std::to_string(n);
                        static const int vals[] = {1000, 900, 500, 400, 100, 90,
                                                   50, 40, 10, 9, 5, 4, 1};
                        static const char* syms[] = {"m", "cm", "d", "cd", "c",
                                                     "xc", "l", "xl", "x", "ix",
                                                     "v", "iv", "i"};
                        std::string s;
                        for (int i = 0; i < 13; ++i)
                            while (n >= vals[i]) { s += syms[i]; n -= vals[i]; }
                        return s;
                    };
                    auto toUpper = [](std::string s) {
                        for (auto& ch : s)
                            ch = static_cast<char>(std::toupper(
                                static_cast<unsigned char>(ch)));
                        return s;
                    };

                    std::string text;
                    if (listType == "decimal") {
                        text = std::to_string(idx);
                    } else if (listType == "decimal-leading-zero") {
                        text = (idx >= 0 && idx < 10 ? "0" : "") + std::to_string(idx);
                    } else if (listType == "lower-alpha" || listType == "lower-latin") {
                        text = toAlpha(idx);
                    } else if (listType == "upper-alpha" || listType == "upper-latin") {
                        text = toUpper(toAlpha(idx));
                    } else if (listType == "lower-roman") {
                        text = toRoman(idx);
                    } else if (listType == "upper-roman") {
                        text = toUpper(toRoman(idx));
                    } else {
                        text = std::to_string(idx);
                    }
                    text += ".";
                    auto tm = renderer_->measureText(text, font);
                    float tx = outside ? (bx - gap - tm.width) : bx;
                    renderer_->drawText(text, tx, baselineY, font, mc);
                }
            }
        }
    }

    // Check for overflow clipping. Per CSS spec, when one axis is non-visible
    // and the other is visible, the visible axis is treated as auto — i.e. both
    // axes effectively clip. So clip if EITHER axis is non-visible.
    bool needsClip = false;
    std::string overflowY = getOverflowY(style);
    std::string overflowX = getOverflowX(style);
    const auto& axisClips = overflowAxisClips;
    if ((axisClips(overflowX) || axisClips(overflowY)) &&
        !overflowBelongsToViewport(elem)) {
        needsClip = true;
        renderer_->save();
        render::Radii clipRadii = getRadii(style, bw, bh);
        if (!clipRadii.isZero())
            renderer_->setClipRRect(bx, by, bw, bh, clipRadii);
        else
            renderer_->setClip(bx, by, bw, bh);
        // Track the rect so a canvas/WebGL layer break in this subtree can
        // report the clip the compositor must scissor to (rounded corners are
        // approximated by their bounding box).
        pushClipRect(bx, by, bw, bh);
    }

    // SVG elements render their own children via the SVG pipeline — skip DOM traversal
    if (elem->svgControl()) {
        // Draw the SVG control, then return (no child traversal)
        if (visible) {
            elem->svgControl()->draw(renderer_, elem, box, offsetX, offsetY);
        }
        if (needsClip) {
            renderer_->restore();
            popClipRect();
        }
        if (hasClipPath) renderer_->restore();
        if (hasFilter) renderer_->restore();
        if (hasOpacity) renderer_->restore();
        if (hasTransform) renderer_->restore();
        return;
    }

    // Canvas/WebGL/scene/iframe elements: a layer break, so the compositor
    // interleaves their separately composited layers with the HTML in
    // document order. Their content isn't drawn through renderer_, so it
    // picks up neither the CTM concat above nor the Skia clip stack: the quad
    // is the content box projected through the element's own ancestor
    // transforms (the math getBoundingClientRect() uses, so a zoomed/panned
    // ancestor positions it), plus the pass's root offset (document scroll;
    // the compositor shifts HTML and quads together by the engine inset), and
    // carries the active overflow/scroll clip for the compositor to re-apply.
    if ((elem->sceneGraph() || elem->canvasScene() || elem->webglContext() || elem->iframeDoc()) && visible) {
        LayerBreak lb;
        lb.element = elem;
        const auto box = dom::absoluteContentBox(elem);
        lb.quad.x = box.x + rootOffsetX_;
        lb.quad.y = box.y + rootOffsetY_;
        lb.quad.w = box.width;
        lb.quad.h = box.height;
        if (!currentClipRect(lb.quad.clipX, lb.quad.clipY, lb.quad.clipW, lb.quad.clipH))
            lb.quad.clipW = lb.quad.clipH = -1.0f;
        auto emit = [&](render::LayerSource source) {
            if (!layerBreakCb_) return;
            lb.source = source;
            layerBreakCb_(lb);
        };
        // A scene element also has a canvasScene (its 2D ShapeNode/SpriteNode
        // content), composited over the 3D image.
        if (elem->sceneGraph()) {
            if (elem->sceneLayerReady()) emit(render::SceneLayerSource{elem->nodeId()});
            if (elem->canvasScene())
                emit(render::CanvasLayerSource{static_cast<canvas::CanvasScene*>(elem->canvasScene())->sceneId()});
        } else if (elem->canvasScene()) {
            emit(render::CanvasLayerSource{static_cast<canvas::CanvasScene*>(elem->canvasScene())->sceneId()});
        } else if (elem->webglContext()) {
            emit(render::WebGLLayerSource{elem->nodeId()});
        } else {
            emit(render::IframeLayerSource{elem->iframeDocId()});
        }
        if (needsClip) { renderer_->restore(); popClipRect(); }
        if (hasClipPath) renderer_->restore();
        if (hasFilter) renderer_->restore();
        if (hasOpacity) renderer_->restore();
        if (hasTransform) renderer_->restore();
        return;
    }

    // Children's offset is the parent's absolute content position
    // (so child positions, which are relative to parent content area, become absolute)
    // Scroll offsets clamped to the valid range — JS may have set them before
    // layout updated.
    float childOffsetX = x - dom::clampedScrollLeftOf(elem);
    float childOffsetY = y - dom::clampedScrollTopOf(elem);

    // ::before pseudo content (drawn before children)
    if (visible) drawPseudo(elem, "before", childOffsetX, childOffsetY);

    // Draw composed children (shadow DOM + slot replacement).
    // A <textarea> renders its value through ElTextarea (below); its DOM text
    // children are the *source* of that value, not separate flow content.
    // Painting them here too double-draws the text — once in the element's
    // computed color via this walk, once again by the control — which reads
    // as the placeholder/value "ghosting" behind the real text.
    if (!elem->textareaControl()) {
        // CSS2.1 Appendix E: non-positioned floats paint above the
        // backgrounds/borders of in-flow block-level siblings. Defer floated
        // children to a second pass so a later sibling's background can't
        // cover a float that precedes it in the DOM.
        std::vector<dom::Node*> floatedChildren;
        for (auto* child : elem->composedChildNodes()) {
            if (child && child->nodeType() == dom::NodeType::Element) {
                auto& cs = static_cast<dom::Element*>(child)->computedStyle();
                auto fIt = cs.find("float");
                if (fIt != cs.end() && fIt->second != "none" &&
                    !fIt->second.empty()) {
                    floatedChildren.push_back(child);
                    continue;
                }
            }
            drawNode(child, childOffsetX, childOffsetY);
        }
        for (auto* child : floatedChildren)
            drawNode(child, childOffsetX, childOffsetY);
    }

    // ::after pseudo content (drawn after children)
    if (visible) drawPseudo(elem, "after", childOffsetX, childOffsetY);

    // A line-clamp container whose kept lines hold no text node (images,
    // inline-blocks, or an empty last line) carries the ellipsis as its own
    // run, in its content coordinates and its font (htmlayout line_clamp.h).
    if (visible && box.textTruncated) {
        for (const auto& run : box.textRuns) {
            if (run.srcStart != htmlayout::layout::kContainerEllipsisSrc ||
                run.text.empty())
                continue;
            render::FontRef fontRef = getFontRef(elem);
            float ascent = renderer_->measureText("", fontRef).ascent;
            bromath::Color color = cfromColor8({0, 0, 0, 255});
            auto cIt = style.find("color");
            if (cIt != style.end()) tryParseColor(cIt->second, color);
            renderer_->drawText(run.text, childOffsetX + run.x,
                                childOffsetY + run.y + ascent, fontRef, color);
        }
    }

    // Draw replaced element content (input, textarea, select, svg)
    if (visible) {
        auto* inputCtrl = elem->inputControl();
        if (inputCtrl) {
            inputCtrl->draw(renderer_, box, style, offsetX, offsetY,
                            rootOffsetX_, rootOffsetY_);
        }
        auto* textareaCtrl = elem->textareaControl();
        if (textareaCtrl) {
            textareaCtrl->draw(renderer_, box, style, offsetX, offsetY,
                               rootOffsetX_, rootOffsetY_);
        }
        auto* selectCtrl = elem->selectControl();
        if (selectCtrl) {
            selectCtrl->draw(renderer_, box, style, offsetX, offsetY,
                             rootOffsetX_, rootOffsetY_);
        }
        auto* videoCtrl = elem->videoControl();
        if (videoCtrl) {
            // object-fit, same as <img>. A player sizes its viewport to the
            // window and lets the picture letterbox inside it; without this
            // every video is stretched to whatever shape the box happens to
            // be.
            std::string videoFit = "fill";
            if (auto ofIt = style.find("object-fit"); ofIt != style.end() && !ofIt->second.empty())
                videoFit = ofIt->second;
            videoCtrl->draw(renderer_, elem, box, offsetX, offsetY, videoFit);
        }
        if (auto* termCtrl = elem->terminalControl()) {
            // Its own layer in the app document, so a busy terminal redraws
            // only itself and the page around it is never re-recorded for it
            // (el_terminal_layer.cpp); inline everywhere else.
            if (terminalLayers_ && layerBreakCb_) {
                LayerBreak lb;
                lb.element = elem;
                const auto cb = dom::absoluteContentBox(elem);
                lb.quad.x = cb.x + rootOffsetX_;
                lb.quad.y = cb.y + rootOffsetY_;
                lb.quad.w = cb.width;
                lb.quad.h = cb.height;
                if (!currentClipRect(lb.quad.clipX, lb.quad.clipY, lb.quad.clipW, lb.quad.clipH))
                    lb.quad.clipW = lb.quad.clipH = -1.0f;
                lb.source = render::TerminalLayerSource{termCtrl->layerId()};
                if (lb.quad.w > 0 && lb.quad.h > 0) layerBreakCb_(lb);
            } else {
                termCtrl->draw(renderer_, box.contentRect.x + offsetX, box.contentRect.y + offsetY,
                               box.contentRect.width, box.contentRect.height);
            }
        }
        if (auto* remoteCtrl = elem->remoteViewControl()) {
            // A remote screen is its own layer, the decoded picture sampled
            // where it is (the engine's RemoteViewHost); a pass with no
            // compositor behind it shows only the element's background.
            if (layerBreakCb_) {
                LayerBreak lb;
                lb.element = elem;
                const auto cb = dom::absoluteContentBox(elem);
                lb.quad.x = cb.x + rootOffsetX_;
                lb.quad.y = cb.y + rootOffsetY_;
                lb.quad.w = cb.width;
                lb.quad.h = cb.height;
                if (!currentClipRect(lb.quad.clipX, lb.quad.clipY, lb.quad.clipW, lb.quad.clipH))
                    lb.quad.clipW = lb.quad.clipH = -1.0f;
                lb.source = render::RemoteViewLayerSource{remoteCtrl->viewId()};
                if (lb.quad.w > 0 && lb.quad.h > 0) layerBreakCb_(lb);
            }
        }
        // <img> replaced content. Layout already sized the box via
        // intrinsicSize() in layout_node_adapter; here we paint the raster
        // bytes (or SVG markup) into the content rect.
        const std::string& tag = elem->tagName();
        if (tag == "img" || tag == "IMG") {
            std::string src = elem->getAttribute("src");
            if (!src.empty()) {
                loadImage(src, basePath_);
                auto it = imageCache_.find(src);
                if (it != imageCache_.end() && !it->second.data.empty()) {
                    float ix = box.contentRect.x + offsetX;
                    float iy = box.contentRect.y + offsetY;
                    float iw = box.contentRect.width;
                    float ih = box.contentRect.height;
                    if (iw > 0 && ih > 0) {
                        // CSS object-fit / object-position. The default (fill)
                        // stretches the image to the content box. cover/contain/
                        // none/scale-down preserve the intrinsic aspect ratio and
                        // position the result via object-position (default
                        // center). cover/none can overflow the content box, so we
                        // clip to it.
                        float dx = ix, dy = iy, dw = iw, dh = ih;
                        float imgW = static_cast<float>(it->second.width);
                        float imgH = static_cast<float>(it->second.height);
                        std::string fit = "fill";
                        if (auto ofIt = style.find("object-fit"); ofIt != style.end() && !ofIt->second.empty())
                            fit = ofIt->second;
                        bool needClip = false;
                        if (fit != "fill" && imgW > 0 && imgH > 0) {
                            float fitScale = 1.0f;
                            if (fit == "contain") {
                                fitScale = std::min(iw / imgW, ih / imgH);
                            } else if (fit == "cover") {
                                fitScale = std::max(iw / imgW, ih / imgH);
                            } else if (fit == "none") {
                                fitScale = 1.0f;
                            } else if (fit == "scale-down") {
                                fitScale = std::min(1.0f, std::min(iw / imgW, ih / imgH));
                            }
                            dw = imgW * fitScale;
                            dh = imgH * fitScale;

                            // object-position: place the scaled box within the
                            // content box. Default "50% 50%". Each axis fraction
                            // f maps the f-point of the image to the f-point of
                            // the box: offset = (boxSize - drawSize) * f.
                            float fx = 0.5f, fy = 0.5f;
                            if (auto opIt = style.find("object-position");
                                opIt != style.end() && !opIt->second.empty()) {
                                std::istringstream iss(opIt->second);
                                std::string t1, t2;
                                iss >> t1; iss >> t2;
                                auto axisFrac = [](const std::string& tok, bool isX, float def) -> float {
                                    if (tok.empty()) return def;
                                    if (tok == "left")   return isX ? 0.0f : def;
                                    if (tok == "right")  return isX ? 1.0f : def;
                                    if (tok == "top")    return isX ? def : 0.0f;
                                    if (tok == "bottom") return isX ? def : 1.0f;
                                    if (tok == "center") return 0.5f;
                                    if (tok.back() == '%')
                                        return std::strtof(tok.c_str(), nullptr) / 100.0f;
                                    return def;  // px offsets unsupported → default
                                };
                                fx = axisFrac(t1, true, 0.5f);
                                fy = axisFrac(t2.empty() ? t1 : t2, false, 0.5f);
                            }
                            dx = ix + (iw - dw) * fx;
                            dy = iy + (ih - dh) * fy;
                            needClip = (dw > iw + 0.5f) || (dh > ih + 0.5f) ||
                                       dx < ix - 0.5f || dy < iy - 0.5f;
                        }

                        if (needClip) { renderer_->save(); renderer_->setClip(ix, iy, iw, ih); }
                        if (it->second.isSvg) {
                            // Recorded; replayer re-parses and draws the SVG markup at the fitted rect.
                            renderer_->drawSvgMarkup(
                                reinterpret_cast<const char*>(it->second.data.data()),
                                it->second.data.size(),
                                dx, dy, dw, dh);
                        } else {
                            renderer_->drawImage(it->second.data.data(),
                                                 it->second.data.size(),
                                                 dx, dy, dw, dh,
                                                 it->second.id);
                        }
                        if (needClip) renderer_->restore();
                    }
                }
            }
        }
    }

    // For tables in border-collapse mode, repaint the table's outer border
    // AFTER cells so it wins on the gridline (per CSS 17.6.2 paint order:
    // cells, then rows, then row-groups, then cols, then col-groups, then
    // table — last in the list paints on top). drawBorders() handles the
    // collapsed-mode centering when isCollapsedTable() is true.
    if (visible && isCollapsedTable(elem)) {
        drawBorders(elem, bx, by, bw, bh);
    }

    if (needsClip) {
        renderer_->restore();
        popClipRect();
    }
    if (hasClipPath) renderer_->restore();
    if (hasFilter) renderer_->restore();
    if (hasOpacity) renderer_->restore();
    if (hasTransform) renderer_->restore();
}

} // namespace bro::layout
