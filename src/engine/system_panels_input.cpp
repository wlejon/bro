// Engine system panel input & inspection methods (split from system_panels.cpp).
#include "engine/engine.h"
#include "engine/key_mapping.h"
#include "engine/overflow.h"
#include "engine/replaced_elements.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "layout/box.h"
#include "layout/layout_node_adapter.h"
#include "platform/keys.h"
#include "platform/wheel.h"
#include "util/time.h"
#include <algorithm>

namespace bro::engine {

// ---------------------------------------------------------------------------
// Mouse input
// ---------------------------------------------------------------------------

dom::Element* Engine::systemHitTest(SystemDocument& doc, float x, float y) {
    if (!doc.document || !doc.document->documentElement()) return nullptr;
    auto* root = doc.document->layoutRoot();
    if (!root) return nullptr;
    auto* node = htmlayout::layout::hitTest(root, x, y);
    auto* hit = layout::LayoutNodeAdapter::elementFor(node);
    if (!hit) return nullptr;
    if (hit == doc.document->documentElement()) return nullptr;
    auto& tag = hit->tagName();
    if (tag == "BODY" && hit->layoutBox().fullHeight() < 1.0f) return nullptr;
    return hit;
}

bool Engine::systemHandleMouseDown(float x, float y, int button) {
    if (!isSystemVisible()) return false;

    systemMouseConsumed_ = false;
    for (auto& doc : systemDocs_) {
        if (!isSystemDocVisible(doc) || !doc.document) continue;
        ControlContext cctx{doc.document.get(),
                           renderer_.get(), window_.get(), &systemDirty_,
                           overlayMgr_.get(), OverlayContext::System,
                           viewportWidth_, viewportHeight_};
        auto* prevActive = doc.document->activeElement();
        auto disp = unfocusPreviousControl(cctx, prevActive);
        if (disp == ClickDisposition::Consumed) {
            systemMouseConsumed_ = true;
            systemDirty_ = true;
            return true;
        }
    }

    for (int i = static_cast<int>(systemDocs_.size()) - 1; i >= 0; i--) {
        auto& doc = systemDocs_[i];
        if (!isSystemDocVisible(doc) || !doc.document) continue;
        ScrollbarMetrics em;
        dom::Element* hitElem = findElementScrollbarHit(
            doc.document->documentElement(), x, y,
            0.0f, 0.0f, elementScrollbar_, em);
        if (!hitElem) continue;
        const float along = em.horizontal ? x : y;
        if (elementScrollbar_.thumbHitTest(x, y, em)) {
            elementScrollbar_.beginDrag(along, em);
            scrollbarDragTarget_.assign(doc.document.get(), hitElem);
            scrollbarDragSystemDoc_ = &doc;
        } else if (pageElementScrollbar(hitElem, elementScrollbar_, em, along)) {
            systemDirty_ = true;
            doc.document->markDirty();
        }
        systemDirty_ = true;
        return true;
    }

    for (int i = static_cast<int>(systemDocs_.size()) - 1; i >= 0; i--) {
        auto& doc = systemDocs_[i];
        if (!isSystemDocVisible(doc)) continue;
        dom::Element* target = systemHitTest(doc, x, y);
        if (target) {
            ControlContext cctx{doc.document.get(),
                               renderer_.get(), window_.get(), &systemDirty_,
                               overlayMgr_.get(), OverlayContext::System,
                               viewportWidth_, viewportHeight_};

            dom::MouseEvent evt("mousedown");
            evt.setClientX(static_cast<double>(x));
            evt.setClientY(static_cast<double>(y));
            evt.setButton(button);
            evt.setIsTrusted(true);
            applyMouseOffset(evt, target);

            PressIntent intent;
            intent.ordinal = pressOrdinal(doc.mouseState, target, x, y,
                                          util::currentTimeMs(),
                                          inputConfig_.doubleClickThresholdMs,
                                          inputConfig_.doubleClickDistancePx);
            intent.extend = (currentModState() & platform::kmod::Shift) != 0;

            controlDragElement_.reset();
            if (button == 0 &&
                (getElTextarea(target) ||
                 (getElInput(target) && getElInput(target)->isTextType(target)))) {
                controlDragElement_.assign(doc.document.get(), target);
                controlDragIsPanel_ = true;
            }

            dispatchDocMousePress(cctx, doc.mouseState, target, evt, x, y, intent);
            return true;
        }
    }
    return false;
}

bool Engine::systemHandleMouseUp(float x, float y, int button) {
    if (!isSystemVisible()) return false;

    if (systemMouseConsumed_) {
        systemMouseConsumed_ = false;
        return true;
    }

    for (int i = static_cast<int>(systemDocs_.size()) - 1; i >= 0; i--) {
        auto& doc = systemDocs_[i];
        if (!isSystemDocVisible(doc)) continue;
        dom::Element* target = systemHitTest(doc, x, y);

        if (target) {
            ControlContext cctx{doc.document.get(),
                               renderer_.get(), window_.get(), &systemDirty_,
                               overlayMgr_.get(), OverlayContext::System,
                               viewportWidth_, viewportHeight_};

            dom::MouseEvent upEvt("mouseup");
            upEvt.setClientX(static_cast<double>(x));
            upEvt.setClientY(static_cast<double>(y));
            upEvt.setButton(button);
            upEvt.setIsTrusted(true);
            applyMouseOffset(upEvt, target);

            dispatchDocMouseRelease(cctx, doc.mouseState, target, upEvt,
                                    x, y, button, 0, 0, 0, 0, x, y,
                                    util::currentTimeMs(),
                                    inputConfig_.doubleClickThresholdMs,
                                    inputConfig_.doubleClickDistancePx);

            if (doc.document) {
                auto* activeEl = doc.document->activeElement();
                auto* input = getElInput(activeEl);
                if (input && input->isDragging()) {
                    input->setDragging(false);
                    dom::Event changeEvt("change");
                    dispatchControlEvent(cctx, activeEl, changeEvt);
                }
            }
            return true;
        }
    }
    return false;
}

dom::Element* Engine::systemHoverHit(float x, float y, SystemDocument*& doc) {
    doc = nullptr;
    for (int i = static_cast<int>(systemDocs_.size()) - 1; i >= 0; i--) {
        auto& d = systemDocs_[i];
        if (!isSystemDocVisible(d)) continue;
        if (dom::Element* target = systemHitTest(d, x, y)) {
            doc = &d;
            return target;
        }
    }
    return nullptr;
}

void Engine::systemSetHover(dom::Element* newTarget, SystemDocument* newDoc, float x, float y) {
    dom::Element* prevSysHover = systemHoverTarget_.get();
    if (newTarget == prevSysHover) return;
    if (prevSysHover && systemHoverDoc_) {
        dom::MouseEvent leaveEvt("mouseleave");
        leaveEvt.setClientX(static_cast<double>(x));
        leaveEvt.setClientY(static_cast<double>(y));
        dom::dispatchDomEvent(prevSysHover, leaveEvt);
    }
    if (newTarget && newDoc) {
        dom::MouseEvent enterEvt("mouseenter");
        enterEvt.setClientX(static_cast<double>(x));
        enterEvt.setClientY(static_cast<double>(y));
        dom::dispatchDomEvent(newTarget, enterEvt);
    }
    systemHoverTarget_.assign(newDoc ? newDoc->document.get() : nullptr, newTarget);
    systemHoverDoc_ = newDoc;
    systemDirty_ = true;
}

bool Engine::systemHandleMouseMove(float x, float y) {
    if (!isSystemVisible()) return false;

    SystemDocument* newDoc = nullptr;
    dom::Element* newTarget = systemHoverHit(x, y, newDoc);

    systemSetHover(newTarget, newDoc, x, y);

    if (newTarget && newDoc) {
        dom::MouseEvent moveEvt("mousemove");
        moveEvt.setClientX(static_cast<double>(x));
        moveEvt.setClientY(static_cast<double>(y));
        dom::dispatchDomEvent(newTarget, moveEvt);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Keyboard input (modal panels)
// ---------------------------------------------------------------------------

bool Engine::systemHandleKeyDown(int keycode, int scancode, int mod, bool repeat) {
    if (!systemSettingsVisible_) return false;
    bool prevented = false;
    for (auto& doc : systemDocs_) {
        if (doc.group != "nav" && doc.group != "settings") continue;
        if (!isSystemDocVisible(doc) || !doc.document) continue;
        auto* body = doc.document->body();
        if (!body) continue;
        dom::KeyboardEvent evt("keydown");
        evt.setKey(sdlKeycodeToWebKey(keycode, mod));
        evt.setCode(sdlScancodeToWebCode(scancode));
        evt.setCtrlKey((mod & platform::kmod::Ctrl) != 0);
        evt.setShiftKey((mod & platform::kmod::Shift) != 0);
        evt.setAltKey((mod & platform::kmod::Alt) != 0);
        evt.setMetaKey((mod & platform::kmod::Gui) != 0);
        evt.setRepeat(repeat);
        evt.setIsTrusted(true);
        dom::dispatchDomEvent(body, evt);
        if (evt.defaultPrevented()) prevented = true;
    }
    return prevented;
}

bool Engine::systemHandleWheel(float x, float y, float dx, float dy) {
    if (!isSystemVisible()) return false;
    platform::shiftWheelToHorizontal(dx, dy, (currentModState() & platform::kmod::Shift) != 0);
    for (int i = static_cast<int>(systemDocs_.size()) - 1; i >= 0; i--) {
        auto& doc = systemDocs_[i];
        if (!isSystemDocVisible(doc) || !doc.document) continue;
        dom::Element* target = systemHitTest(doc, x, y);
        if (!target) continue;

        // A horizontal delta goes to the nearest horizontal scroller that can
        // still take it, as in the app document.
        if (dx != 0.0f) {
            bool movedX = false;
            const float pxX = platform::wheelDeltaToPixels(dx, inputConfig_.scrollSpeed);
            if (wheelScrollChain(target, true, pxX, movedX)) {
                if (movedX) {
                    systemDirty_ = true;
                    if (doc.document) doc.document->markDirty();
                }
                if (dy == 0.0f) return true;
            }
        }

        auto* el = target;
        while (el) {
            std::string ov = getOverflowY(el->computedStyle());
            if (overflowScrollable(ov)) {
                float maxST = dom::maxScrollTopOf(el);
                if (maxST > 0) {
                    float scrollPx = -platform::wheelDeltaToPixels(
                        platform::verticalWheelDelta(dx, dy), inputConfig_.scrollSpeed);
                    float prev = el->scrollTopValue();
                    float next = std::clamp(prev + scrollPx, 0.0f, maxST);
                    if (next != prev) {
                        el->setScrollTopValue(next);
                        systemDirty_ = true;
                        if (doc.document) doc.document->markDirty();
                    }
                    return true;
                }
            }
            el = el->parentElement();
        }
        if (systemSettingsVisible_) return true;
    }
    return false;
}

bool Engine::systemHandleKeyUp(int keycode, int scancode, int mod, bool repeat) {
    if (!systemSettingsVisible_) return false;
    bool prevented = false;
    for (auto& doc : systemDocs_) {
        if (doc.group != "nav" && doc.group != "settings") continue;
        if (!isSystemDocVisible(doc) || !doc.document) continue;
        auto* body = doc.document->body();
        if (!body) continue;
        dom::KeyboardEvent evt("keyup");
        evt.setKey(sdlKeycodeToWebKey(keycode, mod));
        evt.setCode(sdlScancodeToWebCode(scancode));
        evt.setCtrlKey((mod & platform::kmod::Ctrl) != 0);
        evt.setShiftKey((mod & platform::kmod::Shift) != 0);
        evt.setAltKey((mod & platform::kmod::Alt) != 0);
        evt.setMetaKey((mod & platform::kmod::Gui) != 0);
        evt.setRepeat(repeat);
        evt.setIsTrusted(true);
        dom::dispatchDomEvent(body, evt);
        if (evt.defaultPrevented()) prevented = true;
    }
    return prevented;
}


// ---------------------------------------------------------------------------
// Headless DOM inspection
// ---------------------------------------------------------------------------

dom::Element* Engine::overlayQuerySelector(const std::string& panelName,
                                           const std::string& selector) {
    for (auto& doc : systemDocs_) {
        if (doc.name != panelName) continue;
        if (!doc.loaded) loadSystemPanel(doc);   // inspecting a panel builds it
        return doc.document ? doc.document->querySelector(selector) : nullptr;
    }
    return nullptr;
}

std::vector<std::string> Engine::overlayPanelNames() const {
    std::vector<std::string> names;
    for (const auto& doc : systemDocs_) {
        names.push_back(doc.name);
    }
    return names;
}

} // namespace bro::engine
