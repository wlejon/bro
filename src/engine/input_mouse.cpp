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
#include "layout/top_layer_hit.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "layout/layout_node_adapter.h"
#include "layout/selection_geometry.h"
#include "layout/skia_text_metrics.h"
#include "platform/sdl_window.h"
#include "util/platform.h"
#include "util/time.h"

#if BRO_WITH_3D
#include "scene/scene_graph.h"
#include "scene/html_node.h"
#endif

#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace bro::engine {

float Engine::overlayMouseY(float y) const {
    const Overlay* active = overlayMgr_.active();
    if (active && active->context() == OverlayContext::App) {
        return y - static_cast<float>(contentTop());
    }
    return y;
}

dom::Element* Engine::iframeHitTest(IframeDoc* dp, float lx, float ly) {
    if (!dp || !dp->document) return nullptr;
    auto* root = dp->document->layoutRoot();
    if (!root) return nullptr;
    if (auto top = layout::hitTestTopLayer(dp->document.get(), root, lx, ly); top.handled)
        return top.element;
    auto* node = htmlayout::layout::hitTest(root, lx, ly);
    auto* hit = layout::LayoutNodeAdapter::elementFor(node);
    if (!hit || hit == dp->document->documentElement()) return nullptr;
    return hit;
}

bool Engine::iframeHandleMouseDown(dom::Element* frameEl, float docX, float docY,
                                   int button, float movementX, float movementY, int mod) {
    if (!frameEl || !frameEl->iframeDoc()) return false;
    auto* dp = static_cast<IframeDoc*>(frameEl->iframeDoc());
    if (!dp->document) return false;
    dom::AbsoluteRect box = dom::absoluteContentBox(frameEl);
    float lx = docX - box.x, ly = docY - box.y;
    dom::Element* sub = iframeHitTest(dp, lx, ly);
    if (sub) {
        dom::MouseEvent evt("mousedown");
        populateMouseEvent(evt, lx, ly, button, pressedButtons_, movementX, movementY,
                           0.0f, mod, 0.0f);
        applyMouseOffset(evt, sub);
        ControlContext cctx{dp->document.get(), renderer_.get(), window_.get(),
                            &uiDirty_, &overlayMgr_, OverlayContext::App, dp->boxW, dp->boxH};
        dispatchDocMousePress(cctx, dp->mouseState, sub, evt, lx, ly);
    }
    return true;
}

bool Engine::iframeHandleMouseUp(dom::Element* frameEl, float docX, float docY,
                                 int button, float movementX, float movementY, int mod) {
    if (!frameEl || !frameEl->iframeDoc()) return false;
    auto* dp = static_cast<IframeDoc*>(frameEl->iframeDoc());
    if (!dp->document) return false;
    dom::AbsoluteRect box = dom::absoluteContentBox(frameEl);
    float lx = docX - box.x, ly = docY - box.y;
    dom::Element* sub = iframeHitTest(dp, lx, ly);
    if (sub) {
        dom::MouseEvent upEvt("mouseup");
        populateMouseEvent(upEvt, lx, ly, button, pressedButtons_, movementX, movementY,
                           0.0f, mod, 0.0f);
        applyMouseOffset(upEvt, sub);
        ControlContext cctx{dp->document.get(), renderer_.get(), window_.get(),
                            &uiDirty_, &overlayMgr_, OverlayContext::App, dp->boxW, dp->boxH};
        dispatchDocMouseRelease(cctx, dp->mouseState, sub, upEvt,
                                lx, ly, button, pressedButtons_, mod,
                                movementX, movementY, lx, ly,
                                util::currentTimeMs(),
                                inputConfig_.doubleClickThresholdMs,
                                inputConfig_.doubleClickDistancePx);
    }
    return true;
}

