#include "engine/engine.h"
#include "engine/input_common.h"
#include "engine/overflow.h"
#include "engine/overlay.h"
#include "engine/replaced_elements.h"
#include "engine/settings.h"
#include "engine/iframe.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "dom/text_node.h"
#include "layout/control_text.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/layout_node_adapter.h"
#include "layout/selection_geometry.h"
#include "layout/skia_text_metrics.h"
#include "platform/sdl_window.h"

#if BRO_WITH_3D
#include "scene/scene_graph.h"
#include "scene/html_node.h"
#endif

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace bro::engine {

bool Engine::iframeHandleMouseMove(dom::Element* frameEl, float docX, float docY,
                                   float movementX, float movementY, int mod) {
    if (!frameEl || !frameEl->iframeDoc()) return false;
    auto* dp = static_cast<IframeDoc*>(frameEl->iframeDoc());
    if (!dp->document) return false;
    dom::AbsoluteRect box = dom::absoluteContentBox(frameEl);
    float lx = docX - box.x, ly = docY - box.y;
    dom::Element* sub = iframeHitTest(dp, lx, ly);

    if (sub != dp->hoveredElement) {
        if (dp->hoveredElement) dp->hoveredElement->markDirty();
        if (sub) sub->markDirty();
        dp->hoveredElement = sub;
        uiDirty_ = true;
    }
    if (sub) {
        dom::MouseEvent moveEvt("mousemove");
        populateMouseEvent(moveEvt, lx, ly, 0, pressedButtons_, movementX, movementY,
                           0.0f, mod, 0.0f);
        applyMouseOffset(moveEvt, sub);
        dom::dispatchDomEvent(sub, moveEvt);
    }
    return true;
}

void Engine::handleMouseMove(float x, float y, float xrel, float yrel) {
    if (lockedElement_.held() && !lockedElement_.get()) exitPointerLock();

    if (dom::Element* locked = lockedElement_.get()) {
        if (document_) {
            int mod = currentModState();
            dom::MouseEvent moveEvt("mousemove", true, true);
            populateMouseEvent(moveEvt, lockedMouseX_, lockedMouseY_, -1,
                               pressedButtons_, xrel, yrel, scrollY_, mod,
                               static_cast<float>(contentTop()));
            applyMouseOffset(moveEvt, locked);
            dispatchPointerAlias("pointermove", locked, moveEvt);
            dispatchEvent(locked, moveEvt);
        }
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;

    bool overlayActive = overlayMgr_.hasActive();
    if (overlayActive && overlayMgr_.handleMouseMove(x, overlayMouseY(y))) {
        markAppBaseDirty();
    }

    if (auto* dragEl = controlDragElement_.get()) {
        float cx = x;
        float cy = controlDragIsPanel_ ? y : y - static_cast<float>(contentTop());
        if (auto* input = getElInput(dragEl)) {
            input->caretToPoint(cx, cy, /*extend=*/true);
        } else if (auto* ta = getElTextarea(dragEl)) {
            ta->caretToPoint(cx, cy, /*extend=*/true);
        }
        if (controlDragIsPanel_) systemDirty_ = true;
        else markAppBaseDirty();

        if (!controlDragIsPanel_ && document_) {
            if (dom::Element* target = hitTest(docX, docY)) {
                int mod = currentModState();
                dom::MouseEvent moveEvt("mousemove", true, true);
                populateMouseEvent(moveEvt, x, y, -1, pressedButtons_,
                                   xrel, yrel, scrollY_, mod,
                                   static_cast<float>(contentTop()));
                applyMouseOffset(moveEvt, target);
                dispatchPointerAlias("pointermove", target, moveEvt);
                dispatchEvent(target, moveEvt);
            }
        }
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    if (isSystemVisible() && !elementScrollbar_.isDragging() &&
        systemHandleMouseMove(x, y)) {
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    if (inspector_.pickerMode && inspector_.visible) {
        dom::Element* hit = hitTest(docX, docY);
        if (hit != inspector_.pickerHover) {
            inspector_.pickerHover = hit;
            markAppBaseDirty();
        }
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

#if BRO_WITH_3D
    if (gizmoHandleMouseMove(docX, docY)) {
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }
#endif

    dom::TextNode* selAnchor = selectionAnchorNode_.get();
    if (selectionDragging_ && document_ && textMetrics_ && selAnchor) {
        if (!selectionPastThreshold_) {
            const float kThreshold = 4.0f;
            float dx = docX - selectionPressX_;
            float dy = docY - selectionPressY_;
            if (dx*dx + dy*dy < kThreshold * kThreshold) {
            } else {
                selectionPastThreshold_ = true;
            }
        }
        auto* dragHost = editableHostOf(selAnchor);
        auto hit = selectionPastThreshold_
            ? layout::hitTestText(document_.get(), docX, docY, *textMetrics_,
                                  dragHost)
            : layout::TextHit{};
        if (hit.textNode && document_->ownsNode(hit.textNode) &&
            document_->ownsNode(selAnchor)) {
            auto* sel = document_->selection();
            dom::Range probe;
            probe.setStart(selAnchor, selectionAnchorOffset_);
            bool backward = probe.comparePoint(hit.textNode, hit.srcOffset) < 0;
            if (backward) {
                sel->setRange(hit.textNode, hit.srcOffset,
                              selAnchor, selectionAnchorOffset_,
                              dom::Selection::Backward);
            } else {
                sel->setRange(selAnchor, selectionAnchorOffset_,
                              hit.textNode, hit.srcOffset,
                              dom::Selection::Forward);
            }
            markAppBaseDirty();
        }
    }

    if (viewportScrollbar_.isDragging()) {
        float ct = static_cast<float>(contentTop());
        float vh = static_cast<float>(contentHeight());
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(viewportWidth_) - vs.width - vs.margin,
            ct, vh, documentHeight_, vh, scrollY_);
        float maxScroll = std::max(0.0f, documentHeight_ - vh);
        scrollY_ = std::clamp(
            viewportScrollbar_.updateDrag(y, documentHeight_, vh, m),
            0.0f, maxScroll);
        uiDirty_ = true;
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    if (elementScrollbar_.isDragging() && scrollbarDragTarget_) {
        auto* elem = scrollbarDragTarget_.get();
        float viewH = elem->layoutBox().contentRect.height;
        float maxST = maxScrollTop(elem);
        float contentH = viewH + maxST;

        auto& lbox = elem->layoutBox();
        float bh = lbox.fullHeight();
        auto m = elementScrollbar_.layout(0, 0, bh, contentH, viewH,
            elem->scrollTopValue());
        float dragY = scrollbarDragSystemDoc_
            ? y : y - static_cast<float>(contentTop());
        float newScroll = elementScrollbar_.updateDrag(dragY, contentH, viewH, m);
        float prev = elem->scrollTopValue();
        float clamped = std::clamp(newScroll, 0.0f, maxST);
        elem->setScrollTopValue(clamped);
        if (clamped != prev) {
            if (scrollbarDragSystemDoc_) {
                if (scrollbarDragSystemDoc_->document)
                    scrollbarDragSystemDoc_->document->markDirty();
                systemDirty_ = true;
            } else {
                dispatchScrollEvent(elem);
                markAppBaseDirty();
            }
        }
        uiDirty_ = true;
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    {
        float ct = static_cast<float>(contentTop());
        float vh = static_cast<float>(contentHeight());
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(viewportWidth_) - vs.width - vs.margin,
            ct, vh, documentHeight_, vh, scrollY_);
        bool wasHovered = viewportScrollbar_.isHovered();
        viewportScrollbar_.setHovered(viewportScrollbar_.thumbHitTest(x, y, m));
        if (wasHovered != viewportScrollbar_.isHovered()) uiDirty_ = true;
    }

    if (document_ && document_->documentElement()) {
        float cyEl = y - static_cast<float>(contentTop());
        ScrollbarMetrics em;
        dom::Element* hitElem = findElementScrollbarHit(
            document_->documentElement(), x, cyEl,
            0.0f, -scrollY_, elementScrollbar_, em);
        dom::Element* prevHovered = scrollbarHoveredElement_.get();
        if (hitElem && elementScrollbar_.thumbHitTest(x, cyEl, em)) {
            scrollbarHoveredElement_.assign(document_.get(), hitElem);
        } else {
            scrollbarHoveredElement_.reset();
        }
        if (prevHovered != scrollbarHoveredElement_.get()) uiDirty_ = true;
    }

    if (document_) {
        auto* activeEl = document_->activeElement();
        auto* rangeInput = getElInput(activeEl);
        if (rangeInput && rangeInput->isDragging()) {
            auto dp = rangeInput->lastDrawPos();
            float thumbR = layout::ElInput::rangeThumbRadius(dp.h);
            float trackStart = dp.x + thumbR;
            float trackEnd = dp.x + dp.w - thumbR;
            float pct = (trackEnd > trackStart) ?
                std::clamp((x - trackStart) / (trackEnd - trackStart), 0.0f, 1.0f) : 0.0f;
            float mn = rangeInput->rangeMin(), mx = rangeInput->rangeMax();
            float val = mn + pct * (mx - mn);
            float step = rangeInput->rangeStep();
            if (step > 0) {
                val = mn + std::round((val - mn) / step) * step;
                val = std::clamp(val, mn, mx);
            }
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", static_cast<double>(val));
            activeEl->setAttribute("value", buf);
            dispatchInputEvent(activeEl);
            uiDirty_ = true;
        }
    }

    if (document_ && !overlayActive) {
        dom::Element* target = hitTest(docX, docY);

        if (target && target->iframeDoc()) {
            iframeHandleMouseMove(target, docX, docY, xrel, yrel,
                                  currentModState());
        }

        dom::Element* prevHover = hoveredElement_.get();
        if (target != prevHover) {
            if (prevHover && prevHover->iframeDoc()) {
                auto* pdp = static_cast<IframeDoc*>(prevHover->iframeDoc());
                if (pdp->hoveredElement) {
                    pdp->hoveredElement->markDirty();
                    pdp->hoveredElement = nullptr;
                    uiDirty_ = true;
                }
            }
            int mod = currentModState();

            if (prevHover) {
                dom::MouseEvent outEvt("mouseout", true, true);
                populateMouseEvent(outEvt, x, y, -1, pressedButtons_,
                                  xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
                outEvt.setRelatedTarget(target);
                applyMouseOffset(outEvt, prevHover);
                dispatchEvent(prevHover, outEvt);
            }

            if (prevHover) {
                dom::MouseEvent leaveEvt("mouseleave", false, false);
                populateMouseEvent(leaveEvt, x, y, -1, pressedButtons_,
                                  xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
                leaveEvt.setRelatedTarget(target);
                applyMouseOffset(leaveEvt, prevHover);
                dispatchEvent(prevHover, leaveEvt);
            }

            if (target) {
                dom::MouseEvent overEvt("mouseover", true, true);
                populateMouseEvent(overEvt, x, y, -1, pressedButtons_,
                                  xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
                overEvt.setRelatedTarget(prevHover);
                applyMouseOffset(overEvt, target);
                dispatchEvent(target, overEvt);
            }

            if (target) {
                dom::MouseEvent enterEvt("mouseenter", false, false);
                populateMouseEvent(enterEvt, x, y, -1, pressedButtons_,
                                  xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
                enterEvt.setRelatedTarget(prevHover);
                applyMouseOffset(enterEvt, target);
                dispatchEvent(target, enterEvt);
            }

            if (document_ && document_->cascade().usesHoverPseudo()) {
                markHoverChainDirty(document_->cascade(), hoveredElement_.get(), target);
                uiDirty_ = true;
            }
            hoveredElement_.assign(document_.get(), target);
        }

        updateCursorFromHover(hoveredElement_.get());

#if BRO_WITH_3D
        scene::HtmlNode* hnNode = nullptr;
        dom::Element* hnEl = nullptr;
        float hnPxX = 0.0f, hnPxY = 0.0f;
        bool hnHit = (target && pickHtmlNodeUnderMouse(target, docX, docY,
                                                        hnNode, hnEl, hnPxX, hnPxY));
        dom::Element* prevHnEl = hoveredHtmlElement_.get();
        if (hnEl != prevHnEl) {
            int mod = currentModState();
            if (prevHnEl) {
                dispatchHtmlNodeMouseEvent("mouseout", prevHnEl,
                                            hnPxX, hnPxY, -1, pressedButtons_,
                                            mod, xrel, yrel, /*bubbles=*/true,
                                            hnEl);
                dispatchHtmlNodeMouseEvent("mouseleave", prevHnEl,
                                            hnPxX, hnPxY, -1, pressedButtons_,
                                            mod, xrel, yrel, /*bubbles=*/false,
                                            hnEl);
                if (auto* ph = hoveredHtmlElement_.get()) ph->markDirty();
            }
            if (hnEl) {
                dispatchHtmlNodeMouseEvent("mouseover", hnEl,
                                            hnPxX, hnPxY, -1, pressedButtons_,
                                            mod, xrel, yrel, /*bubbles=*/true,
                                            prevHnEl);
                dispatchHtmlNodeMouseEvent("mouseenter", hnEl,
                                            hnPxX, hnPxY, -1, pressedButtons_,
                                            mod, xrel, yrel, /*bubbles=*/false,
                                            prevHnEl);
                hnEl->markDirty();
            }
            hoveredHtmlElement_.assign(hnNode ? hnNode->document() : nullptr, hnEl);
            hoveredHtmlNode_ = hnNode;
            uiDirty_ = true;
        }
#endif

#if BRO_WITH_3D
        if (hnHit) {
            int mod = currentModState();
            dispatchHtmlNodeMouseEvent("mousemove", hnEl, hnPxX, hnPxY,
                                        -1, pressedButtons_, mod,
                                        xrel, yrel, /*bubbles=*/true);
        } else
#endif
        if (target) {
            int mod = currentModState();
            dom::MouseEvent moveEvt("mousemove", true, true);
            populateMouseEvent(moveEvt, x, y, -1, pressedButtons_,
                              xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
            applyMouseOffset(moveEvt, target);
            dispatchPointerAlias("pointermove", target, moveEvt);
            dispatchEvent(target, moveEvt);
        }

        dragDrop_.update(target, x, y, pressedButtons_);
    }

    lastMouseX_ = x;
    lastMouseY_ = y;
}

#if BRO_WITH_3D
scene::SceneGraph* Engine::sceneGraphForElement(const dom::Element* el) const {
    if (!el) return nullptr;
    for (auto& sg : sceneGraphs_) {
        if (sg.element == el && sg.graph) return sg.graph.get();
    }
    return nullptr;
}
#endif

bool Engine::elementAbsoluteOrigin(dom::Element* el, float& outX, float& outY) const {
    if (!el) return false;
    dom::AbsolutePoint p = dom::absoluteContentOrigin(el);
    outX = p.x;
    outY = p.y;
    return true;
}

#if BRO_WITH_3D
bool Engine::pickHtmlNodeUnderMouse(dom::Element* canvasEl, float docX, float docY,
                                    scene::HtmlNode*& outNode, dom::Element*& outEl,
                                    float& outLocalPxX, float& outLocalPxY) {
    outNode = nullptr;
    outEl = nullptr;
    auto* sg = sceneGraphForElement(canvasEl);
    if (!sg) return false;

    float originX = 0.0f, originY = 0.0f;
    if (!elementAbsoluteOrigin(canvasEl, originX, originY)) return false;
    const float canvasLocalX = docX - originX;
    const float canvasLocalY = docY - originY;

    scene::SceneGraph::HtmlNodePick pick;
    if (!sg->pickHtmlNode(canvasLocalX, canvasLocalY, pick)) return false;
    if (!pick.node) return false;

    auto* doc = pick.node->document();
    if (!doc) return false;
    auto* root = doc->layoutRoot();
    if (!root) return false;

    auto* layoutNode = htmlayout::layout::hitTest(root, pick.localPxX, pick.localPxY);
    auto* hitEl = layout::LayoutNodeAdapter::elementFor(layoutNode);
    if (!hitEl) hitEl = doc->documentElement();
    if (!hitEl) return false;

    outNode = pick.node;
    outEl = hitEl;
    outLocalPxX = pick.localPxX;
    outLocalPxY = pick.localPxY;
    return true;
}

void Engine::dispatchHtmlNodeMouseEvent(const std::string& type,
                                        dom::Element* target,
                                        float localPxX, float localPxY,
                                        int button, int pressedButtons, int mods,
                                        float movX, float movY, bool bubbles,
                                        dom::Element* relatedTarget) {
    if (!target) return;
    dom::MouseEvent evt(type, bubbles, /*cancelable=*/true);
    evt.setIsTrusted(true);
    evt.setClientX(static_cast<double>(localPxX));
    evt.setClientY(static_cast<double>(localPxY));
    evt.setScreenX(static_cast<double>(localPxX));
    evt.setScreenY(static_cast<double>(localPxY));
    evt.setPageX(static_cast<double>(localPxX));
    evt.setPageY(static_cast<double>(localPxY));
    evt.setMovementX(static_cast<double>(movX));
    evt.setMovementY(static_cast<double>(movY));
    evt.setButton(button);
    evt.setButtons(pressedButtons);
    evt.setShiftKey((mods & SDL_KMOD_SHIFT) != 0);
    evt.setCtrlKey ((mods & SDL_KMOD_CTRL ) != 0);
    evt.setAltKey  ((mods & SDL_KMOD_ALT  ) != 0);
    evt.setMetaKey ((mods & SDL_KMOD_GUI  ) != 0);
    if (relatedTarget) evt.setRelatedTarget(relatedTarget);

    evt.setOffsetX(static_cast<double>(localPxX));
    evt.setOffsetY(static_cast<double>(localPxY));

    const char* pointerType = (type == "mousedown") ? "pointerdown"
                            : (type == "mouseup")   ? "pointerup"
                            : (type == "mousemove") ? "pointermove"
                            : nullptr;
    if (pointerType) dispatchPointerAlias(pointerType, target, evt);

    dispatchEvent(target, evt);
}
#endif

} // namespace bro::engine
