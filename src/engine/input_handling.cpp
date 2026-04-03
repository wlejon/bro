// Engine input handling methods — split from engine.cpp for readability.
// These are Engine member function implementations, not a separate class.

#include "engine/engine.h"
#include "engine/hit_testing.h"
#include "engine/key_mapping.h"
#include "engine/overflow.h"
#include "engine/system_overlay.h"

#include "platform/sdl_window.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/event_dispatch.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "util/time.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace bro::engine {

// ---------------------------------------------------------------------------
// Input focus helpers
// ---------------------------------------------------------------------------

static layout::ElInput* getElInput(dom::Element* el) {
    return el ? el->inputControl() : nullptr;
}
static layout::ElTextarea* getElTextarea(dom::Element* el) {
    return el ? el->textareaControl() : nullptr;
}
static layout::ElSelect* getElSelect(dom::Element* el) {
    return el ? el->selectControl() : nullptr;
}

// Returns true if the element is a focusable text-editing control (input or textarea)
static bool isTextEditable(dom::Element* el) {
    return getElInput(el) || getElTextarea(el);
}

// Pick a color from the color picker grid at pixel position (x, y).
// The grid is a 10x8 HSL palette anchored at (gridX, gridY) with size (gridW, gridH).
// Returns a hex color string like "#ff8800".
static std::string pickColorFromGrid(float x, float y,
                                     float gridX, float gridY,
                                     float gridW, float gridH) {
    float cellW = (gridW - 4) / 10.0f;
    float cellH = (gridH - 4) / 8.0f;
    int col = std::clamp(static_cast<int>((x - gridX - 2) / cellW), 0, 9);
    int row = std::clamp(static_cast<int>((y - gridY - 2) / cellH), 0, 7);

    float hue = col * 36.0f;
    float sat, lit;
    if (row == 0) {
        sat = 0.0f; lit = col / 9.0f;
    } else {
        sat = 1.0f; lit = 0.15f + (row - 1) * 0.1f;
    }

    auto hue2rgb = [](float p, float q, float t) -> float {
        if (t < 0) t += 1; if (t > 1) t -= 1;
        if (t < 1.0f/6) return p + (q-p)*6*t;
        if (t < 1.0f/2) return q;
        if (t < 2.0f/3) return p + (q-p)*(2.0f/3-t)*6;
        return p;
    };
    uint8_t cr, cg, cb;
    if (sat == 0) {
        cr = cg = cb = static_cast<uint8_t>(lit * 255);
    } else {
        float q = lit < 0.5f ? lit*(1+sat) : lit+sat-lit*sat;
        float p = 2*lit-q;
        float hn = hue/360.0f;
        cr = static_cast<uint8_t>(hue2rgb(p, q, hn+1.0f/3)*255);
        cg = static_cast<uint8_t>(hue2rgb(p, q, hn)*255);
        cb = static_cast<uint8_t>(hue2rgb(p, q, hn-1.0f/3)*255);
    }

    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", cr, cg, cb);
    return hex;
}

// Build a KeyboardEvent with all modifier fields set.
static dom::KeyboardEvent makeKeyboardEvent(const char* type,
                                            int keycode, int scancode,
                                            int mod, bool repeat) {
    dom::KeyboardEvent evt(type);
    evt.setKey(sdlKeycodeToWebKey(keycode, mod));
    evt.setCode(sdlScancodeToWebCode(scancode));
    evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
    evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
    evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
    evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
    evt.setRepeat(repeat);
    evt.setIsTrusted(true);

    // Set location for left/right modifier keys
    if (scancode == SDL_SCANCODE_LSHIFT || scancode == SDL_SCANCODE_LCTRL ||
        scancode == SDL_SCANCODE_LALT || scancode == SDL_SCANCODE_LGUI)
        evt.setLocation(1); // DOM_KEY_LOCATION_LEFT
    else if (scancode == SDL_SCANCODE_RSHIFT || scancode == SDL_SCANCODE_RCTRL ||
             scancode == SDL_SCANCODE_RALT || scancode == SDL_SCANCODE_RGUI)
        evt.setLocation(2); // DOM_KEY_LOCATION_RIGHT
    else if (keycode >= SDLK_KP_DIVIDE && keycode <= SDLK_KP_EQUALS)
        evt.setLocation(3); // DOM_KEY_LOCATION_NUMPAD

    return evt;
}

// Build a MouseEvent with standard fields populated
static void populateMouseEvent(dom::MouseEvent& evt, float x, float y,
                               int button, int buttons,
                               float lastX, float lastY,
                               float scrollY, int mod) {
    evt.setClientX(static_cast<double>(x));
    evt.setClientY(static_cast<double>(y));
    evt.setScreenX(static_cast<double>(x));
    evt.setScreenY(static_cast<double>(y));
    evt.setPageX(static_cast<double>(x));
    evt.setPageY(static_cast<double>(y + scrollY));
    evt.setMovementX(static_cast<double>(x - lastX));
    evt.setMovementY(static_cast<double>(y - lastY));
    evt.setButton(button);
    evt.setButtons(buttons);
    evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
    evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
    evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
    evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
    evt.setIsTrusted(true);
}

// ---------------------------------------------------------------------------
// Scrollbar hit testing helper
// ---------------------------------------------------------------------------