void Engine::handleMouseDown(float x, float y, int button) {
    float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;
    uiDirty_ = true;

    const float prevMouseX = lastMouseX_, prevMouseY = lastMouseY_;
    lastMouseX_ = x;
    lastMouseY_ = y;

    button = sdlToDomButton(button);

    if (overlayMgr_.handleMouseDown(x, overlayMouseY(y), button)) {
        pressedButtons_ |= domButtonMask(button);
        markAppBaseDirty();
        return;
    }

    if (systemHandleMouseDown(x, y, button)) {
        pressedButtons_ |= domButtonMask(button);
        return;
    }

    if (inspector_.pickerMode && inspector_.visible && button == 0) {
        dom::Element* hit = inspector_.pickerHover ? inspector_.pickerHover : hitTest(docX, docY);
        if (hit) inspectorPickElement(hit);
        inspectorSetPickerMode(false);
        systemDirty_ = true;
        pressedButtons_ |= domButtonMask(button);
        return;
    }

#if BRO_WITH_3D
    if (gizmoHandleMouseDown(docX, docY, button)) {
        pressedButtons_ |= domButtonMask(button);
        return;
    }
#endif

    pressedButtons_ |= domButtonMask(button);
    dispatchMouseButtonAction(button, true);

    {
        float ct = static_cast<float>(contentTop());
        float vh = static_cast<float>(contentHeight());
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(viewportWidth_) - vs.width - vs.margin,
            ct, vh, documentHeight_, vh, scrollY_);
        if (viewportScrollbar_.hitTest(x, y, m)) {
            if (viewportScrollbar_.thumbHitTest(x, y, m)) {
                viewportScrollbar_.beginDrag(y, m);
                draggingViewportScrollbar_ = true;
            } else {
                setViewportScrollY(viewportScrollbar_.scrollToPosition(y,
                    documentHeight_, vh, m));
            }
            uiDirty_ = true;
            return;
        }
    }

    if (document_ && document_->documentElement()) {
        float cy = y - static_cast<float>(contentTop());
        ScrollbarMetrics em;
        dom::Element* hitElem = findElementScrollbarHit(
            document_->documentElement(), x, cy,
            0.0f, -scrollY_, elementScrollbar_, em);
        if (hitElem) {
            if (elementScrollbar_.thumbHitTest(x, cy, em)) {
                elementScrollbar_.beginDrag(cy, em);
                scrollbarDragTarget_.assign(document_.get(), hitElem);
            } else {
                float viewH = hitElem->layoutBox().contentRect.height;
                float maxST = maxScrollTop(hitElem);
                float contentH = viewH + maxST;
                float newScroll = elementScrollbar_.scrollToPosition(cy,
                    contentH, viewH, em);
                float prev = hitElem->scrollTopValue();
                float clamped = std::clamp(newScroll, 0.0f, maxST);
                hitElem->setScrollTopValue(clamped);
                if (clamped != prev) dispatchScrollEvent(hitElem);
            }
            markAppBaseDirty();
            return;
        }
    }

    if (document_) {
        commitActiveComposition();

        dom::MouseEvent evt("mousedown");
        int mod = currentModState();
        populateMouseEvent(evt, x, y, button, pressedButtons_,
                           x - prevMouseX, y - prevMouseY, scrollY_, mod,
                           static_cast<float>(contentTop()));

        dom::Element* target = hitTest(docX, docY);
        if (target && target->iframeDoc() &&
            iframeHandleMouseDown(target, docX, docY, button,
                                  x - prevMouseX, y - prevMouseY, mod)) {
            markAppBaseDirty();
            return;
        }
        if (target) applyMouseOffset(evt, target);

#if BRO_WITH_3D
        scene::HtmlNode* hnHit = nullptr;
        dom::Element* hnEl = nullptr;
        float hnPxX = 0.0f, hnPxY = 0.0f;
        if (target && pickHtmlNodeUnderMouse(target, docX, docY,
                                              hnHit, hnEl, hnPxX, hnPxY)) {
            htmlNodeMouseDownNode_ = hnHit;
            htmlNodeMouseDownElement_.assign(hnHit->document(), hnEl);
            dispatchHtmlNodeMouseEvent("mousedown", hnEl, hnPxX, hnPxY,
                                        button, pressedButtons_, mod,
                                        x - prevMouseX, y - prevMouseY,
                                        /*bubbles=*/true);
            return;
        }
        htmlNodeMouseDownNode_ = nullptr;
        htmlNodeMouseDownElement_.reset();
#endif

        if (button == 0) dragDrop_.arm(target, x, y);

        ControlContext cctx{document_.get(), renderer_.get(), window_.get(),
                            &uiDirty_, &overlayMgr_, OverlayContext::App,
                            contentWidth(), contentHeight()};

        const float focusX = x, focusY = y - static_cast<float>(contentTop());
        PressIntent intent;
        intent.ordinal = pressOrdinal(appMouseState_, target, focusX, focusY,
                                      util::currentTimeMs(),
                                      inputConfig_.doubleClickThresholdMs,
                                      inputConfig_.doubleClickDistancePx);
        intent.extend = (mod & SDL_KMOD_SHIFT) != 0;

        controlDragElement_.reset();
        if (button == 0 && isCaretControl(target)) {
            controlDragElement_.assign(document_.get(), target);
            controlDragIsPanel_ = false;
        }

        dispatchPointerAlias("pointerdown", target, evt);
        dispatchDocMousePress(cctx, appMouseState_, target, evt,
                              focusX, focusY, intent);
        updateTextInputArea();
        markAppBaseDirty();

        target = appMouseState_.mouseDownTarget.get();
        if (target && document_ && !document_->isNodeLive(target)) {
            target = nullptr;
        }

        // The press's default action on the selection. None at all when the
        // page cancelled the mousedown — that is how a canvas or a custom
        // widget says "this drag is mine" — and none inside a form control,
        // which keeps its own caret: a press on the <b> inside a toolbar
        // <button> leaves the document's selection where it was, exactly as
        // a press on the button itself does.
        if (button == 0 && document_ && textMetrics_ && !evt.defaultPrevented()) {
            bool isEditableControl = false;
            for (dom::Element* e = target; e; e = e->parentElement()) {
                const std::string& tag = e->tagName();
                if (tag == "INPUT" || tag == "TEXTAREA" || tag == "SELECT" ||
                    tag == "BUTTON" || tag == "OPTION") {
                    isEditableControl = true;
                    break;
                }
            }
            // A replaced element (canvas, image, video) holds no text a press
            // could put a caret in. Outside an editing host the press clears
            // the selection and starts none — a double-click on a game canvas
            // does not word-select the HUD text beside it.
            bool replaced = false;
            if (target && !inEditableHost(target)) {
                const std::string& tag = target->tagName();
                replaced = tag == "CANVAS" || tag == "IMG" || tag == "VIDEO";
            }
            bool suppressed = target && isSelectionSuppressed(target);
            if (replaced && !isEditableControl && !suppressed) {
                document_->selection()->removeAllRanges();
                selectionDragging_ = false;
                selectionAnchorNode_.reset();
                markAppBaseDirty();
            } else if (!isEditableControl && !suppressed) {
                // The caret goes into the text of the element pressed, not
                // the nearest text anywhere: a press on an empty block used to
                // land the caret in whatever paragraph was closest (possibly
                // a contenteditable one). Editing hosts scope to the host.
                auto* editHost = editableHostOf(target);
                dom::Element* scope = editHost ? editHost : target;
                auto hit = layout::hitTestText(document_.get(), docX, docY,
                                               *textMetrics_, scope);
                auto* sel = document_->selection();
                if (hit.textNode && document_->ownsNode(hit.textNode)) {
                    int detail = intent.ordinal;
                    if (detail >= 3) {
                        sel->setRange(hit.textNode, 0,
                                      hit.textNode,
                                      static_cast<int>(hit.textNode->length()),
                                      dom::Selection::Forward);
                        selectionDragging_ = false;
                    } else if (detail == 2) {
                        const std::string& s = hit.textNode->data();
                        int off = std::max(0, std::min(hit.srcOffset,
                            static_cast<int>(s.size())));
                        auto isWordChar = [](unsigned char c) {
                            return std::isalnum(c) || c == '_';
                        };
                        int lo = off;
                        while (lo > 0 && isWordChar(
                            static_cast<unsigned char>(s[lo - 1]))) lo--;
                        int hi = off;
                        while (hi < static_cast<int>(s.size()) &&
                               isWordChar(static_cast<unsigned char>(s[hi]))) hi++;
                        sel->setRange(hit.textNode, lo, hit.textNode, hi,
                                      dom::Selection::Forward);
                        selectionDragging_ = false;
                    } else {
                        sel->collapse(hit.textNode, hit.srcOffset);
                        selectionAnchorNode_.assign(document_.get(), hit.textNode);
                        selectionAnchorOffset_ = hit.srcOffset;
                        selectionDragging_ = true;
                        selectionPressX_ = docX;
                        selectionPressY_ = docY;
                        selectionPastThreshold_ = false;
                    }
                    markAppBaseDirty();
                } else if (target && document_->ownsNode(target) &&
                           inEditableHost(target)) {
                    const int idx = static_cast<int>(target->childNodes().size());
                    sel->collapse(target, idx);
                    selectionDragging_ = false;
                    selectionAnchorNode_.reset();
                    markAppBaseDirty();
                } else if (target && document_->ownsNode(target)) {
                    // A block with no text of its own: the caret goes into
                    // it (Chromium's answer), not into text elsewhere.
                    sel->collapse(target, 0);
                    selectionDragging_ = false;
                    selectionAnchorNode_.reset();
                    markAppBaseDirty();
                } else {
                    sel->removeAllRanges();
                    selectionDragging_ = false;
                    selectionAnchorNode_.reset();
                    markAppBaseDirty();
                }
            }
        }
    }
}

