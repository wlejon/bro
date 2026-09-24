#include "engine/engine.h"
#include "engine/input_common.h"
#include "engine/overflow.h"
#include "engine/overlay.h"
#include "engine/settings.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "layout/el_textarea.h"
#include "util/platform.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace bro::engine {

void Engine::dispatchScrollEvent(dom::Element* el) {
    if (!el) return;
    dom::Event evt("scroll", false, false);
    evt.setIsTrusted(true);
    dispatchEvent(el, evt);
}

void Engine::scrollViewportTo(float y) {
    if (!document_) return;
    flushLayoutForRead(document_.get());
    if (!std::isfinite(y)) y = 0.0f;
    const float maxScroll =
        std::max(0.0f, documentHeight_ - static_cast<float>(contentHeight()));
    const float prev = scrollY_;
    scrollY_ = std::clamp(y, 0.0f, maxScroll);
    // A programmatic scroll replaces whatever a wheel gesture still had to go.
    wheelResidualY_ = 0.0f;
    if (scrollY_ != prev) {
        uiDirty_ = true;
        dispatchScrollEvent(document_->documentElement());
    }
}

void Engine::handleWheel(float x, float y, float dx, float dy) {
    if (!document_) return;

    if (overlayMgr_.handleWheel(x, overlayMouseY(y), dx, dy)) {
        uiDirty_ = true;
        return;
    }

    if (isSystemVisible() && systemHandleWheel(x, y, dx, dy)) {
        uiDirty_ = true;
        return;
    }

    float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;
    dom::Element* target = hitTest(docX, docY);

    const float pxPerTick = inputConfig_.scrollSpeed;
    const float pxX = util::wheelDeltaToPixels(dx, pxPerTick);
    const float pxY = util::wheelDeltaToPixels(dy, pxPerTick);
    const float pxV = util::wheelDeltaToPixels(
        util::verticalWheelDelta(dx, dy), pxPerTick);

    if (target) {
        dom::WheelEvent wheelEvt("wheel", true, true);
        int mod = currentModState();
        populateMouseEvent(wheelEvt, x, y, -1, pressedButtons_,
                           x - lastMouseX_, y - lastMouseY_, scrollY_, mod,
                           static_cast<float>(contentTop()));
        wheelEvt.setDeltaX(static_cast<double>(-pxX));
        wheelEvt.setDeltaY(static_cast<double>(-pxY));
        wheelEvt.setDeltaZ(0.0);
        wheelEvt.setDeltaMode(dom::WheelEvent::DOM_DELTA_PIXEL);
        applyMouseOffset(wheelEvt, target);
        dispatchEvent(target, wheelEvt);

        if (wheelEvt.defaultPrevented()) {
            return;
        }
    }

    auto* activeEl = document_->activeElement();
    auto* textarea = getElTextarea(activeEl);
    if (textarea && textarea->isFocused()) {
        float scroll = textarea->scrollY() - pxV;
        scroll = std::max(scroll, 0.0f);
        textarea->setScrollY(scroll);
        markAppBaseDirty();
        return;
    }

    auto* hoverTa = getElTextarea(target);
    if (hoverTa) {
        float scroll = hoverTa->scrollY() - pxV;
        scroll = std::max(scroll, 0.0f);
        hoverTa->setScrollY(scroll);
        markAppBaseDirty();
        return;
    }

    {
        auto* el = target;
        while (el) {
            std::string ov = getOverflowY(el->computedStyle());
            if (overflowScrollable(ov)) {
                float maxST = maxScrollTop(el);
                if (maxST > 0.0f) {
                    float prevScroll = el->scrollTopValue();
                    const bool canScroll = (pxV > 0.0f) ? (prevScroll > 0.5f)
                                                        : (prevScroll < maxST - 0.5f);
                    if (canScroll) {
                        float newScroll = std::clamp(prevScroll - pxV, 0.0f, maxST);
                        el->setScrollTopValue(newScroll);
                        if (newScroll != prevScroll) {
                            dispatchScrollEvent(el);
                        }
                        markAppBaseDirty();
                        return;
                    }
                }
            }
            el = composedParent(el);
        }
    }

    wheelResidualY_ -= pxV;
    uiDirty_ = true;
}

void Engine::drainWheelSmoothing(float frameDtSec) {
    if (wheelResidualY_ == 0.0f) return;

    constexpr float kSmoothRate = 60.0f;
    float t = 1.0f - std::exp(-frameDtSec * kSmoothRate);
    if (t > 1.0f) t = 1.0f;

    float apply = wheelResidualY_ * t;
    if (std::abs(wheelResidualY_) < 0.5f) {
        apply = wheelResidualY_;
        wheelResidualY_ = 0.0f;
    } else {
        wheelResidualY_ -= apply;
    }

    if (!document_) { wheelResidualY_ = 0.0f; return; }

    float maxScroll = std::max(0.0f, documentHeight_ - static_cast<float>(contentHeight()));
    float prevScroll = scrollY_;
    scrollY_ = std::clamp(scrollY_ + apply, 0.0f, maxScroll);
    if (scrollY_ == 0.0f || scrollY_ == maxScroll) {
        wheelResidualY_ = 0.0f;
    }
    if (scrollY_ != prevScroll) {
        if (document_->documentElement()) {
            dispatchScrollEvent(document_->documentElement());
        }
        uiDirty_ = true;
    }
}

void Engine::handleDropFile(const std::vector<std::string>& paths, float x, float y) {
    if (!document_) return;
    if (paths.empty()) return;

    float dropX = (x >= 0) ? x : lastMouseX_;
    float dropY = (y >= 0) ? y : lastMouseY_;
    float docX = dropX, docY = dropY - static_cast<float>(contentTop()) + scrollY_;
    dom::Element* target = hitTest(docX, docY);
    if (!target) target = document_->body();
    if (!target) return;

    for (const char* type : { "dragenter", "dragover", "drop" }) {
        dom::DragEvent evt(type, true, true);
        for (const auto& p : paths) evt.addFile(p);
        evt.setIsTrusted(true);
        dispatchEvent(target, evt);
    }
}

void Engine::handleDropText(const std::string& text, float x, float y) {
    if (!document_) return;

    float dropX = (x >= 0) ? x : lastMouseX_;
    float dropY = (y >= 0) ? y : lastMouseY_;
    float docX = dropX, docY = dropY - static_cast<float>(contentTop()) + scrollY_;
    dom::Element* target = hitTest(docX, docY);
    if (!target) target = document_->body();
    if (!target) return;

    dom::DragEvent enterEvt("dragenter", true, true);
    enterEvt.setDataText(text);
    enterEvt.setIsTrusted(true);
    dispatchEvent(target, enterEvt);

    dom::DragEvent overEvt("dragover", true, true);
    overEvt.setDataText(text);
    overEvt.setIsTrusted(true);
    dispatchEvent(target, overEvt);

    dom::DragEvent dropEvt("drop", true, true);
    dropEvt.setDataText(text);
    dropEvt.setIsTrusted(true);
    dispatchEvent(target, dropEvt);
}

} // namespace bro::engine