/// Walk the DOM tree to find which overflow element's scrollbar contains (x, y).
/// Returns the element whose scrollbar was hit, or nullptr.
/// Sets outMetrics to the scrollbar metrics for the hit element.
static dom::Element* findElementScrollbarHit(
    dom::Element* elem, float x, float y,
    float offsetX, float offsetY,
    Scrollbar& scrollbar, ScrollbarMetrics& outMetrics)
{
    if (!elem) return nullptr;
    auto& style = elem->computedStyle();
    {
        auto it = style.find("display");
        if (it != style.end() && it->second == "none") return nullptr;
    }

    auto& lbox = elem->layoutBox();
    float absX = lbox.contentRect.x + offsetX;
    float absY = lbox.contentRect.y + offsetY;

    // Recurse into composed children FIRST to find the deepest match
    float childOffsetX = absX;
    float childOffsetY = absY - elem->scrollTopValue();
    dom::Element* hit = nullptr;
    elem->forEachComposedChild([&](dom::Element* child) {
        if (!hit) {
            hit = findElementScrollbarHit(child, x, y,
                childOffsetX, childOffsetY, scrollbar, outMetrics);
        }
    });
    if (hit) return hit;

    // Only show scrollbars for scroll/auto, not hidden
    std::string ov = getOverflowY(style);
    if (overflowScrollable(ov)) {
        float maxST = maxScrollTop(elem);
        if (maxST > 0) {
            float viewH = lbox.contentRect.height;
            float contentH = viewH + maxST;
            float bx = absX - lbox.padding.left - lbox.border.left;
            float by = absY - lbox.padding.top - lbox.border.top;
            float bw = lbox.fullWidth();
            float bh = lbox.fullHeight();

            auto& es = scrollbar.style();
            auto m = scrollbar.layout(
                bx + bw - es.width - es.margin,
                by, bh, contentH, viewH,
                elem->scrollTopValue());
            if (scrollbar.hitTest(x, y, m)) {
                outMetrics = m;
                return elem;
            }
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Focus event dispatching
// ---------------------------------------------------------------------------

void Engine::dispatchFocusEvents(dom::Element* oldTarget, dom::Element* newTarget) {
    if (oldTarget == newTarget) return;

    (void)jsRuntime_; // focus events are dispatched via dispatchEvent

    // blur (non-bubbling) on old target
    if (oldTarget) {
        dom::FocusEvent blurEvt("blur", false, false);
        blurEvt.setRelatedTarget(newTarget);
        blurEvt.setIsTrusted(true);
        dispatchEvent(oldTarget, blurEvt);
    }

    // focus (non-bubbling) on new target
    if (newTarget) {
        dom::FocusEvent focusEvt("focus", false, false);
        focusEvt.setRelatedTarget(oldTarget);
        focusEvt.setIsTrusted(true);
        dispatchEvent(newTarget, focusEvt);
    }

    // focusout (bubbling) on old target
    if (oldTarget) {
        dom::FocusEvent focusoutEvt("focusout", true, false);
        focusoutEvt.setRelatedTarget(newTarget);
        focusoutEvt.setIsTrusted(true);
        dispatchEvent(oldTarget, focusoutEvt);
    }

    // focusin (bubbling) on new target
    if (newTarget) {
        dom::FocusEvent focusinEvt("focusin", true, false);
        focusinEvt.setRelatedTarget(oldTarget);
        focusinEvt.setIsTrusted(true);
        dispatchEvent(newTarget, focusinEvt);
    }
}

// ---------------------------------------------------------------------------
// Scroll event dispatching
// ---------------------------------------------------------------------------

void Engine::dispatchScrollEvent(dom::Element* el) {
    if (!el) return;
    dom::Event evt("scroll", false, false); // scroll doesn't bubble
    evt.setIsTrusted(true);
    dispatchEvent(el, evt);
}

// ---------------------------------------------------------------------------
// Mouse events
// ---------------------------------------------------------------------------

void Engine::handleMouseDown(float x, float y, int button) {
    // x, y = raw mouse position (screen space).
    // docX, docY = document space (for hit testing into the scrolled document).
    // IMPORTANT: overlay positions (lastDrawPos, color picker, select dropdown)
    // are in screen space — always use x/y when comparing, never docX/docY.
    float docX = x, docY = y + scrollY_;
    uiDirty_ = true;

    // Update button bitmask
    pressedButtons_ |= (1 << button);

    // --- Scrollbar interaction (before DOM hit testing) ---

    // Check viewport scrollbar
    {
        float vh = static_cast<float>(viewportHeight_);
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(viewportWidth_) - vs.width - vs.margin,
            0.0f, vh, documentHeight_, vh, scrollY_);
        if (viewportScrollbar_.hitTest(x, y, m)) {
            if (viewportScrollbar_.thumbHitTest(x, y, m)) {
                viewportScrollbar_.beginDrag(y, m);
                draggingViewportScrollbar_ = true;
            } else {
                // Click on track — page scroll
                scrollY_ = viewportScrollbar_.scrollToPosition(y,
                    documentHeight_, vh, m);
            }
            uiDirty_ = true;
            return; // consumed
        }
    }

    // Check element scrollbars
    if (document_ && document_->documentElement()) {
        ScrollbarMetrics em;
        dom::Element* hitElem = findElementScrollbarHit(
            document_->documentElement(), x, y,
            0.0f, -scrollY_, elementScrollbar_, em);
        if (hitElem) {
            if (elementScrollbar_.thumbHitTest(x, y, em)) {
                elementScrollbar_.beginDrag(y, em);
                scrollbarDragTarget_ = hitElem;
            } else {
                // Click on track — page scroll
                float viewH = hitElem->layoutBox().contentRect.height;
                float maxST = maxScrollTop(hitElem);
                float contentH = viewH + maxST;
                float newScroll = elementScrollbar_.scrollToPosition(y,
                    contentH, viewH, em);
                hitElem->setScrollTopValue(std::clamp(newScroll, 0.0f, maxST));
            }
            uiDirty_ = true;
            return; // consumed
        }
    }

    if (document_) {
        dom::MouseEvent evt("mousedown");
        int mod = SDL_GetModState();
        populateMouseEvent(evt, x, y, button, pressedButtons_,
                          lastMouseX_, lastMouseY_, scrollY_, mod);

        dom::Element* target = hitTest(docX, docY);
        mouseDownTarget_ = target;
        if (target) {
            // Track focus change
            auto* prevActive = document_->activeElement();

            // Unfocus previous controls
            auto* prevInput = getElInput(prevActive);
            if (prevInput) {
                // Close color picker if clicking outside it
                if (prevInput->isPickerOpen()) {
                    auto dp = prevInput->lastDrawPos();
                    float px = dp.x, py = dp.y + dp.h + 2;
                    float pw = 200.0f, ph = 160.0f;
                    bool inPicker = (x >= px && x < px + pw && y >= py && y < py + ph);
                    bool inSwatch = (x >= dp.x && x < dp.x + dp.w && y >= dp.y && y < dp.y + dp.h);
                    if (inPicker) {
                        // Click inside picker — select the color
                        std::string hex = pickColorFromGrid(x, y, px, py, pw, ph);
                        prevActive->setAttribute("value", hex.c_str());
                        dom::Event changeEvt("change");
                        dispatchEvent(prevActive, changeEvt);
                        dispatchInputEvent(prevActive);
                        prevInput->setPickerOpen(false);
                        uiDirty_ = true;
                        return; // consumed the click
                    } else if (inSwatch) {
                        prevInput->setPickerOpen(false);
                        uiDirty_ = true;
                        return; // consumed — don't fall through
                    } else {
                        prevInput->setPickerOpen(false);
                    }
                }
                prevInput->setFocused(false);
                uiDirty_ = true;
            }
            auto* prevTextarea = getElTextarea(prevActive);
            if (prevTextarea) {
                prevTextarea->setFocused(false);
                uiDirty_ = true;
            }
            auto* prevSelect = getElSelect(prevActive);
            if (prevSelect && prevSelect->isOpen()) {
                // Check if click is inside the dropdown
                auto dp = prevSelect->lastDrawPos();
                auto opts = prevSelect->getOptions();
                float lineH = prevSelect->dropdownLineHeight();
                float dropY = dp.y + dp.h;
                float dropH = lineH * static_cast<float>(opts.size()) + 2.0f;
                bool inDropdown = (x >= dp.x && x < dp.x + dp.w &&
                                   y >= dropY && y < dropY + dropH);
                if (inDropdown) {
                    // Select the clicked option
                    int idx = static_cast<int>((y - dropY - 1.0f) / lineH);
                    idx = std::clamp(idx, 0, static_cast<int>(opts.size()) - 1);
                    prevSelect->setSelectedIndex(idx);
                    prevSelect->setOpen(false);
                    // Update value attribute
                    if (prevActive) {
                        prevActive->setAttribute("value", opts[idx].value);
                        dom::Event changeEvt("change");
                        dispatchEvent(prevActive, changeEvt);
                        dispatchInputEvent(prevActive);
                    }
                    uiDirty_ = true;
                    // Don't process further — we handled the dropdown click
                    dispatchEvent(target, evt);
                    return;
                }
                prevSelect->setOpen(false);
                uiDirty_ = true;
            }

            // Set new active element and dispatch focus events
            document_->setActiveElement(target);
            if (target != prevActive) {
                dispatchFocusEvents(prevActive, target);
            }
            jsRuntime_->executePendingJobs();

            // Focus new input if clicking on one
            auto* newInput = getElInput(target);
            auto* newTextarea = getElTextarea(target);
            auto* newSelect = getElSelect(target);

            if (newInput) {
                newInput->setFocused(true);
                auto itype = newInput->inputType(target);

                if (itype == layout::ElInput::InputType::Checkbox) {
                    // Toggle checked state
                    if (target->hasAttribute("checked")) {
                        target->removeAttribute("checked");
                    } else {
                        target->setAttribute("checked", "");
                    }
                    dom::Event changeEvt("change");
                    dispatchEvent(target, changeEvt);
                    dispatchInputEvent(target);
                    uiDirty_ = true;
                } else if (itype == layout::ElInput::InputType::Radio) {
                    // Uncheck other radios with same name
                    std::string nameStr = target->getAttribute("name");
                    const char* name = nameStr.empty() ? nullptr : nameStr.c_str();
                    if (name && *name && document_) {
                        auto* body = document_->body();
                        if (body) {
                            auto radios = body->querySelectorAll("input[type=\"radio\"]");
                            for (auto* el : radios) {
                                if (el == target) continue;
                                auto* otherInput = getElInput(el);
                                if (otherInput) {
                                    std::string otherNameStr = el->getAttribute("name");
                                    if (!otherNameStr.empty() && otherNameStr == nameStr) {
                                        el->removeAttribute("checked");
                                    }
                                }
                            }
                        }
                    }
                    target->setAttribute("checked", "");
                    dom::Event changeEvt("change");
                    dispatchEvent(target, changeEvt);
                    dispatchInputEvent(target);
                    uiDirty_ = true;
                } else if (itype == layout::ElInput::InputType::Range) {
                    // Click to set value at position
                    auto dp = newInput->lastDrawPos();
                    float thumbR = 7.0f;
                    float trackStart = dp.x + thumbR;
                    float trackEnd = dp.x + dp.w - thumbR;
                    float pct = (trackEnd > trackStart) ?
                        std::clamp((x - trackStart) / (trackEnd - trackStart), 0.0f, 1.0f) : 0.0f;
                    float mn = newInput->rangeMin(), mx = newInput->rangeMax();
                    float val = mn + pct * (mx - mn);
                    // Snap to step
                    float step = newInput->rangeStep();
                    if (step > 0) {
                        val = mn + std::round((val - mn) / step) * step;
                        val = std::clamp(val, mn, mx);
                    }
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%g", static_cast<double>(val));
                    target->setAttribute("value", buf);
                    newInput->setDragging(true);
                    dispatchInputEvent(target);
                    uiDirty_ = true;
                } else if (itype == layout::ElInput::InputType::Color) {
                    if (newInput->isPickerOpen()) {
                        // Click inside picker to select color
                        auto dp = newInput->lastDrawPos();
                        float px = dp.x, py = dp.y + dp.h + 2;
                        float pw = 200.0f, ph = 160.0f;
                        if (x >= px && x < px + pw && y >= py && y < py + ph) {
                            std::string hex = pickColorFromGrid(x, y, px, py, pw, ph);
                            target->setAttribute("value", hex.c_str());
                            dom::Event changeEvt("change");
                            dispatchEvent(target, changeEvt);
                            dispatchInputEvent(target);
                        }
                        newInput->setPickerOpen(false);
                    } else {
                        newInput->setPickerOpen(true);
                    }
                    SDL_StopTextInput(window_->getSDLWindow());
                    uiDirty_ = true;
                } else if (newInput->isTextType(target)) {
                    // Check for number spin button click
                    if (newInput->inputType(target) == layout::ElInput::InputType::Number) {
                        auto dp = newInput->lastDrawPos();
                        float btnW = 16.0f;
                        float bx = dp.x + dp.w - btnW;
                        if (x >= bx && x <= dp.x + dp.w) {
                            // Click on spin buttons
                            std::string val = target->getAttribute("value");
                            float v = val.empty() ? 0 : static_cast<float>(atof(val.c_str()));
                            float step = newInput->rangeStep();
                            float midY = dp.y + dp.h / 2;
                            v += (y < midY) ? step : -step;
                            std::string minAttr = target->getAttribute("min");
                            std::string maxAttr = target->getAttribute("max");
                            if (!minAttr.empty()) v = std::max(v, newInput->rangeMin());
                            if (!maxAttr.empty()) v = std::min(v, newInput->rangeMax());
                            char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
                            target->setAttribute("value", buf);
                            newInput->setCursorPos(static_cast<int>(strlen(buf)));
                            dispatchInputEvent(target);
                        }
                    }
                    std::string valStr = target->getAttribute("value");
                    newInput->setCursorPos(static_cast<int>(valStr.size()));
                    SDL_StartTextInput(window_->getSDLWindow());
                    uiDirty_ = true;
                } else {
                    // Button types — no text input
                    SDL_StopTextInput(window_->getSDLWindow());
                    uiDirty_ = true;
                }
            } else if (newTextarea) {
                newTextarea->setFocused(true);
                std::string taValStr = target->getAttribute("value");
                newTextarea->setCursorPos(static_cast<int>(taValStr.size()));
                SDL_StartTextInput(window_->getSDLWindow());
                uiDirty_ = true;
            } else if (newSelect) {
                // Toggle dropdown open/close
                newSelect->setOpen(!newSelect->isOpen());
                if (newSelect->isOpen()) {
                    newSelect->setHighlightedIndex(newSelect->selectedIndex());
                }
                SDL_StopTextInput(window_->getSDLWindow());
                uiDirty_ = true;
            } else {
                SDL_StopTextInput(window_->getSDLWindow());
            }

            dispatchEvent(target, evt);
        }
    }
}

void Engine::handleMouseUp(float x, float y, int button) {
    // x, y = screen space. docX, docY = document space (see handleMouseDown).
    float docX = x, docY = y + scrollY_;
    uiDirty_ = true;

    // Update button bitmask
    pressedButtons_ &= ~(1 << button);

    // End scrollbar drags
    if (viewportScrollbar_.isDragging()) {
        viewportScrollbar_.endDrag();
        draggingViewportScrollbar_ = false;
        uiDirty_ = true;
    }
    if (elementScrollbar_.isDragging()) {
        elementScrollbar_.endDrag();
        scrollbarDragTarget_ = nullptr;
        uiDirty_ = true;
    }

    // Stop range slider dragging
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

    if (document_) {
        dom::Element* target = hitTest(docX, docY);
        int mod = SDL_GetModState();

        // Dispatch mouseup event
        {
            dom::MouseEvent upEvt("mouseup");
            populateMouseEvent(upEvt, x, y, button, pressedButtons_,
                              lastMouseX_, lastMouseY_, scrollY_, mod);
            if (target) {
                dispatchEvent(target, upEvt);
            }
        }

        // Dispatch click event (only if mouseup is on the same element as mousedown)
        if (target && target == mouseDownTarget_) {
            // Double-click detection
            double now = util::currentTimeMs();
            static constexpr double kDblClickThresholdMs = 500.0;
            static constexpr float kDblClickDistancePx = 5.0f;

            if (lastClickTarget_ == target &&
                (now - lastClickTimeMs_) < kDblClickThresholdMs &&
                std::abs(x - lastClickX_) < kDblClickDistancePx &&
                std::abs(y - lastClickY_) < kDblClickDistancePx) {
                clickCount_++;
            } else {
                clickCount_ = 1;
            }
            lastClickTimeMs_ = now;
            lastClickX_ = x;
            lastClickY_ = y;
            lastClickTarget_ = target;

            dom::MouseEvent clickEvt("click");
            populateMouseEvent(clickEvt, x, y, button, pressedButtons_,
                              lastMouseX_, lastMouseY_, scrollY_, mod);
            clickEvt.setDetail(clickCount_);
            dispatchEvent(target, clickEvt);

            // Dispatch dblclick on second click
            if (clickCount_ == 2) {
                dom::MouseEvent dblEvt("dblclick", true, true);
                populateMouseEvent(dblEvt, x, y, button, pressedButtons_,
                                  lastMouseX_, lastMouseY_, scrollY_, mod);
                dblEvt.setDetail(2);
                dispatchEvent(target, dblEvt);
            }

            // Dispatch contextmenu on right-click
            if (button == 2) {
                dom::MouseEvent ctxEvt("contextmenu", true, true);
                populateMouseEvent(ctxEvt, x, y, button, pressedButtons_,
                                  lastMouseX_, lastMouseY_, scrollY_, mod);
                dispatchEvent(target, ctxEvt);
            }
        }

        mouseDownTarget_ = nullptr;
        if (jsRuntime_) jsRuntime_->executePendingJobs();
    }
}

void Engine::handleMouseMove(float x, float y) {
    // x, y = screen space. docX, docY = document space (see handleMouseDown).
    float docX = x, docY = y + scrollY_;

    // Viewport scrollbar drag
    if (viewportScrollbar_.isDragging()) {
        float vh = static_cast<float>(viewportHeight_);
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(viewportWidth_) - vs.width - vs.margin,
            0.0f, vh, documentHeight_, vh, scrollY_);
        float maxScroll = std::max(0.0f, documentHeight_ - vh);
        scrollY_ = std::clamp(
            viewportScrollbar_.updateDrag(y, documentHeight_, vh, m),
            0.0f, maxScroll);
        uiDirty_ = true;
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    // Element scrollbar drag
    if (elementScrollbar_.isDragging() && scrollbarDragTarget_) {
        auto* elem = scrollbarDragTarget_;
        float viewH = elem->layoutBox().contentRect.height;
        float maxST = maxScrollTop(elem);
        float contentH = viewH + maxST;

        auto& lbox = elem->layoutBox();
        auto& es = elementScrollbar_.style();
        float bh = lbox.fullHeight();
        auto m = elementScrollbar_.layout(0, 0, bh, contentH, viewH,
            elem->scrollTopValue());
        float newScroll = elementScrollbar_.updateDrag(y, contentH, viewH, m);
        elem->setScrollTopValue(std::clamp(newScroll, 0.0f, maxST));
        uiDirty_ = true;
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    // Viewport scrollbar hover
    {
        float vh = static_cast<float>(viewportHeight_);
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(viewportWidth_) - vs.width - vs.margin,
            0.0f, vh, documentHeight_, vh, scrollY_);
        bool wasHovered = viewportScrollbar_.isHovered();
        viewportScrollbar_.setHovered(viewportScrollbar_.thumbHitTest(x, y, m));
        if (wasHovered != viewportScrollbar_.isHovered()) uiDirty_ = true;
    }

    // Element scrollbar hover (per-element tracking)
    if (document_ && document_->documentElement()) {
        ScrollbarMetrics em;
        dom::Element* hitElem = findElementScrollbarHit(
            document_->documentElement(), x, y,
            0.0f, -scrollY_, elementScrollbar_, em);
        dom::Element* prevHovered = scrollbarHoveredElement_;
        if (hitElem && elementScrollbar_.thumbHitTest(x, y, em)) {
            scrollbarHoveredElement_ = hitElem;
        } else {
            scrollbarHoveredElement_ = nullptr;
        }
        if (prevHovered != scrollbarHoveredElement_) uiDirty_ = true;
    }

    // Range slider dragging
    if (document_) {
        auto* activeEl = document_->activeElement();
        auto* rangeInput = getElInput(activeEl);
        if (rangeInput && rangeInput->isDragging()) {
            auto dp = rangeInput->lastDrawPos();
            float thumbR = 7.0f;
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

    // Update dropdown highlight on hover
    if (document_) {
        auto* activeEl = document_->activeElement();
        auto* select = getElSelect(activeEl);
        if (select && select->isOpen()) {
            auto dp = select->lastDrawPos();
            auto opts = select->getOptions();
            float lineH = select->dropdownLineHeight();
            float dropY = dp.y + dp.h;
            float dropH = lineH * static_cast<float>(opts.size()) + 2.0f;
            if (x >= dp.x && x < dp.x + dp.w && y >= dropY && y < dropY + dropH) {
                int idx = static_cast<int>((y - dropY - 1.0f) / lineH);
                idx = std::clamp(idx, 0, static_cast<int>(opts.size()) - 1);
                if (idx != select->highlightedIndex()) {
                    select->setHighlightedIndex(idx);
                    uiDirty_ = true;
                }
            }
        }
    }

    // Dispatch mousemove event
    if (document_) {
        dom::Element* target = hitTest(docX, docY);

        // Dispatch mouseover/mouseout when element changes (bubbling versions)
        if (target != hoveredElement_) {
            int mod = SDL_GetModState();

            // mouseout on previous element (bubbles)
            if (hoveredElement_) {
                dom::MouseEvent outEvt("mouseout", true, true);
                populateMouseEvent(outEvt, x, y, -1, pressedButtons_,
                                  lastMouseX_, lastMouseY_, scrollY_, mod);
                outEvt.setRelatedTarget(target);
                dispatchEvent(hoveredElement_, outEvt);
            }

            // mouseleave on previous element (doesn't bubble)
            if (hoveredElement_) {
                dom::MouseEvent leaveEvt("mouseleave", false, false);
                populateMouseEvent(leaveEvt, x, y, -1, pressedButtons_,
                                  lastMouseX_, lastMouseY_, scrollY_, mod);
                leaveEvt.setRelatedTarget(target);
                dispatchEvent(hoveredElement_, leaveEvt);
            }

            // mouseover on new element (bubbles)
            if (target) {
                dom::MouseEvent overEvt("mouseover", true, true);
                populateMouseEvent(overEvt, x, y, -1, pressedButtons_,
                                  lastMouseX_, lastMouseY_, scrollY_, mod);
                overEvt.setRelatedTarget(hoveredElement_);
                dispatchEvent(target, overEvt);
            }

            // mouseenter on new element (doesn't bubble)
            if (target) {
                dom::MouseEvent enterEvt("mouseenter", false, false);
                populateMouseEvent(enterEvt, x, y, -1, pressedButtons_,
                                  lastMouseX_, lastMouseY_, scrollY_, mod);
                enterEvt.setRelatedTarget(hoveredElement_);
                dispatchEvent(target, enterEvt);
            }

            hoveredElement_ = target;
            uiDirty_ = true;
        }

        // Always dispatch mousemove
        if (target) {
            int mod = SDL_GetModState();
            dom::MouseEvent moveEvt("mousemove", true, true);
            populateMouseEvent(moveEvt, x, y, -1, pressedButtons_,
                              lastMouseX_, lastMouseY_, scrollY_, mod);
            dispatchEvent(target, moveEvt);
        }

        if (jsRuntime_) jsRuntime_->executePendingJobs();
    }

    lastMouseX_ = x;
    lastMouseY_ = y;
}

// ---------------------------------------------------------------------------
// Keyboard events
// ---------------------------------------------------------------------------

// Helper: update input value and dispatch "input" event for v-model
void Engine::dispatchInputEvent(dom::Element* el, const std::string& data,
                                const std::string& inputType) {
    if (!el) return;
    dom::InputEvent evt("input");
    evt.setData(data);
    evt.setInputType(inputType);
    evt.setIsTrusted(true);
    dispatchEvent(el, evt);
    jsRuntime_->executePendingJobs();
    uiDirty_ = true;
}

void Engine::handleKeyDown(int keycode, int scancode, int mod, bool repeat) {
    // F8 toggles system overlay
    if (keycode == SDLK_F8 && !repeat) {
        if (systemOverlay_) {
            systemOverlay_->toggle();
            uiDirty_ = true;
        }
        return;
    }

    if (!document_) return;

    // Tab key: advance focus to next/previous focusable element
    if (keycode == SDLK_TAB) {
        advanceFocus((mod & SDL_KMOD_SHIFT) != 0);
        uiDirty_ = true;
        return;
    }

    // Check if a text input is focused — handle editing keys
    auto* activeEl = document_->activeElement();
    auto* input = getElInput(activeEl);

    // Handle checkbox/radio space toggle
    if (input && input->isFocused()) {
        auto itype = input->inputType(activeEl);
        if ((itype == layout::ElInput::InputType::Checkbox || itype == layout::ElInput::InputType::Radio)
            && keycode == SDLK_SPACE) {
            if (itype == layout::ElInput::InputType::Checkbox) {
                if (activeEl->hasAttribute("checked"))
                    activeEl->removeAttribute("checked");
                else
                    activeEl->setAttribute("checked", "");
            } else {
                activeEl->setAttribute("checked", "");
            }
            dom::Event changeEvt("change");
            dispatchEvent(activeEl, changeEvt);
            dispatchInputEvent(activeEl);

            auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
            dispatchEvent(activeEl, evt);
            return;
        }

        // Handle range arrow keys
        if (itype == layout::ElInput::InputType::Range) {
            bool handled = false;
            if (keycode == SDLK_LEFT || keycode == SDLK_DOWN) {
                float v = input->rangeValue() - input->rangeStep();
                v = std::clamp(v, input->rangeMin(), input->rangeMax());
                char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
                activeEl->setAttribute("value", buf);
                dispatchInputEvent(activeEl);
                handled = true;
            } else if (keycode == SDLK_RIGHT || keycode == SDLK_UP) {
                float v = input->rangeValue() + input->rangeStep();
                v = std::clamp(v, input->rangeMin(), input->rangeMax());
                char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
                activeEl->setAttribute("value", buf);
                dispatchInputEvent(activeEl);
                handled = true;
            }
            if (handled) {
                auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
                dispatchEvent(activeEl, evt);
                return;
            }
        }

        // Skip text editing for non-text types
        if (!input->isTextType(activeEl)) {
            auto nontextEvt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
            dispatchEvent(activeEl, nontextEvt);
            return;
        }
    }

    if (input && input->isFocused() && input->isTextType(activeEl)) {
        std::string val = activeEl->getAttribute("value");
        int pos = input->cursorPos();
        pos = std::clamp(pos, 0, static_cast<int>(val.size()));
        bool handled = false;

        if (keycode == SDLK_BACKSPACE) {
            if (pos > 0) {
                std::string deleted = val.substr(pos - 1, 1);
                val.erase(pos - 1, 1);
                input->setCursorPos(pos - 1);
                activeEl->setAttribute("value", val);
                dispatchInputEvent(activeEl, deleted, "deleteContentBackward");
            }
            handled = true;
        } else if (keycode == SDLK_DELETE) {
            if (pos < static_cast<int>(val.size())) {
                std::string deleted = val.substr(pos, 1);
                val.erase(pos, 1);
                activeEl->setAttribute("value", val);
                dispatchInputEvent(activeEl, deleted, "deleteContentForward");
            }
            handled = true;
        } else if (keycode == SDLK_LEFT) {
            if (pos > 0) {
                input->setCursorPos(pos - 1);
                uiDirty_ = true;
            }
            handled = true;
        } else if (keycode == SDLK_RIGHT) {
            if (pos < static_cast<int>(val.size())) {
                input->setCursorPos(pos + 1);
                uiDirty_ = true;
            }
            handled = true;
        } else if (keycode == SDLK_HOME) {
            input->setCursorPos(0);
            uiDirty_ = true;
            handled = true;
        } else if (keycode == SDLK_END) {
            input->setCursorPos(static_cast<int>(val.size()));
            uiDirty_ = true;
            handled = true;
        } else if (input->inputType(activeEl) == layout::ElInput::InputType::Number &&
                   (keycode == SDLK_UP || keycode == SDLK_DOWN)) {
            // Increment/decrement number value
            float v = 0;
            if (!val.empty()) v = static_cast<float>(atof(val.c_str()));
            float step = input->rangeStep();
            v += (keycode == SDLK_UP) ? step : -step;
            // Clamp to min/max if specified
            float mn = input->rangeMin(), mx = input->rangeMax();
            std::string minAttrStr = activeEl->getAttribute("min");
            std::string maxAttrStr = activeEl->getAttribute("max");
            if (!minAttrStr.empty()) v = std::max(v, mn);
            if (!maxAttrStr.empty()) v = std::min(v, mx);
            char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
            activeEl->setAttribute("value", buf);
            input->setCursorPos(static_cast<int>(strlen(buf)));
            dispatchInputEvent(activeEl);
            handled = true;
        } else if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
            // Unfocus the input on Enter
            input->setFocused(false);
            dispatchFocusEvents(activeEl, nullptr);
            SDL_StopTextInput(window_->getSDLWindow());
            uiDirty_ = true;
            handled = true;
        } else if (keycode == SDLK_ESCAPE) {
            // Unfocus on Escape
            input->setFocused(false);
            dispatchFocusEvents(activeEl, nullptr);
            SDL_StopTextInput(window_->getSDLWindow());
            uiDirty_ = true;
            handled = true;
        } else if ((mod & SDL_KMOD_CTRL) && keycode == SDLK_A) {
            // Ctrl+A: select all (move cursor to end for now)
            input->setCursorPos(static_cast<int>(val.size()));
            uiDirty_ = true;
            handled = true;
        }

        if (handled) {
            // Still dispatch keydown event for JS listeners
            auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
            dispatchEvent(activeEl, evt);
            return;
        }
    }

    // Check if a textarea is focused — handle multi-line editing keys
    auto* textarea = getElTextarea(activeEl);
    if (textarea && textarea->isFocused()) {
        std::string val = activeEl->getAttribute("value");
        int pos = textarea->cursorPos();
        pos = std::clamp(pos, 0, static_cast<int>(val.size()));
        bool handled = false;

        if (keycode == SDLK_BACKSPACE) {
            if (pos > 0) {
                std::string deleted = val.substr(pos - 1, 1);
                val.erase(pos - 1, 1);
                textarea->setCursorPos(pos - 1);
                activeEl->setAttribute("value", val);
                dispatchInputEvent(activeEl, deleted, "deleteContentBackward");
            }
            handled = true;
        } else if (keycode == SDLK_DELETE) {
            if (pos < static_cast<int>(val.size())) {
                std::string deleted = val.substr(pos, 1);
                val.erase(pos, 1);
                activeEl->setAttribute("value", val);
                dispatchInputEvent(activeEl, deleted, "deleteContentForward");
            }
            handled = true;
        } else if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
            // Insert newline in textarea
            val.insert(pos, 1, '\n');
            textarea->setCursorPos(pos + 1);
            activeEl->setAttribute("value", val);
            dispatchInputEvent(activeEl, "\n", "insertLineBreak");
            handled = true;
        } else if (keycode == SDLK_LEFT) {
            if (pos > 0) {
                textarea->setCursorPos(pos - 1);
                uiDirty_ = true;
            }
            handled = true;
        } else if (keycode == SDLK_RIGHT) {
            if (pos < static_cast<int>(val.size())) {
                textarea->setCursorPos(pos + 1);
                uiDirty_ = true;
            }
            handled = true;
        } else if (keycode == SDLK_UP) {
            // Move cursor up one line
            int line = 0, col = 0;
            for (int i = 0; i < pos; ++i) {
                if (val[i] == '\n') { ++line; col = 0; } else { ++col; }
            }
            if (line > 0) {
                // Find start of previous line
                int prevLineStart = 0, prevLineLen = 0;
                int curLine = 0;
                for (int i = 0; i <= static_cast<int>(val.size()); ++i) {
                    if (curLine == line - 1) { prevLineStart = i; break; }
                    if (i < static_cast<int>(val.size()) && val[i] == '\n') ++curLine;
                }
                // Find length of previous line
                for (int i = prevLineStart; i < static_cast<int>(val.size()) && val[i] != '\n'; ++i)
                    ++prevLineLen;
                textarea->setCursorPos(prevLineStart + std::min(col, prevLineLen));
                uiDirty_ = true;
            }
            handled = true;
        } else if (keycode == SDLK_DOWN) {
            // Move cursor down one line
            int line = 0, col = 0;
            for (int i = 0; i < pos; ++i) {
                if (val[i] == '\n') { ++line; col = 0; } else { ++col; }
            }
            // Find start of next line
            int nextLineStart = -1;
            int curLine = 0;
            for (int i = 0; i < static_cast<int>(val.size()); ++i) {
                if (val[i] == '\n') {
                    if (curLine == line) { nextLineStart = i + 1; break; }
                    ++curLine;
                }
            }
            if (nextLineStart >= 0) {
                int nextLineLen = 0;
                for (int i = nextLineStart; i < static_cast<int>(val.size()) && val[i] != '\n'; ++i)
                    ++nextLineLen;
                textarea->setCursorPos(nextLineStart + std::min(col, nextLineLen));
                uiDirty_ = true;
            }
            handled = true;
        } else if (keycode == SDLK_HOME) {
            // Move to start of current line
            int lineStart = pos;
            while (lineStart > 0 && val[lineStart - 1] != '\n') --lineStart;
            textarea->setCursorPos(lineStart);
            uiDirty_ = true;
            handled = true;
        } else if (keycode == SDLK_END) {
            // Move to end of current line
            int lineEnd = pos;
            while (lineEnd < static_cast<int>(val.size()) && val[lineEnd] != '\n') ++lineEnd;
            textarea->setCursorPos(lineEnd);
            uiDirty_ = true;
            handled = true;
        } else if (keycode == SDLK_ESCAPE) {
            textarea->setFocused(false);
            dispatchFocusEvents(activeEl, nullptr);
            SDL_StopTextInput(window_->getSDLWindow());
            uiDirty_ = true;
            handled = true;
        } else if ((mod & SDL_KMOD_CTRL) && keycode == SDLK_A) {
            textarea->setCursorPos(static_cast<int>(val.size()));
            uiDirty_ = true;
            handled = true;
        }

        if (handled) {
            auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
            dispatchEvent(activeEl, evt);
            return;
        }
    }

    // Check if a select is open — handle arrow keys and enter
    auto* select = getElSelect(activeEl);
    if (select && select->isOpen()) {
        auto opts = select->getOptions();
        int hi = select->highlightedIndex();
        bool handled = false;

        if (keycode == SDLK_DOWN) {
            if (hi < static_cast<int>(opts.size()) - 1) {
                select->setHighlightedIndex(hi + 1);
                uiDirty_ = true;
            }
            handled = true;
        } else if (keycode == SDLK_UP) {
            if (hi > 0) {
                select->setHighlightedIndex(hi - 1);
                uiDirty_ = true;
            }
            handled = true;
        } else if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
            if (hi >= 0 && hi < static_cast<int>(opts.size())) {
                select->setSelectedIndex(hi);
                activeEl->setAttribute("value", opts[hi].value);
                dom::Event changeEvt("change");
                dispatchEvent(activeEl, changeEvt);
                dispatchInputEvent(activeEl);
            }
            select->setOpen(false);
            uiDirty_ = true;
            handled = true;
        } else if (keycode == SDLK_ESCAPE) {
            select->setOpen(false);
            uiDirty_ = true;
            handled = true;
        }

        if (handled) {
            auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
            dispatchEvent(activeEl, evt);
            return;
        }
    }

    // Default: dispatch keydown to body
    auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
    dom::Element* target = document_->body();
    if (target) {
        dispatchEvent(target, evt);
    }
}

void Engine::handleKeyUp(int keycode, int scancode, int mod, bool repeat) {
    if (!document_) return;

    // Dispatch keyup to the focused input if any, otherwise body
    auto evt = makeKeyboardEvent("keyup", keycode, scancode, mod, repeat);

    auto* activeEl = document_->activeElement();
    bool focusedControl = false;
    if (auto* input = getElInput(activeEl)) focusedControl = input->isFocused();
    if (auto* ta = getElTextarea(activeEl)) focusedControl = focusedControl || ta->isFocused();
    if (auto* sel = getElSelect(activeEl)) focusedControl = focusedControl || sel->isOpen();
    dom::Element* target = focusedControl ? activeEl : document_->body();
    if (target) {
        dispatchEvent(target, evt);
    }
}

// Filter out control characters (tab, etc.) that shouldn't be inserted as text
static bool isControlChar(const std::string& text) {
    if (text.empty()) return true;
    unsigned char c = static_cast<unsigned char>(text[0]);
    // Allow printable ASCII and multi-byte UTF-8 sequences
    if (text.size() == 1 && c < 0x20 && c != '\n') return true; // control chars except newline
    if (text.size() == 1 && c == 0x7f) return true; // DEL
    return false;
}

// Validate text for number input (digits, minus, decimal point, 'e'/'E')
static bool isValidNumberChar(const std::string& text) {
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || c == '-' || c == '.' || c == 'e' || c == 'E' || c == '+'))
            return false;
    }
    return !text.empty();
}