void Engine::handleMouseUp(float x, float y, int button) {
    float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;
    uiDirty_ = true;

    const float prevMouseX = lastMouseX_, prevMouseY = lastMouseY_;
    lastMouseX_ = x;
    lastMouseY_ = y;

    button = sdlToDomButton(button);
    pressedButtons_ &= ~domButtonMask(button);
    dispatchMouseButtonAction(button, false);

    if (overlayMgr_.handleMouseUp(x, overlayMouseY(y), button)) {
        markAppBaseDirty();
        return;
    }

    if (viewportScrollbar_.isDragging()) {
        viewportScrollbar_.endDrag();
        draggingViewportScrollbar_ = false;
        uiDirty_ = true;
    }
    if (elementScrollbar_.isDragging()) {
        elementScrollbar_.endDrag();
        scrollbarDragTarget_.reset();
        if (scrollbarDragSystemDoc_) {
            systemDirty_ = true;
            scrollbarDragSystemDoc_ = nullptr;
        }
        uiDirty_ = true;
        return;
    }

    if (systemHandleMouseUp(x, y, button)) {
        return;
    }

#if BRO_WITH_3D
    if (gizmoHandleMouseUp(docX, docY, button)) {
        return;
    }
#endif

    if (document_) {
        auto* activeEl = document_->activeElement();
        auto* input = getElInput(activeEl);
        if (input && input->isDragging()) {
            input->setDragging(false);
            dom::Event changeEvt("change");
            dispatchEvent(activeEl, changeEvt);
            uiDirty_ = true;
        }
    }

    if (button == 0) {
        selectionDragging_ = false;
        controlDragElement_.reset();
    }

    if (document_) {
        dom::Element* target = hitTest(docX, docY);
        int mod = currentModState();
        float ct = static_cast<float>(contentTop());
        float movX = x - prevMouseX;
        float movY = y - prevMouseY;
        float clientY = y - ct;
        float pageY = clientY + scrollY_;

        if (target && target->iframeDoc() &&
            iframeHandleMouseUp(target, docX, docY, button, movX, movY, mod)) {
            markAppBaseDirty();
            return;
        }

#if BRO_WITH_3D
        scene::HtmlNode* hnHit = nullptr;
        dom::Element* hnEl = nullptr;
        float hnPxX = 0.0f, hnPxY = 0.0f;
        bool hnReleaseHit = (target && pickHtmlNodeUnderMouse(target, docX, docY,
                                                                hnHit, hnEl, hnPxX, hnPxY));
        if (htmlNodeMouseDownNode_) {
            dom::Element* downEl = htmlNodeMouseDownElement_.get();
            if (hnReleaseHit && hnHit == htmlNodeMouseDownNode_) {
                dispatchHtmlNodeMouseEvent("mouseup", hnEl, hnPxX, hnPxY,
                                            button, pressedButtons_, mod,
                                            movX, movY, /*bubbles=*/true);
                if (button == 0 && hnEl == downEl) {
                    dispatchHtmlNodeMouseEvent("click", hnEl, hnPxX, hnPxY,
                                                button, pressedButtons_, mod,
                                                movX, movY, /*bubbles=*/true);
                }
            } else {
                dispatchHtmlNodeMouseEvent("mouseup", downEl,
                                            hnPxX, hnPxY,
                                            button, pressedButtons_, mod,
                                            movX, movY, /*bubbles=*/true);
            }
            htmlNodeMouseDownNode_ = nullptr;
            htmlNodeMouseDownElement_.reset();
            return;
        }
#endif

        dom::MouseEvent upEvt("mouseup");
        populateMouseEvent(upEvt, x, y, button, pressedButtons_,
                           movX, movY, scrollY_, mod, ct);
        if (target) applyMouseOffset(upEvt, target);

        ControlContext cctx{document_.get(), renderer_.get(), window_.get(),
                            &uiDirty_, &overlayMgr_, OverlayContext::App,
                            contentWidth(), contentHeight()};

        if (dragDrop_.finish(target, x, y)) {
            markAppBaseDirty();
            return;
        }

        dispatchPointerAlias("pointerup", target, upEvt);
        dispatchDocMouseRelease(cctx, appMouseState_, target, upEvt,
                                x, clientY, button, pressedButtons_, mod,
                                movX, movY, x, pageY,
                                util::currentTimeMs(),
                                inputConfig_.doubleClickThresholdMs,
                                inputConfig_.doubleClickDistancePx);
    }
}

