// Shared replaced-element initialization and interaction logic.
// Used by the Engine for both the app document and system panels.

#include "engine/replaced_elements.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "engine/color_picker_overlay.h"
#include "engine/dropdown_overlay.h"
#include "engine/overlay.h"
#include "layout/el_select.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_svg.h"
#include "layout/el_video.h"
#include "js/dom_bindings.h"
#include "js/event_dispatch.h"
#include "platform/sdl_window.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace bro::engine {

// ---------------------------------------------------------------------------
// Replaced element initialization
// ---------------------------------------------------------------------------

void ensureReplacedElements(dom::Element* elem, render::Renderer* renderer,
                            JSContext* jsCtx, broaudio::Engine* audioEngine) {
    if (!elem) return;

    const auto& tag = elem->tagName();

    if (tag == "INPUT" && !elem->inputControl()) {
        auto ctrl = std::make_unique<layout::ElInput>(renderer);
        ctrl->setElement(elem);
        elem->setInputControl(std::move(ctrl));
    } else if (tag == "TEXTAREA" && !elem->textareaControl()) {
        auto ctrl = std::make_unique<layout::ElTextarea>(renderer);
        ctrl->setElement(elem);
        elem->setTextareaControl(std::move(ctrl));
    } else if (tag == "SELECT" && !elem->selectControl()) {
        auto ctrl = std::make_unique<layout::ElSelect>(renderer);
        ctrl->setElement(elem);
        ctrl->initSelectedIndex();
        elem->setSelectControl(std::move(ctrl));
    } else if ((tag == "SVG" || tag == "svg") && !elem->svgControl()) {
        auto ctrl = std::make_unique<layout::ElSvg>(renderer);
        ctrl->setElement(elem);
        ctrl->parseAttributes();
        elem->setSvgControl(std::move(ctrl));
    } else if ((tag == "VIDEO" || tag == "video") && !elem->videoControl()) {
        auto ctrl = std::make_unique<layout::ElVideo>(renderer);
        ctrl->setElement(elem);
        ctrl->setJsContext(jsCtx);
        ctrl->setAudioEngine(audioEngine);
        // If the element already has a src attribute, load it now.
        // Otherwise the JS binding will trigger load when src is set.
        std::string src = elem->getAttribute("src");
        if (!src.empty()) ctrl->load(src);
        elem->setVideoControl(std::move(ctrl));
    }

    // Recurse into children
    for (auto* child : elem->childNodes()) {
        if (child->nodeType() == dom::NodeType::Element) {
            ensureReplacedElements(static_cast<dom::Element*>(child), renderer,
                                   jsCtx, audioEngine);
        }
    }

    // Recurse into shadow DOM
    if (elem->hasShadow()) {
        auto* sr = elem->shadowRoot();
        for (auto* child : sr->childNodes()) {
            if (child->nodeType() == dom::NodeType::Element) {
                ensureReplacedElements(static_cast<dom::Element*>(child),
                                       renderer, jsCtx, audioEngine);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void safeStartTextInput(platform::Window* window) {
    if (window) SDL_StartTextInput(window->getSDLWindow());
}
static void safeStopTextInput(platform::Window* window) {
    if (window) SDL_StopTextInput(window->getSDLWindow());
}


// ---------------------------------------------------------------------------
// Event dispatch helpers
// ---------------------------------------------------------------------------

void dispatchControlEvent(const ControlContext& ctx, dom::Element* el,
                          dom::Event& event) {
    if (!el || !ctx.jsCtx) return;
    js::dispatchDomEvent(ctx.jsCtx, el, event);
}

void dispatchInputEvent(const ControlContext& ctx, dom::Element* el,
                        const std::string& data,
                        const std::string& inputType) {
    if (!el) return;
    dom::InputEvent evt("input");
    evt.setData(data);
    evt.setInputType(inputType);
    evt.setIsTrusted(true);
    dispatchControlEvent(ctx, el, evt);
    *ctx.dirtyFlag = true;
}

void dispatchFocusEvents(const ControlContext& ctx,
                         dom::Element* oldTarget, dom::Element* newTarget) {
    if (oldTarget == newTarget) return;

    if (oldTarget) {
        dom::FocusEvent blurEvt("blur", false, false);
        blurEvt.setRelatedTarget(newTarget);
        blurEvt.setIsTrusted(true);
        dispatchControlEvent(ctx, oldTarget, blurEvt);
    }
    if (newTarget) {
        dom::FocusEvent focusEvt("focus", false, false);
        focusEvt.setRelatedTarget(oldTarget);
        focusEvt.setIsTrusted(true);
        dispatchControlEvent(ctx, newTarget, focusEvt);
    }
    if (oldTarget) {
        dom::FocusEvent focusoutEvt("focusout", true, false);
        focusoutEvt.setRelatedTarget(newTarget);
        focusoutEvt.setIsTrusted(true);
        dispatchControlEvent(ctx, oldTarget, focusoutEvt);
    }
    if (newTarget) {
        dom::FocusEvent focusinEvt("focusin", true, false);
        focusinEvt.setRelatedTarget(oldTarget);
        focusinEvt.setIsTrusted(true);
        dispatchControlEvent(ctx, newTarget, focusinEvt);
    }
}

// ---------------------------------------------------------------------------
// Unfocus previous control
// ---------------------------------------------------------------------------

ClickDisposition unfocusPreviousControl(
    const ControlContext& ctx,
    dom::Element* prevActive)
{
    auto* prevInput = getElInput(prevActive);
    if (prevInput) {
        prevInput->setFocused(false);
        *ctx.dirtyFlag = true;
    }

    auto* prevTextarea = getElTextarea(prevActive);
    if (prevTextarea) {
        prevTextarea->setFocused(false);
        *ctx.dirtyFlag = true;
    }

    // Select dropdowns and color pickers are now managed by OverlayManager;
    // click-outside dismissal is handled there, so nothing to do here.

    return ClickDisposition::PassThrough;
}

// ---------------------------------------------------------------------------
// Focus new control
// ---------------------------------------------------------------------------

void focusNewControl(
    const ControlContext& ctx,
    dom::Element* target,
    float x, float y)
{
    auto* newInput = getElInput(target);
    auto* newTextarea = getElTextarea(target);
    auto* newSelect = getElSelect(target);

    if (newInput) {
        newInput->setFocused(true);
        auto itype = newInput->inputType(target);

        if (itype == layout::ElInput::InputType::Checkbox) {
            if (target->hasAttribute("checked")) {
                target->removeAttribute("checked");
            } else {
                target->setAttribute("checked", "");
            }
            dom::Event changeEvt("change");
            dispatchControlEvent(ctx, target, changeEvt);
            dispatchInputEvent(ctx, target);
            *ctx.dirtyFlag = true;
        } else if (itype == layout::ElInput::InputType::Radio) {
            std::string nameStr = target->getAttribute("name");
            const char* name = nameStr.empty() ? nullptr : nameStr.c_str();
            if (name && *name && ctx.document) {
                auto* body = ctx.document->body();
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
            dispatchControlEvent(ctx, target, changeEvt);
            dispatchInputEvent(ctx, target);
            *ctx.dirtyFlag = true;
        } else if (itype == layout::ElInput::InputType::Range) {
            auto dp = newInput->lastDrawPos();
            float thumbR = 7.0f;
            float trackStart = dp.x + thumbR;
            float trackEnd = dp.x + dp.w - thumbR;
            float pct = (trackEnd > trackStart) ?
                std::clamp((x - trackStart) / (trackEnd - trackStart), 0.0f, 1.0f) : 0.0f;
            float mn = newInput->rangeMin(), mx = newInput->rangeMax();
            float val = mn + pct * (mx - mn);
            float step = newInput->rangeStep();
            if (step > 0) {
                val = mn + std::round((val - mn) / step) * step;
                val = std::clamp(val, mn, mx);
            }
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", static_cast<double>(val));
            target->setAttribute("value", buf);
            newInput->setDragging(true);
            dispatchInputEvent(ctx, target);
            *ctx.dirtyFlag = true;
        } else if (itype == layout::ElInput::InputType::Color) {
            // Open the engine color picker overlay anchored to this swatch.
            // Supports alpha when the element has a `data-alpha` attribute.
            if (ctx.overlays) {
                auto dp = newInput->lastDrawPos();
                std::string current = target->getAttribute("value");
                if (current.empty()) current = "#000000";
                bool withAlpha = target->hasAttribute("data-alpha");

                JSContext* jsCtx = ctx.jsCtx;
                bool* dirtyFlag = ctx.dirtyFlag;
                dom::Element* elem = target;

                auto onInput = [jsCtx, dirtyFlag, elem](const std::string& hex) {
                    elem->setAttribute("value", hex);
                    if (jsCtx) {
                        dom::InputEvent evt("input");
                        evt.setData(hex);
                        evt.setIsTrusted(true);
                        js::dispatchDomEvent(jsCtx, elem, evt);
                    }
                    if (dirtyFlag) *dirtyFlag = true;
                };
                auto onCommit = [jsCtx, dirtyFlag, elem](const std::string& hex) {
                    elem->setAttribute("value", hex);
                    if (jsCtx) {
                        dom::Event evt("change");
                        evt.setIsTrusted(true);
                        js::dispatchDomEvent(jsCtx, elem, evt);
                    }
                    if (dirtyFlag) *dirtyFlag = true;
                };

                auto picker = std::make_unique<ColorPickerOverlay>(
                    dp.x, dp.y, dp.w, dp.h,
                    static_cast<float>(ctx.viewportW),
                    static_cast<float>(ctx.viewportH),
                    current, withAlpha,
                    std::move(onInput), std::move(onCommit));
                ctx.overlays->open(std::move(picker), ctx.overlayContext,
                                   ctx.renderer);

                // Enable SDL text input so the hex field receives characters.
                safeStartTextInput(ctx.window);
            }
            *ctx.dirtyFlag = true;
        } else if (newInput->isTextType(target)) {
            // Number spin button click
            if (newInput->inputType(target) == layout::ElInput::InputType::Number) {
                auto dp = newInput->lastDrawPos();
                float btnW = 16.0f;
                float bx = dp.x + dp.w - btnW;
                if (x >= bx && x <= dp.x + dp.w) {
                    std::string val = target->getAttribute("value");
                    float v = val.empty() ? 0 : static_cast<float>(atof(val.c_str()));
                    float step = newInput->rangeStep();
                    float midY = dp.y + dp.h / 2;
                    v += (y < midY) ? step : -step;
                    std::string minAttr = target->getAttribute("min");
                    std::string maxAttr = target->getAttribute("max");
                    if (!minAttr.empty()) v = std::max(v, newInput->rangeMin());
                    if (!maxAttr.empty()) v = std::min(v, newInput->rangeMax());
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
                    target->setAttribute("value", buf);
                    newInput->setCursorPos(static_cast<int>(strlen(buf)));
                    dispatchInputEvent(ctx, target);
                }
            }
            std::string valStr = target->getAttribute("value");
            newInput->setCursorPos(static_cast<int>(valStr.size()));
            safeStartTextInput(ctx.window);
            *ctx.dirtyFlag = true;
        } else {
            // Button types — no text input
            safeStopTextInput(ctx.window);
            *ctx.dirtyFlag = true;
        }
    } else if (newTextarea) {
        newTextarea->setFocused(true);
        std::string taValStr = target->getAttribute("value");
        newTextarea->setCursorPos(static_cast<int>(taValStr.size()));
        safeStartTextInput(ctx.window);
        *ctx.dirtyFlag = true;
    } else if (newSelect) {
        // Open the engine dropdown overlay anchored to this select.
        if (ctx.overlays) {
            auto dp = newSelect->lastDrawPos();
            auto opts = newSelect->getOptions();
            std::vector<DropdownOverlay::Option> ddOpts;
            ddOpts.reserve(opts.size());
            for (auto& o : opts) ddOpts.push_back({o.value, o.text});

            // Resolve font family / size from computed style.
            std::string family = "Arial";
            float fontSize = 16.0f;
            {
                auto& style = target->computedStyle();
                auto it = style.find("font-family");
                if (it != style.end() && !it->second.empty()) family = it->second;
                auto sit = style.find("font-size");
                if (sit != style.end()) {
                    char* end = nullptr;
                    float v = std::strtof(sit->second.c_str(), &end);
                    if (end != sit->second.c_str() && v > 0) fontSize = v;
                }
            }

            JSContext* jsCtx = ctx.jsCtx;
            bool* dirtyFlag = ctx.dirtyFlag;
            dom::Element* elem = target;
            std::vector<DropdownOverlay::Option> optsCopy = ddOpts;
            layout::ElSelect* sel = newSelect;

            auto onSelect = [jsCtx, dirtyFlag, elem, optsCopy, sel](int index) {
                if (index < 0 || index >= static_cast<int>(optsCopy.size())) return;
                sel->setSelectedIndex(index);
                elem->setAttribute("value", optsCopy[index].value);
                if (jsCtx) {
                    dom::Event changeEvt("change");
                    changeEvt.setIsTrusted(true);
                    js::dispatchDomEvent(jsCtx, elem, changeEvt);
                    dom::InputEvent inputEvt("input");
                    inputEvt.setData(optsCopy[index].value);
                    inputEvt.setIsTrusted(true);
                    js::dispatchDomEvent(jsCtx, elem, inputEvt);
                }
                if (dirtyFlag) *dirtyFlag = true;
            };

            auto dd = std::make_unique<DropdownOverlay>(
                dp.x, dp.y, dp.w, dp.h,
                static_cast<float>(ctx.viewportW),
                static_cast<float>(ctx.viewportH),
                std::move(ddOpts),
                newSelect->selectedIndex(),
                std::move(family), fontSize,
                std::move(onSelect));
            ctx.overlays->open(std::move(dd), ctx.overlayContext, ctx.renderer);
        }
        safeStopTextInput(ctx.window);
        *ctx.dirtyFlag = true;
    } else {
        safeStopTextInput(ctx.window);
    }
}

// ---------------------------------------------------------------------------
// Per-document mouse press/release dispatch
// ---------------------------------------------------------------------------

void applyMouseOffset(dom::MouseEvent& evt, dom::Element* target) {
    if (!target) return;
    auto& box = target->layoutBox();
    float absX = box.contentRect.x;
    float absY = box.contentRect.y;
    for (auto* lp = target->layoutParent(); lp; lp = lp->layoutParent()) {
        auto& pb = lp->layoutBox();
        absX += pb.contentRect.x;
        absY += pb.contentRect.y;
        absY -= lp->scrollTopValue();
    }
    evt.setOffsetX(evt.clientX() - static_cast<double>(absX));
    evt.setOffsetY(evt.clientY() - static_cast<double>(absY));
}

bool dispatchDocMousePress(
    const ControlContext& ctx,
    MouseDispatchState& state,
    dom::Element* target,
    dom::MouseEvent& evt,
    float focusX, float focusY) {

    if (!ctx.document) { state.mouseDownTarget = nullptr; return false; }
    if (!target) { state.mouseDownTarget = nullptr; return false; }

    auto* prevActive = ctx.document->activeElement();

    // Give the previously-active control a chance to consume (e.g. dropdown
    // option selection). If consumed, still fire mousedown and bail — caller
    // should not treat the press as normal activation.
    auto disp = unfocusPreviousControl(ctx, prevActive);
    if (disp == ClickDisposition::Consumed) {
        js::dispatchDomEvent(ctx.jsCtx, target, evt);
        state.mouseDownTarget = target;
        if (ctx.dirtyFlag) *ctx.dirtyFlag = true;
        return true;
    }

    ctx.document->setActiveElement(target);
    if (target != prevActive) {
        dispatchFocusEvents(ctx, prevActive, target);
    }

    focusNewControl(ctx, target, focusX, focusY);

    js::dispatchDomEvent(ctx.jsCtx, target, evt);
    state.mouseDownTarget = target;
    if (ctx.dirtyFlag) *ctx.dirtyFlag = true;
    return false;
}

void dispatchDocMouseRelease(
    const ControlContext& ctx,
    MouseDispatchState& state,
    dom::Element* target,
    dom::MouseEvent& upEvt,
    float clientX, float clientY,
    int button, int buttons, int mod,
    float movementX, float movementY,
    float pageX, float pageY,
    double nowMs,
    double dblThresholdMs,
    float dblDistPx) {

    if (target && ctx.jsCtx) {
        js::dispatchDomEvent(ctx.jsCtx, target, upEvt);
    }

    // Click fires only when mouseup lands on the same element as mousedown.
    bool sameTarget = (target && target == state.mouseDownTarget);
    if (sameTarget) {
        // Rolling double-click detection.
        if (state.lastClickTarget == target &&
            (nowMs - state.lastClickTimeMs) < dblThresholdMs &&
            std::fabs(clientX - state.lastClickX) < dblDistPx &&
            std::fabs(clientY - state.lastClickY) < dblDistPx) {
            state.clickCount++;
        } else {
            state.clickCount = 1;
        }
        state.lastClickTimeMs = nowMs;
        state.lastClickX = clientX;
        state.lastClickY = clientY;
        state.lastClickTarget = target;

        auto populate = [&](dom::MouseEvent& e) {
            e.setClientX(clientX); e.setClientY(clientY);
            e.setScreenX(clientX); e.setScreenY(clientY);
            e.setPageX(pageX);     e.setPageY(pageY);
            e.setMovementX(movementX); e.setMovementY(movementY);
            e.setButton(button); e.setButtons(buttons);
            e.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
            e.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
            e.setAltKey((mod & SDL_KMOD_ALT) != 0);
            e.setMetaKey((mod & SDL_KMOD_GUI) != 0);
            e.setIsTrusted(true);
        };

        dom::MouseEvent clickEvt("click");
        populate(clickEvt);
        clickEvt.setDetail(state.clickCount);
        js::dispatchDomEvent(ctx.jsCtx, target, clickEvt);

        // Interactive form submission: a click on <button> or
        // <input type=submit> walks up to the owning form and fires the
        // submit event (with constraint validation). Skipped if the click
        // was cancelled, or the button's type is explicitly button/reset.
        if (!clickEvt.defaultPrevented() && target && ctx.jsCtx) {
            const auto& tag = target->tagName();
            const bool isButton = (tag == "BUTTON" || tag == "button");
            const bool isInput = (tag == "INPUT" || tag == "input");
            const std::string inputType = isInput ? target->getAttribute("type") : "";
            const bool isActionInput =
                isInput && (inputType == "submit" || inputType == "reset" || inputType == "image");
            if (isButton || isActionInput) {
                std::string btnType = target->getAttribute("type");
                // <button> defaults to type=submit when no type attribute.
                if (btnType.empty() && isButton) btnType = "submit";
                if (btnType == "image") btnType = "submit";
                if (btnType == "submit") {
                    dom::Element* owner = nullptr;
                    const std::string& attrForm = target->getAttribute("form");
                    if (!attrForm.empty() && target->document()) {
                        auto* o = target->document()->getElementById(attrForm);
                        if (o && (o->tagName() == "FORM" || o->tagName() == "form")) owner = o;
                    } else {
                        for (auto* p = target->parentElement(); p; p = p->parentElement()) {
                            if (p->tagName() == "FORM" || p->tagName() == "form") { owner = p; break; }
                        }
                    }
                    if (owner) js::requestFormSubmit(ctx.jsCtx, owner, target);
                } else if (btnType == "reset") {
                    dom::Element* owner = nullptr;
                    for (auto* p = target->parentElement(); p; p = p->parentElement()) {
                        if (p->tagName() == "FORM" || p->tagName() == "form") { owner = p; break; }
                    }
                    if (owner) {
                        dom::Event resetEvt("reset", true, true);
                        resetEvt.setIsTrusted(true);
                        js::dispatchDomEvent(ctx.jsCtx, owner, resetEvt);
                    }
                }
            }
        }

        if (state.clickCount == 2) {
            dom::MouseEvent dblEvt("dblclick", true, true);
            populate(dblEvt);
            dblEvt.setDetail(2);
            js::dispatchDomEvent(ctx.jsCtx, target, dblEvt);
        }

        if (button == 2) {
            dom::MouseEvent ctxEvt("contextmenu", true, true);
            populate(ctxEvt);
            js::dispatchDomEvent(ctx.jsCtx, target, ctxEvt);
        }
    }

    state.mouseDownTarget = nullptr;
    if (ctx.dirtyFlag) *ctx.dirtyFlag = true;
}

} // namespace bro::engine