void Engine::handleTextInput(const std::string& text) {
    if (!document_) return;

    // Filter control characters for all inputs
    if (isControlChar(text)) return;

    auto* activeEl = document_->activeElement();

    // Try textarea first (also text-editable)
    auto* textarea = getElTextarea(activeEl);
    if (textarea && textarea->isFocused()) {
        std::string val = activeEl->getAttribute("value");
        int pos = std::clamp(textarea->cursorPos(), 0, static_cast<int>(val.size()));
        val.insert(pos, text);
        textarea->setCursorPos(pos + static_cast<int>(text.size()));
        activeEl->setAttribute("value", val);
        dispatchInputEvent(activeEl, text, "insertText");
        return;
    }

    auto* input = getElInput(activeEl);
    if (!input || !input->isFocused() || !input->isTextType(activeEl)) return;

    // Number type: only allow numeric characters
    if (input->inputType(activeEl) == layout::ElInput::InputType::Number) {
        if (!isValidNumberChar(text)) return;
    }

    // Insert text at cursor position
    std::string val = activeEl->getAttribute("value");
    int pos = std::clamp(input->cursorPos(), 0, static_cast<int>(val.size()));
    val.insert(pos, text);
    input->setCursorPos(pos + static_cast<int>(text.size()));
    activeEl->setAttribute("value", val);
    dispatchInputEvent(activeEl, text, "insertText");
}

