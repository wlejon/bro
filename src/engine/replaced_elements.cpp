// Shared replaced-element initialization and interaction logic.
// Used by the Engine for both the app document and system panels.

#include "engine/replaced_elements.h"
#include "js/anchor_download.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
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
#include "svg/svg_renderer.h"
#include "util/object_url.h"
#include "util/string_utils.h"

#include "broimage/decode.h"
#if BRO_WITH_WEBP
#include "render/webp_image.h"
#endif

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace bro::engine {

// ---------------------------------------------------------------------------
// <img> intrinsic size
// ---------------------------------------------------------------------------

namespace {

// Dimensions of encoded image bytes, without decoding the pixels.
bool probeBytes(const uint8_t* data, size_t len, int& w, int& h) {
    if (!data || len == 0) return false;
    int c = 0;
    if (broimage::probe_dimensions_memory(data, len, &w, &h, &c)) return true;
#if BRO_WITH_WEBP
    // broimage is stb-backed and stb has no WebP, so a .webp needs libwebp's
    // header reader — the same split the decode paths have (render/webp_image.h).
    std::vector<uint8_t> ignored;
    int ww = 0, hh = 0;
    if (render::decodeWebPHeader(data, len, ww, hh)) { w = ww; h = hh; return true; }
#endif
    return false;
}

} // namespace

// Resolve `src` and read enough of it to learn the image's size. Returns
// false (leaving w/h at 0) for a missing file or an unreadable header, which
// leaves the <img> zero-sized — the same as a browser showing a broken image.
//
// Public because the JS `img.src =` setter needs the same answer for an image
// that is never inserted into the document — three.js's ImageLoader builds one,
// sets src, and reads the size off it without ever appending it, so nothing
// here would ever walk to it. One implementation, so a detached image and a
// laid-out one cannot disagree about how big the same file is.
bool probeImageSize(dom::Element* elem, const std::string& src,
                    int& w, int& h) {
    w = 0;
    h = 0;
    if (!elem || src.empty()) return false;

    // data: URLs carry their bytes inline. SVG data URLs are handled in the
    // layout adapter (it parses the <svg> width/height out of the markup), so
    // only raster payloads need probing here.
    if (src.compare(0, 5, "data:") == 0) {
        const auto comma = src.find(',');
        if (comma == std::string::npos) return false;
        const std::string meta = src.substr(5, comma - 5);
        if (meta.find("image/svg+xml") != std::string::npos) return false;
        const std::string body = src.substr(comma + 1);
        if (meta.find(";base64") == std::string::npos) return false;
        const std::vector<uint8_t> bytes = util::base64Decode(body);
        return probeBytes(bytes.data(), bytes.size(), w, h);
    }

    // blob: URL — bytes the page holds, registered when it minted the URL.
    // An SVG object URL answers from its markup, the same as an SVG file does.
    if (util::isObjectURL(src)) {
        auto data = util::lookupObjectURL(src);
        if (!data || data->bytes.empty()) return false;
        const char* chars = reinterpret_cast<const char*>(data->bytes.data());
        if (svg::looksLikeSvg(chars, data->bytes.size())) {
            float sw = 0, sh = 0;
            svg::svgIntrinsicSize(chars, data->bytes.size(), sw, sh);
            w = static_cast<int>(sw);
            h = static_cast<int>(sh);
            return w > 0 && h > 0;
        }
        return probeBytes(data->bytes.data(), data->bytes.size(), w, h);
    }

    // Resolve against the document's base path, matching the rule
    // DrawTraversal::loadImage uses when it later reads the same file.
    std::string clean = src;
    if (const auto q = clean.find_first_of("?#"); q != std::string::npos)
        clean.resize(q);
    std::string path;
    const bool absolute =
        (clean.size() >= 2 && clean[1] == ':') ||
        (!clean.empty() && (clean[0] == '/' || clean[0] == '\\'));
    const std::string& base = elem->document() ? elem->document()->basePath()
                                               : std::string();
    if (absolute || base.empty()) {
        path = clean;
    } else {
        path = base;
        if (path.back() != '/' && path.back() != '\\') path += '/';
        path += clean;
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;
    // Header only. Every format we probe puts its dimensions in the first few
    // hundred bytes, so a 64 KB ceiling covers them all without reading a
    // multi-megabyte photo just to size its box. The full decode happens later
    // in the draw path, and only for images that are actually painted.
    std::vector<uint8_t> head(64 * 1024);
    ifs.read(reinterpret_cast<char*>(head.data()),
             static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(ifs.gcount()));

    // An SVG carries its size in the root tag, not in a binary header, so the
    // bitmap probe cannot see it. Reading it here is what makes the <img> a
    // replaced element with a real intrinsic size; otherwise it lays out as an
    // empty inline box and the icon has nowhere to paint. Same reader the paint
    // path and the rasterizer use, so all three agree on how big it is.
    if (svg::looksLikeSvg(reinterpret_cast<const char*>(head.data()), head.size())) {
        float sw = 0, sh = 0;
        svg::svgIntrinsicSize(reinterpret_cast<const char*>(head.data()),
                              head.size(), sw, sh);
        w = static_cast<int>(sw);
        h = static_cast<int>(sh);
        return w > 0 && h > 0;
    }

    return probeBytes(head.data(), head.size(), w, h);
}

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
    } else if (tag == "IMG" || tag == "img") {
        // Give layout the image's intrinsic size. Without it an <img> is not a
        // replaced element, so it lays out as an empty inline box and never
        // appears — see Element::imageNaturalWidth().
        //
        // Re-probed only when `src` changes: this runs on every DOM-dirty
        // pass, and reading a header per pass per image would put file I/O on
        // the layout path.
        const std::string src = elem->getAttribute("src");
        if (!src.empty() && src != elem->imageProbedSrc()) {
            int w = 0, h = 0;
            probeImageSize(elem, src, w, h);
            elem->setImageNaturalSize(src, w, h);
        } else if (src.empty() && !elem->imageProbedSrc().empty()) {
            // src removed: drop the stale size rather than keep sizing the
            // box from an image that is no longer referenced.
            elem->setImageNaturalSize("", 0, 0);
        }
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

// True when the element sits inside a contenteditable host (attribute set to
// anything other than "false") — such hosts take text input too, even though
// they are not replaced-element controls.
static bool insideEditableHost(dom::Element* el) {
    for (auto* e = el; e; e = e->parentElement()) {
        if (!e->hasAttribute("contenteditable")) continue;
        return e->getAttribute("contenteditable") != "false";
    }
    return false;
}


// Where a press puts focus.
//
// HTML only moves focus to a *focusable* element: a form control, a link with
// an href, anything carrying tabindex, or an editing host. A press on ordinary
// content — the <span> inside a syntax-highlighted line, a label, a bare <div>
// — takes focus away from whatever had it and leaves it on the body. It does
// not make that span the activeElement.
//
// Focusing the raw hit target instead is not a cosmetic difference. A widget
// that focuses its own element from its mousedown handler — CodeMirror keeps a
// hidden <textarea> focused and reads keystrokes out of it — had that focus
// taken straight back by whatever the pointer happened to land on, so the
// editor accepted no typing at all. Nearest focusable ancestor, or nothing.
static dom::Element* clickFocusTarget(dom::Element* target) {
    for (auto* e = target; e; e = e->parentElement()) {
        const std::string& tag = e->tagName();
        if (tag == "INPUT" || tag == "input") {
            // type=hidden has no box to click, but it can be reached through a
            // label; it is never focusable either way.
            if (util::toLower(e->getAttribute("type")) == "hidden") continue;
            return e;
        }
        if (tag == "TEXTAREA" || tag == "textarea" ||
            tag == "SELECT"   || tag == "select"   ||
            tag == "BUTTON"   || tag == "button")
            return e;
        if ((tag == "A" || tag == "a" || tag == "AREA" || tag == "area") &&
            e->hasAttribute("href"))
            return e;
        if (e->hasAttribute("tabindex")) return e;
        if (e->hasAttribute("contenteditable") &&
            e->getAttribute("contenteditable") != "false")
            return e;
    }
    return nullptr;
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
// The `change` event of a text control
// ---------------------------------------------------------------------------

namespace {

// Where an element stands, well enough to find the same one again after the
// tree it was in has been rebuilt.
//
// The child indices from the root, plus what the element is and what it says.
// An application that redraws a panel builds the same rows in the same order,
// so the redrawn control lands at the same path with the same tag and the same
// text — and a control that is *not* the same one is caught by one of those
// three. Deliberately blunt: this is only ever asked about a press that the
// application has just been made to redraw, and the alternative to a blunt
// answer is no answer at all.
struct ElementPlace {
    std::vector<int> path;   // child index at each level, root first
    std::string tag;
    std::string text;
    bool ok = false;
};

ElementPlace placeOf(dom::Element* el) {
    ElementPlace p;
    if (!el) return p;
    p.tag = el->tagName();
    p.text = el->textContent();
    for (auto* node = el; node; node = node->parentElement()) {
        auto* parent = node->parentElement();
        if (!parent) { p.ok = true; break; }   // reached the root
        const auto kids = parent->children();
        int idx = -1;
        for (size_t i = 0; i < kids.size(); i++) {
            if (kids[i] == node) { idx = static_cast<int>(i); break; }
        }
        if (idx < 0) return {};                 // detached already: no place
        p.path.push_back(idx);
    }
    std::reverse(p.path.begin(), p.path.end());
    return p;
}

// Is this element still in the document, rather than in a subtree somebody
// replaced? A freed node stays resolvable until the deferred free is drained
// (see NodeHandle), so being alive is not the same question.
bool isAttached(dom::Document* doc, dom::Element* el) {
    if (!doc || !el) return false;
    auto* root = doc->documentElement();
    for (auto* node = el; node; node = node->parentElement()) {
        if (node == root) return true;
    }
    return false;
}

dom::Element* elementAt(dom::Document* doc, const ElementPlace& p) {
    if (!p.ok || !doc) return nullptr;
    dom::Element* at = doc->documentElement();
    for (int idx : p.path) {
        if (!at) return nullptr;
        const auto kids = at->children();
        if (idx < 0 || idx >= static_cast<int>(kids.size())) return nullptr;
        at = kids[idx];
    }
    if (!at || at->tagName() != p.tag || at->textContent() != p.text) return nullptr;
    return at;
}

} // namespace

void armValueChange(dom::Element* el) {
    if (auto* input = getElInput(el)) input->armChange(el);
    if (auto* ta = getElTextarea(el)) ta->armChange(el);
}

bool takeValueChange(dom::Element* el) {
    if (auto* input = getElInput(el)) return input->takeChange(el);
    if (auto* ta = getElTextarea(el)) return ta->takeChange(el);
    return false;
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

int pressOrdinal(const MouseDispatchState& state, dom::Element* target,
                 float clientX, float clientY, double nowMs,
                 double dblThresholdMs, float dblDistPx) {
    const bool continuesStreak =
        target && state.lastClickTarget.get() == target &&
        (nowMs - state.lastClickTimeMs) < dblThresholdMs &&
        std::fabs(clientX - state.lastClickX) < dblDistPx &&
        std::fabs(clientY - state.lastClickY) < dblDistPx;
    return continuesStreak ? state.clickCount + 1 : 1;
}

void focusNewControl(
    const ControlContext& ctx,
    MouseDispatchState& state,
    dom::Element* target,
    float x, float y,
    PressIntent intent)
{
    auto* newInput = getElInput(target);
    auto* newTextarea = getElTextarea(target);
    auto* newSelect = getElSelect(target);

    // Only a checkbox/radio press below leaves something to undo. Clearing the
    // record up front means a press on anything else cannot leave a previous
    // one armed for the next release to act on.
    state.activationTarget.reset();
    state.activationPrevRadio.reset();
    state.activationWasChecked = false;

    if (newInput) {
        newInput->setFocused(true);
        auto itype = newInput->inputType(target);

        if (itype == layout::ElInput::InputType::Checkbox) {
            state.activationTarget.assign(ctx.document, target);
            state.activationWasChecked = target->hasAttribute("checked");
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
            // Read the group's state BEFORE clearing it — after clearRadioGroup
            // there is nothing left to say which member had the check.
            state.activationTarget.assign(ctx.document, target);
            state.activationWasChecked = target->hasAttribute("checked");
            state.activationPrevRadio.assign(ctx.document,
                                             js::checkedRadioInGroup(target));
            js::clearRadioGroup(target);
            target->setAttribute("checked", "");
            dom::Event changeEvt("change");
            dispatchControlEvent(ctx, target, changeEvt);
            dispatchInputEvent(ctx, target);
            *ctx.dirtyFlag = true;
        } else if (itype == layout::ElInput::InputType::Range) {
            auto dp = newInput->lastDrawPos();
            float thumbR = layout::ElInput::rangeThumbRadius(dp.h);
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
            bool onSpinButton = false;
            if (newInput->inputType(target) == layout::ElInput::InputType::Number) {
                auto dp = newInput->lastDrawPos();
                float bx = dp.x + dp.w - layout::ElInput::kSpinButtonWidth;
                if (x >= bx && x <= dp.x + dp.w) {
                    onSpinButton = true;
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
            // Seed the selection from the click. A press on the spin buttons
            // isn't a caret press — it just steps the value, and the caret
            // stays at the end of the number it wrote.
            if (!onSpinButton) {
                if (intent.ordinal >= 3)      newInput->selectAll();
                else if (intent.ordinal == 2) newInput->selectWordAtPoint(x, y);
                else                          newInput->caretToPoint(x, y, intent.extend);
            }
            safeStartTextInput(ctx.window);
            *ctx.dirtyFlag = true;
        } else {
            // Button types — no text input
            safeStopTextInput(ctx.window);
            *ctx.dirtyFlag = true;
        }
    } else if (newTextarea) {
        newTextarea->setFocused(true);
        if (intent.ordinal >= 3)      newTextarea->selectAll();
        else if (intent.ordinal == 2) newTextarea->selectWordAtPoint(x, y);
        else                          newTextarea->caretToPoint(x, y, intent.extend);
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
    } else if (insideEditableHost(target)) {
        // A contenteditable host still takes text input (raw TEXT_INPUT
        // commits insert via the DOM Selection) — without this, clicking
        // into one would stop SDL text input and typing would go dead in
        // windowed mode. IME preedit rendering inside contenteditable is
        // not wired (form controls only).
        safeStartTextInput(ctx.window);
    } else {
        safeStopTextInput(ctx.window);
    }
}

// ---------------------------------------------------------------------------
// Per-document mouse press/release dispatch
// ---------------------------------------------------------------------------

void applyMouseOffset(dom::MouseEvent& evt, dom::Element* target) {
    if (!target) return;
    dom::AbsolutePoint origin = dom::absoluteContentOrigin(target);
    evt.setOffsetX(evt.clientX() - static_cast<double>(origin.x));
    evt.setOffsetY(evt.clientY() - static_cast<double>(origin.y));
}

bool dispatchDocMousePress(
    const ControlContext& ctx,
    MouseDispatchState& state,
    dom::Element* target,
    dom::MouseEvent& evt,
    float focusX, float focusY,
    PressIntent intent) {

    if (!ctx.document) { state.mouseDownTarget.reset(); return false; }
    if (!target) { state.mouseDownTarget.reset(); return false; }

    auto* prevActive = ctx.document->activeElement();

    // Give the previously-active control a chance to consume (e.g. dropdown
    // option selection). If consumed, still fire mousedown and bail — caller
    // should not treat the press as normal activation.
    auto disp = unfocusPreviousControl(ctx, prevActive);
    if (disp == ClickDisposition::Consumed) {
        js::dispatchDomEvent(ctx.jsCtx, target, evt);
        state.mouseDownTarget.assign(ctx.document, target);
        if (ctx.dirtyFlag) *ctx.dirtyFlag = true;
        return true;
    }

    // Focus follows the nearest focusable ancestor, not the hit target — but
    // the press itself still belongs to what was actually clicked, so `target`
    // keeps driving dispatch and control behaviour below.
    dom::Element* focusEl = clickFocusTarget(target);
    ctx.document->setActiveElement(focusEl);
    if (focusEl != prevActive) {
        // A text control that was edited reports it now, before blur — the
        // order browsers use, and the one that matters: a listener reading the
        // model on blur must already have been told what was typed. Only when
        // focus genuinely leaves, which is why this is here rather than in
        // unfocusPreviousControl: a press *inside* the focused field to move
        // the caret goes through that too, and is not a departure.
        if (takeValueChange(prevActive)) {
            // A change listener is application code that has just been handed
            // something new, and the usual thing to do with something new is to
            // redraw — which frees the element this very press is on its way to.
            //
            // **The press survives that.** A browser loses it: click fires only
            // where mousedown and mouseup land on one element, so typing in a
            // field and clicking the button beside it does nothing the first
            // time and works the second, which is the wart every DOM
            // application knows and nobody wants. The engine caused the
            // interleaving — it is the one that reports `change` in the middle
            // of a press — so it is the one that puts the press back: the
            // element is found again where it stood, and only if what stands
            // there now is the same tag with the same text. A control that was
            // genuinely replaced by a different one fails that and the press is
            // dropped, which is the browser's answer and the safe one.
            const ElementPlace place = placeOf(target);
            dom::ElementHandle keep(ctx.document, target);
            // And the field itself, which the same redraw takes with it — blur
            // still has to be dispatched on it, and a field that is gone gets
            // no blur rather than one on a node nothing owns.
            dom::ElementHandle keepPrev(ctx.document, prevActive);
            dom::Event changeEvt("change");
            changeEvt.setIsTrusted(true);
            dispatchControlEvent(ctx, prevActive, changeEvt);
            prevActive = keepPrev.get();
            if (prevActive && !isAttached(ctx.document, prevActive)) prevActive = nullptr;
            // The one it landed on if that is still in the tree — nothing was
            // rebuilt, or not this part of it — and the one standing in its
            // place if it is not.
            dom::Element* still = keep.get();
            if (still && !isAttached(ctx.document, still)) still = nullptr;
            target = still ? still : elementAt(ctx.document, place);
            if (!target) {
                state.mouseDownTarget.reset();
                if (ctx.dirtyFlag) *ctx.dirtyFlag = true;
                return false;
            }
            focusEl = clickFocusTarget(target);
            ctx.document->setActiveElement(focusEl);
        }
        dispatchFocusEvents(ctx, prevActive, focusEl);
        // What the departure will be measured against next time.
        armValueChange(focusEl);
    }

    focusNewControl(ctx, state, target, focusX, focusY, intent);

    js::dispatchDomEvent(ctx.jsCtx, target, evt);
    state.mouseDownTarget.assign(ctx.document, target);
    if (ctx.dirtyFlag) *ctx.dirtyFlag = true;
    return false;
}

// A disabled form control is inert: per HTML it has no activation behavior, so it
// dispatches no click/dblclick (and no submit/reset/disclosure default action).
// Walk up from the event target so a click on a child node (an icon/span inside a
// <button>) counts too, and so a disabled <fieldset> disables its descendants.
static bool isInDisabledControl(dom::Element* el) {
    for (auto* e = el; e; e = e->parentElement()) {
        const std::string& tag = e->tagName();
        const bool formControl =
            tag == "BUTTON"   || tag == "button"   ||
            tag == "INPUT"    || tag == "input"    ||
            tag == "SELECT"   || tag == "select"   ||
            tag == "TEXTAREA" || tag == "textarea" ||
            tag == "FIELDSET" || tag == "fieldset";
        if (formControl && e->hasAttribute("disabled")) return true;
    }
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

    // A click is on the nearest element containing both the press and the
    // release — the UI Events rule. Most presses are on one element and the
    // click is on it; a press on a button's icon that releases a pixel later on
    // the button's own padding is a click on the button, where requiring the
    // two to be the same element (what this did before) dropped it, and
    // hands drift by a pixel all the time. Siblings click their parent, which
    // is what the spec says and rarely what anybody listens for.
    //
    // A press target that has left the tree by the release is a click on
    // nothing — a browser's answer too. An application that rebuilds a button
    // between the press and the release has replaced the thing that was
    // pressed, and putting the click on whatever stands there now would press
    // controls nobody pressed; the one interleaving the engine itself causes,
    // a redraw off `change`, is put back at the press (see
    // dispatchDocMousePress). Never on a disabled form control, which has no
    // activation behavior.
    dom::Element* clickTarget = nullptr;
    if (target) {
        dom::Element* down = state.mouseDownTarget.get();
        if (down == target) clickTarget = target;
        else if (down && isAttached(ctx.document, down)) {
            for (auto* a = target; a && !clickTarget; a = a->parentElement())
                for (auto* d = down; d; d = d->parentElement())
                    if (d == a) { clickTarget = a; break; }
        }
    }
    if (clickTarget && !isInDisabledControl(clickTarget)) {
        // The click's own target from here on: the double-click bookkeeping,
        // the activation behaviours and the form walk are all about it.
        dom::Element* target = clickTarget;
        // Rolling double-click detection.
        if (state.lastClickTarget.get() == target &&
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
        state.lastClickTarget.assign(ctx.document, target);

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
        if (target) applyMouseOffset(clickEvt, target);
        js::dispatchDomEvent(ctx.jsCtx, target, clickEvt);

        // Checkbox / radio "legacy-canceled-activation behavior": the press
        // already applied the new checkedness (focusNewControl) so the control
        // ticks while the button is held; a click whose default was prevented
        // has to put it back. Same rule, same gate, as the <details>/<summary>
        // disclosure toggle further down — that one just happens to run its
        // default action here rather than on press.
        if (dom::Element* toggled = state.activationTarget.get()) {
            if (clickEvt.defaultPrevented() && toggled == target) {
                if (state.activationWasChecked) toggled->setAttribute("checked", "");
                else                            toggled->removeAttribute("checked");
                // Radio only: hand the check back to whichever member held it.
                // Resolves to null if that element has since been removed, in
                // which case the group is simply left empty — the same place a
                // browser lands when the previous option is gone.
                if (auto* prev = state.activationPrevRadio.get())
                    prev->setAttribute("checked", "");
                if (ctx.dirtyFlag) *ctx.dirtyFlag = true;
            }
        }

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

        // <details>/<summary> default action: clicking inside a <summary>
        // toggles the [open] attribute on the parent <details>. Walk up from
        // the click target so a click on text or icon inside the summary
        // counts too — but only the first <summary> child of <details> is
        // the disclosure handle (HTML spec). Skip when preventDefault'd.
        if (!clickEvt.defaultPrevented() && target) {
            for (auto* el = target; el; el = el->parentElement()) {
                const auto& tag = el->tagName();
                if (tag != "SUMMARY" && tag != "summary") continue;
                auto* parent = el->parentElement();
                if (!parent) break;
                const auto& ptag = parent->tagName();
                if (ptag != "DETAILS" && ptag != "details") break;
                // First <summary> child only — others are inert per spec.
                dom::Element* firstSummary = nullptr;
                for (auto* c : parent->children()) {
                    const auto& ct = c->tagName();
                    if (ct == "SUMMARY" || ct == "summary") {
                        firstSummary = c;
                        break;
                    }
                }
                if (firstSummary != el) break;
                if (parent->hasAttribute("open")) {
                    parent->removeAttribute("open");
                } else {
                    parent->setAttribute("open", "");
                }
                // Fire a non-bubbling "toggle" event for parity with HTML5.
                dom::Event toggleEvt("toggle", false, false);
                toggleEvt.setIsTrusted(true);
                if (ctx.jsCtx) js::dispatchDomEvent(ctx.jsCtx, parent, toggleEvt);
                break;
            }
        }

        // <input type=file> default action: open the native picker. Mirrors
        // the programmatic element.click() path.
        if (!clickEvt.defaultPrevented() && target && ctx.jsCtx) {
            js::runFilePickerActivation(ctx.jsCtx, target);
        }

        // <a download> default action: save the link's bytes rather than
        // navigating to them. Mirrors the programmatic element.click() path.
        if (!clickEvt.defaultPrevented() && target && ctx.jsCtx) {
            js::runAnchorDownload(ctx.jsCtx, target);
        }

        // <label> default action: a click on the label's text (or an icon, or a
        // wrapper span) activates the control the label labels. Runs last, and
        // only when the click did not already land on that control, so a direct
        // click on a wrapped checkbox is not toggled a second time here.
        if (!clickEvt.defaultPrevented() && target && ctx.jsCtx) {
            js::forwardLabelActivation(ctx.jsCtx, target);
        }

        if (state.clickCount == 2) {
            dom::MouseEvent dblEvt("dblclick", true, true);
            populate(dblEvt);
            dblEvt.setDetail(2);
            if (target) applyMouseOffset(dblEvt, target);
            js::dispatchDomEvent(ctx.jsCtx, target, dblEvt);
        }

        if (button == 2) {
            dom::MouseEvent ctxEvt("contextmenu", true, true);
            populate(ctxEvt);
            if (target) applyMouseOffset(ctxEvt, target);
            js::dispatchDomEvent(ctx.jsCtx, target, ctxEvt);
        }
    }

    state.mouseDownTarget.reset();
    // Disarm the activation undo whatever happened above — including the paths
    // that never reach the click (mouseup on a different element, a disabled
    // control), where the record would otherwise still be live for the next
    // release to act on.
    state.activationTarget.reset();
    state.activationPrevRadio.reset();
    state.activationWasChecked = false;
    if (ctx.dirtyFlag) *ctx.dirtyFlag = true;
}

} // namespace bro::engine