void Engine::updateCursorFromHover(dom::Element* target) {
    std::string css;
    if (target) {
        const auto& cs = target->computedStyle();
        auto it = cs.find("cursor");
        if (it != cs.end()) css = it->second;
    }
    platform::CursorShape shape = cursorShapeFromCss(css);
    resolvedCursor_ = cursorShapeName(shape);
    if (displayMode_ == DisplayMode::Windowed && window_ && !lockedElement_.get()) {
        window_->setCursor(shape);
    }
}

bool Engine::requestPointerLock(dom::Element* target) {
    if (!target || !document_ || !document_->isNodeLive(target)) return false;
    if (lockedElement_.get() == target) return true;

    lockedElement_.assign(document_.get(), target);
    lockedMouseX_ = lastMouseX_;
    lockedMouseY_ = lastMouseY_;

    if (window_) {
        SDL_SetWindowRelativeMouseMode(window_->getSDLWindow(), true);
    }

    if (document_ && document_->documentElement()) {
        dom::Event evt("pointerlockchange", true, false);
        evt.setIsTrusted(true);
        dispatchEvent(document_->documentElement(), evt);
    }
    return true;
}

void Engine::setPageVisibility(bool visible) {
    // Edge-triggered, as the web's is: focus-lost and minimized both call
    // this with false, and a page must not see two visibilitychange events
    // for one transition.
    if (pageVisible_ == visible) return;
    pageVisible_ = visible;
    if (document_ && document_->documentElement()) {
        dom::Event evt("visibilitychange", true, false);
        evt.setIsTrusted(true);
        dispatchEvent(document_->documentElement(), evt);
    }
}

void Engine::setFullscreenState(bool fullscreen) {
    if (document_ && document_->documentElement()) {
        dom::Event evt("fullscreenchange", true, false);
        evt.setIsTrusted(true);
        dispatchEvent(document_->documentElement(), evt);
    }
}

void Engine::exitPointerLock() {
    if (!lockedElement_.held()) return;
    lockedElement_.reset();

    if (window_) {
        SDL_WarpMouseInWindow(window_->getSDLWindow(), lockedMouseX_, lockedMouseY_);
        SDL_SetWindowRelativeMouseMode(window_->getSDLWindow(), false);
    }

    if (document_ && document_->documentElement()) {
        dom::Event evt("pointerlockchange", true, false);
        evt.setIsTrusted(true);
        dispatchEvent(document_->documentElement(), evt);
    }
}

} // namespace bro::engine