// ---------------------------------------------------------------------------
// Tab focus navigation
// ---------------------------------------------------------------------------

void Engine::advanceFocus(bool reverse) {
    if (!document_) return;

    // Build list of focusable elements in DOM order
    std::vector<dom::Element*> focusable;
    auto* body = document_->body();
    if (!body) return;

    // Collect all elements via querySelectorAll for common focusable tags
    auto inputs = body->querySelectorAll("input");
    auto textareas = body->querySelectorAll("textarea");
    auto selects = body->querySelectorAll("select");
    auto buttons = body->querySelectorAll("button");

    // Merge into a single list — we need DOM order, so collect all elements
    // and filter. Use a simple recursive walk.
    std::function<void(dom::Node*)> walk = [&](dom::Node* node) {
        if (!node) return;
        if (node->nodeType() == dom::NodeType::Element) {
            auto* el = static_cast<dom::Element*>(node);
            std::string tag = el->tagName();
            for (auto& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool isFocusable = (tag == "input" || tag == "textarea" || tag == "select" || tag == "button");
            if (isFocusable) {
                // Skip hidden inputs
                auto* inp = getElInput(el);
                if (inp && inp->inputType(el) == layout::ElInput::InputType::Hidden)
                    isFocusable = false;
                // Skip disabled
                if (el->getAttribute("disabled") == "true" || el->attributes().count("disabled"))
                    isFocusable = false;
            }
            if (isFocusable) focusable.push_back(el);
        }
        for (auto* child : node->childNodes()) walk(child);
    };
    walk(body);

    if (focusable.empty()) return;

    // Find current active element
    auto* activeEl = document_->activeElement();
    int currentIdx = -1;
    for (int i = 0; i < static_cast<int>(focusable.size()); ++i) {
        if (focusable[i] == activeEl) { currentIdx = i; break; }
    }

    // Compute next index
    int nextIdx;
    if (reverse) {
        nextIdx = (currentIdx <= 0) ? static_cast<int>(focusable.size()) - 1 : currentIdx - 1;
    } else {
        nextIdx = (currentIdx < 0 || currentIdx >= static_cast<int>(focusable.size()) - 1) ? 0 : currentIdx + 1;
    }

    auto* nextEl = focusable[nextIdx];

    // Unfocus current
    if (activeEl) {
        auto* prevInput = getElInput(activeEl);
        if (prevInput) prevInput->setFocused(false);
        auto* prevTa = getElTextarea(activeEl);
        if (prevTa) prevTa->setFocused(false);
        auto* prevSel = getElSelect(activeEl);
        if (prevSel) prevSel->setOpen(false);
    }

    // Focus next
    document_->setActiveElement(nextEl);
    dispatchFocusEvents(activeEl, nextEl);

    auto* newInput = getElInput(nextEl);
    auto* newTa = getElTextarea(nextEl);

    if (newInput) {
        newInput->setFocused(true);
        if (newInput->isTextType(nextEl)) {
            std::string v = nextEl->getAttribute("value");
            newInput->setCursorPos(static_cast<int>(v.size()));
            SDL_StartTextInput(window_->getSDLWindow());
        } else {
            SDL_StopTextInput(window_->getSDLWindow());
        }
    } else if (newTa) {
        newTa->setFocused(true);
        std::string v = nextEl->getAttribute("value");
        newTa->setCursorPos(static_cast<int>(v.size()));
        SDL_StartTextInput(window_->getSDLWindow());
    } else {
        SDL_StopTextInput(window_->getSDLWindow());
    }

    uiDirty_ = true;
}

// ---------------------------------------------------------------------------
// Mouse wheel
// ---------------------------------------------------------------------------

void Engine::handleWheel(float x, float y, float dx, float dy) {
    if (!document_) return;

    float docX = x, docY = y + scrollY_;
    dom::Element* target = hitTest(docX, docY);

    // Dispatch wheel event to JS
    if (target) {
        dom::WheelEvent wheelEvt("wheel", true, true);
        int mod = SDL_GetModState();
        populateMouseEvent(wheelEvt, x, y, -1, pressedButtons_,
                          lastMouseX_, lastMouseY_, scrollY_, mod);
        // SDL gives scroll amounts in lines; convert to pixels for DOM_DELTA_PIXEL
        static constexpr float kPixelsPerLine = 48.0f;
        wheelEvt.setDeltaX(static_cast<double>(-dx * kPixelsPerLine));
        wheelEvt.setDeltaY(static_cast<double>(-dy * kPixelsPerLine));
        wheelEvt.setDeltaZ(0.0);
        wheelEvt.setDeltaMode(dom::WheelEvent::DOM_DELTA_PIXEL);
        dispatchEvent(target, wheelEvt);

        // If JS called preventDefault(), don't do default scrolling
        if (wheelEvt.defaultPrevented()) {
            if (jsRuntime_) jsRuntime_->executePendingJobs();
            return;
        }
    }

    // Check if mouse is over a focused textarea
    auto* activeEl = document_->activeElement();
    auto* textarea = getElTextarea(activeEl);
    if (textarea && textarea->isFocused()) {
        float lineH = 16.0f;
        float scroll = textarea->scrollY() - dy * lineH * 3.0f;
        scroll = std::max(scroll, 0.0f);
        textarea->setScrollY(scroll);
        uiDirty_ = true;
        return;
    }

    // Also allow scrolling textarea under mouse cursor (not just active one)
    auto* hoverTa = getElTextarea(target);
    if (hoverTa) {
        float lineH = 16.0f;
        float scroll = hoverTa->scrollY() - dy * lineH * 3.0f;
        scroll = std::max(scroll, 0.0f);
        hoverTa->setScrollY(scroll);
        uiDirty_ = true;
        return;
    }

    // Check if target or an ancestor is a scrollable overflow element
    {
        auto* el = target;
        bool blockedByHidden = false;
        while (el) {
            std::string ov = getOverflowY(el->computedStyle());
            if (overflowScrollable(ov)) {
                float maxST = maxScrollTop(el);
                if (maxST <= 0) break; // content fits, no scrolling needed
                float scrollPx = -dy * kScrollSpeed;
                float prevScroll = el->scrollTopValue();
                float newScroll = std::clamp(prevScroll + scrollPx, 0.0f, maxST);
                el->setScrollTopValue(newScroll);
                if (newScroll != prevScroll) {
                    dispatchScrollEvent(el);
                }
                uiDirty_ = true;
                return;
            }
            if (ov == "hidden") {
                blockedByHidden = true;
                break;
            }
            el = composedParent(el);
        }

        // overflow:hidden ancestor blocks viewport scroll too
        if (blockedByHidden) return;
    }

    // Viewport scrolling
    float maxScroll = std::max(0.0f, documentHeight_ - static_cast<float>(viewportHeight_));
    float prevScroll = scrollY_;
    scrollY_ = std::clamp(scrollY_ - dy * kScrollSpeed, 0.0f, maxScroll);
    if (scrollY_ != prevScroll) {
        // Dispatch scroll event on document element
        if (document_->documentElement()) {
            dispatchScrollEvent(document_->documentElement());
        }
    }
    uiDirty_ = true;
}

} // namespace bro::engine
