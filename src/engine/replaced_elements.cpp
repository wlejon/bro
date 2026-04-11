// Shared replaced-element initialization and interaction logic.
// Used by the Engine for both the app document and system panels.

#include "engine/replaced_elements.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "layout/el_select.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_svg.h"
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

void ensureReplacedElements(dom::Element* elem, render::Renderer* renderer) {
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
    }

    // Recurse into children
    for (auto* child : elem->childNodes()) {
        if (child->nodeType() == dom::NodeType::Element) {
            ensureReplacedElements(static_cast<dom::Element*>(child), renderer);
        }
    }

    // Recurse into shadow DOM
    if (elem->hasShadow()) {
        auto* sr = elem->shadowRoot();
        for (auto* child : sr->childNodes()) {
            if (child->nodeType() == dom::NodeType::Element) {
                ensureReplacedElements(static_cast<dom::Element*>(child), renderer);
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

// Pick a color from the color picker grid at pixel position (x, y).
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
        float q2 = lit < 0.5f ? lit*(1+sat) : lit+sat-lit*sat;
        float p = 2*lit-q2;
        float hn = hue/360.0f;
        cr = static_cast<uint8_t>(hue2rgb(p, q2, hn+1.0f/3)*255);
        cg = static_cast<uint8_t>(hue2rgb(p, q2, hn)*255);
        cb = static_cast<uint8_t>(hue2rgb(p, q2, hn-1.0f/3)*255);
    }

    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", cr, cg, cb);
    return hex;
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
    dom::Element* prevActive,
    float x, float y)
{
    auto* prevInput = getElInput(prevActive);
    if (prevInput) {
        // Close color picker if clicking outside it
        if (prevInput->isPickerOpen()) {
            auto dp = prevInput->lastDrawPos();
            float px = dp.x, py = dp.y + dp.h + 2;
            float pw = 200.0f, ph = 160.0f;
            bool inPicker = (x >= px && x < px + pw && y >= py && y < py + ph);
            bool inSwatch = (x >= dp.x && x < dp.x + dp.w &&
                             y >= dp.y && y < dp.y + dp.h);
            if (inPicker) {
                std::string hex = pickColorFromGrid(x, y, px, py, pw, ph);
                prevActive->setAttribute("value", hex.c_str());
                dom::Event changeEvt("change");
                dispatchControlEvent(ctx, prevActive, changeEvt);
                dispatchInputEvent(ctx, prevActive);
                prevInput->setPickerOpen(false);
                *ctx.dirtyFlag = true;
                return ClickDisposition::Consumed;
            } else if (inSwatch) {
                prevInput->setPickerOpen(false);
                *ctx.dirtyFlag = true;
                return ClickDisposition::Consumed;
            } else {
                prevInput->setPickerOpen(false);
            }
        }
        prevInput->setFocused(false);
        *ctx.dirtyFlag = true;
    }

    auto* prevTextarea = getElTextarea(prevActive);
    if (prevTextarea) {
        prevTextarea->setFocused(false);
        *ctx.dirtyFlag = true;
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
            if (prevActive) {
                prevActive->setAttribute("value", opts[idx].value);
                dom::Event changeEvt("change");
                dispatchControlEvent(ctx, prevActive, changeEvt);
                dispatchInputEvent(ctx, prevActive);
            }
            *ctx.dirtyFlag = true;
            return ClickDisposition::Consumed;
        }
        prevSelect->setOpen(false);
        *ctx.dirtyFlag = true;
    }

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
            if (newInput->isPickerOpen()) {
                auto dp = newInput->lastDrawPos();
                float px = dp.x, py = dp.y + dp.h + 2;
                float pw = 200.0f, ph = 160.0f;
                if (x >= px && x < px + pw && y >= py && y < py + ph) {
                    std::string hex = pickColorFromGrid(x, y, px, py, pw, ph);
                    target->setAttribute("value", hex.c_str());
                    dom::Event changeEvt("change");
                    dispatchControlEvent(ctx, target, changeEvt);
                    dispatchInputEvent(ctx, target);
                }
                newInput->setPickerOpen(false);
            } else {
                newInput->setPickerOpen(true);
            }
            safeStopTextInput(ctx.window);
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
        newSelect->setOpen(!newSelect->isOpen());
        if (newSelect->isOpen()) {
            newSelect->setHighlightedIndex(newSelect->selectedIndex());
        }
        safeStopTextInput(ctx.window);
        *ctx.dirtyFlag = true;
    } else {
        safeStopTextInput(ctx.window);
    }
}

// ---------------------------------------------------------------------------
// Dropdown hover
// ---------------------------------------------------------------------------

bool updateDropdownHover(const ControlContext& ctx, float x, float y) {
    if (!ctx.document) return false;
    auto* activeEl = ctx.document->activeElement();
    auto* select = getElSelect(activeEl);
    if (!select || !select->isOpen()) return false;

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
            *ctx.dirtyFlag = true;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Draw active overlays (dropdowns, pickers)
// ---------------------------------------------------------------------------

void drawActiveOverlays(dom::Document* doc) {
    if (!doc) return;
    auto* activeEl = doc->activeElement();

    auto* sel = getElSelect(activeEl);
    if (sel && sel->isOpen()) {
        sel->drawDropdown();
    }

    auto* inp = getElInput(activeEl);
    if (inp && inp->isPickerOpen()) {
        inp->drawColorPicker();
    }
}

} // namespace bro::engine
