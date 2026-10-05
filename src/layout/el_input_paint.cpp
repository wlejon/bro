// Painting for <input>: the text field (selection wash, caret, IME
// underline, number spinner) and the checkbox, radio, range and colour chrome.
#include "layout/el_input.h"
#include "layout/control_text.h"
#include "layout/draw_traversal.h"
#include "layout/pseudo_style.h"
#include "dom/element.h"
#include "render/renderer.h"

#include <algorithm>
#include <span>

namespace bro::layout {

using bromath::Color;
using bromath::cfromColor8;

void ElInput::draw(render::Renderer* renderer,
                   const htmlayout::layout::LayoutBox& box,
                   const htmlayout::css::ComputedStyle& /*style*/,
                   float offsetX, float offsetY,
                   float docOffsetX, float docOffsetY) {
    if (!renderer || !elem_) return;

    // Use the caller's renderer (may differ from construction renderer,
    // e.g. raster thread has its own SkiaRenderer). Leave it set — the
    // raster thread is idle when the main thread uses control methods,
    // so no race condition.
    renderer_ = renderer;

    float x = box.contentRect.x + offsetX;
    float y = box.contentRect.y + offsetY;
    float w = box.contentRect.width;
    float h = box.contentRect.height;

    if (w <= 0 || h <= 0) return;

    // lastDrawPos_ feeds mouse-drag math in Engine::handleMouseMove and the
    // overlay anchors in focusNewControl, which compare against cursor
    // coordinates translated into this pass's surface space — so it needs
    // the ancestor-transform-projected rect (same fix as canvas/webgl/scene
    // layers in DrawTraversal), not the raw pre-transform layout position,
    // or a range slider under a zoomed/panned ancestor drags at the wrong
    // screen-to-value ratio entirely. absoluteContentBox() is document-space;
    // the caller's doc→surface offset (just −scroll for the app document)
    // lands this in app content space (window space for system panels).
    docOffsetX_ = docOffsetX;
    docOffsetY_ = docOffsetY;
    lastDrawPos_ = contentBox_();

    auto t = inputType(nullptr);
    if (t == InputType::Hidden) return;

    switch (t) {
        case InputType::Checkbox: drawCheckbox_(x, y, w, h); break;
        case InputType::Radio:    drawRadio_(x, y, w, h); break;
        case InputType::Range:    drawRange_(x, y, w, h); break;
        case InputType::Color:    drawColor_(x, y, w, h); break;
        default: drawText_(x, y, w, h); break;
    }
}

float ElInput::textWidth_(float w) const {
    if (inputType(nullptr) == InputType::Number) {
        return std::max(0.0f, w - kSpinButtonWidth);
    }
    return w;
}

void ElInput::drawText_(float x, float y, float w, float h) {
    std::string val = getAttr("value");
    std::string placeholder = getAttr("placeholder");
    std::string text;
    bool isPlaceholder = false;

    if (!val.empty()) {
        text = displayText_();
    } else if (!focused_ && !placeholder.empty()) {
        text = placeholder;
        isPlaceholder = true;
    }

    // ::placeholder styling from the cascade (color/opacity/font-*). Empty
    // when no rule targets it — the legacy gray paint applies unchanged.
    htmlayout::css::ComputedStyle phStyle;
    if (isPlaceholder) phStyle = resolveStyledPseudo(elem_, "placeholder");

    render::FontRef fontRef = getFontRef();
    if (!phStyle.empty()) applyPseudoFont(phStyle, fontRef);
    auto lm = render::LineMetrics::from(renderer_->measureText("M", fontRef));
    float textY = lm.baselineY(y, h);

    // Distance from the text's left edge to the caret. Both password and plain
    // text draw one display glyph per value byte-run, so a byte prefix of `val`
    // is a glyph prefix of `text` — measuring `text` works for both (measuring
    // `val` under a password would place the caret against the wrong glyphs).
    const int cpos = std::clamp(sel_.caret, 0, static_cast<int>(val.size()));
    float caretOffset = 0.0f;
    if (!isPlaceholder && cpos > 0 && cpos <= static_cast<int>(text.size())) {
        caretOffset = caretXInRun(text, 0, text.size(),
                                  static_cast<size_t>(cpos), fontRef, renderer_);
    }

    // Scroll the text under the fixed box so the caret stays visible once the
    // value is wider than the content area — otherwise typing past the right
    // edge clips the caret away and you lose your place. Only a focused field
    // scrolls; an unfocused one always shows its text from the start.
    const float availW = textWidth_(w);
    if (!focused_ || isPlaceholder) {
        scrollX_ = 0.0f;
    } else {
        float fullW = text.empty() ? 0.0f
                                   : renderer_->measureText(text, fontRef).width;
        // Leave a pixel for the caret itself so it isn't half-clipped at the edge.
        if (caretOffset - scrollX_ < 0.0f) {
            scrollX_ = caretOffset;
        } else if (caretOffset - scrollX_ > availW - 1.0f) {
            scrollX_ = caretOffset - availW + 1.0f;
        }
        scrollX_ = std::clamp(scrollX_, 0.0f, std::max(0.0f, fullW - availW + 1.0f));
    }
    float drawX = x - scrollX_;

    renderer_->save();
    renderer_->setClip(x, y, availW, h);

    // Selection wash, behind the text. Measured against the drawn glyphs, so a
    // password field highlights its mask rather than the raw value's widths.
    // ::selection may restyle the wash (background-color) and the selected
    // glyphs (color, repainted after the main text run below).
    htmlayout::css::ComputedStyle selStyle;
    int selS = 0, selE = 0;
    bool hasSelBand = false;
    if (focused_ && !isPlaceholder && hasSelection() && !text.empty()) {
        selS = std::clamp(sel_.start(), 0, static_cast<int>(text.size()));
        selE = std::clamp(sel_.end(), 0, static_cast<int>(text.size()));
        if (selE > selS) {
            hasSelBand = true;
            selStyle = resolveStyledPseudo(elem_, "selection");
            float sx = drawX + caretXInRun(text, 0, text.size(), static_cast<size_t>(selS), fontRef, renderer_);
            float ex = drawX + caretXInRun(text, 0, text.size(), static_cast<size_t>(selE), fontRef, renderer_);
            float top = textY - lm.ascent;
            renderer_->fillRect(sx, top, ex - sx, lm.lineHeight(),
                                selectionWash(selStyle, accentColor_()));
        }
    }

    if (!text.empty()) {
        // Use the element's computed color for text (respects app themes)
        bromath::Color textColor = cfromColor8({0, 0, 0, 255});
        if (elem_) {
            auto& style = elem_->computedStyle();
            auto cIt = style.find("color");
            if (cIt != style.end() && !cIt->second.empty()) {
                bromath::Color parsed;
                if (DrawTraversal::tryParseColor(cIt->second, parsed)) {
                    textColor = parsed;
                }
            }
        }
        bromath::Color color = isPlaceholder ? cfromColor8({128, 128, 128, 180})
                                            : textColor;
        if (isPlaceholder && !phStyle.empty()) {
            // ::placeholder color (inherits the input's color when the rule
            // doesn't set one) with its opacity applied on top.
            pseudoColor(phStyle, "color", color);
            color.a *= pseudoOpacity(phStyle);
        }
        renderer_->drawText(text, drawX, textY, fontRef, color);

        // ::selection color: repaint the selected glyph run over the wash.
        // Drawn as its own run starting at the wash's left edge — the same
        // prefix measurement the wash used, so the glyphs land on themselves.
        // Skipped when the resolved color matches the base text color (a rule
        // that only sets background-color inherits the element's color), so
        // the common case doesn't double-draw anti-aliased edges.
        bromath::Color selColor;
        if (hasSelBand && !selStyle.empty() &&
            pseudoColor(selStyle, "color", selColor) &&
            (selColor.r != color.r || selColor.g != color.g ||
             selColor.b != color.b || selColor.a != color.a)) {
            float sx = drawX + caretXInRun(text, 0, text.size(), static_cast<size_t>(selS),
                                          fontRef, renderer_);
            renderer_->drawText(
                std::string_view(text).substr(static_cast<size_t>(selS),
                                              static_cast<size_t>(selE - selS)),
                sx, textY, fontRef, selColor);
        }
    }

    if (focused_ && isTextType(nullptr)) {
        // Use computed color for cursor too
        bromath::Color cursorColor = cfromColor8({0, 0, 0, 255});
        if (elem_) {
            auto& style = elem_->computedStyle();
            auto cIt = style.find("color");
            if (cIt != style.end() && !cIt->second.empty()) {
                bromath::Color parsed;
                if (DrawTraversal::tryParseColor(cIt->second, parsed)) {
                    cursorColor = parsed;
                }
            }
        }
        float cursorX = drawX + caretOffset;
        float cursorTop = textY - lm.ascent;
        float cursorBottom = cursorTop + lm.lineHeight();
        renderer_->drawLine(cursorX, cursorTop, cursorX, cursorBottom, cursorColor, 1.0f);

        // IME preedit: thin underline under the whole provisional run, just
        // below the baseline (the caret above marks the composition cursor).
        // Password fields underline the mask — its bytes map 1:1 to the value.
        if (comp_.active && comp_.length > 0 && !isPlaceholder) {
            const int n = static_cast<int>(text.size());
            const int ps = std::clamp(comp_.start, 0, n);
            const int pe = std::clamp(comp_.start + comp_.length, ps, n);
            if (pe > ps) {
                float ux0 = drawX + caretXInRun(text, 0, text.size(), static_cast<size_t>(ps),
                                               fontRef, renderer_);
                float ux1 = drawX + caretXInRun(text, 0, text.size(), static_cast<size_t>(pe),
                                               fontRef, renderer_);
                float uy = textY + 2.0f;
                renderer_->drawLine(ux0, uy, ux1, uy, cursorColor, 1.0f);
            }
        }
    }

    // The spin buttons sit outside the text's clip — they own the right edge.
    renderer_->restore();

    if (inputType(nullptr) == InputType::Number) {
        float btnW = kSpinButtonWidth;
        float bx = x + w - btnW;
        renderer_->drawLine(bx, y, bx, y + h, cfromColor8({180, 180, 180, 255}), 1.0f);
        renderer_->drawLine(bx, y + h / 2, bx + btnW, y + h / 2, cfromColor8({180, 180, 180, 255}), 1.0f);

        float cx = bx + btnW / 2;
        render::PointF upPts[3] = {
            {cx - 4, y + h / 4 + 2}, {cx + 4, y + h / 4 + 2}, {cx, y + h / 4 - 2}
        };
        renderer_->drawPolygon(std::span<const render::PointF>(upPts, 3),
                              cfromColor8({80, 80, 80, 255}), cfromColor8({0, 0, 0, 0}), 0.0f);

        render::PointF downPts[3] = {
            {cx - 4, y + h * 3 / 4 - 2}, {cx + 4, y + h * 3 / 4 - 2}, {cx, y + h * 3 / 4 + 2}
        };
        renderer_->drawPolygon(std::span<const render::PointF>(downPts, 3),
                              cfromColor8({80, 80, 80, 255}), cfromColor8({0, 0, 0, 0}), 0.0f);
    }
}

// color-scheme, per CSS Color Adjustment: an element whose computed
// color-scheme includes "dark" gets dark-rendered UA control chrome.
// htmlayout registers the property as inherited, so `body { color-scheme:
// dark }` is enough to theme every form control in an app.
bool ElInput::darkScheme_() const {
    if (!elem_) return false;
    auto& style = elem_->computedStyle();
    auto it = style.find("color-scheme");
    return it != style.end() && it->second.find("dark") != std::string::npos;
}

// CSS accent-color for the "accent parts" of a control: the checked
// checkbox/radio fill and the range fill + thumb. Windows-blue when unset.
Color ElInput::accentColor_() const {
    Color accent = cfromColor8({0, 120, 215, 255});
    if (elem_) {
        auto& style = elem_->computedStyle();
        auto it = style.find("accent-color");
        if (it != style.end() && !it->second.empty() && it->second != "auto") {
            accent = DrawTraversal::parseColor(it->second);
        }
    }
    return accent;
}

void ElInput::drawCheckbox_(float x, float y, float w, float h) {
    float sz = std::min(w, h);
    float bx = x + (w - sz) / 2;
    float by = y + (h - sz) / 2;

    bool dark = darkScheme_();
    bool checked = elem_ && elem_->hasAttribute("checked");
    Color accent = accentColor_();
    Color box = dark ? cfromColor8({43, 47, 56, 255}) : cfromColor8({255, 255, 255, 255});
    Color border = dark ? cfromColor8({110, 118, 130, 255}) : cfromColor8({118, 118, 118, 255});

    // Checked box fills with the accent (Chromium's accent-color behavior);
    // the mark contrasts against that fill, not the scheme.
    renderer_->fillRect(bx, by, sz, sz, checked ? accent : box);
    if (!checked) renderer_->drawRect(bx, by, sz, sz, border);

    if (focused_) {
        Color ring = {accent.r, accent.g, accent.b, 128.0f / 255.0f};
        renderer_->drawRect(bx - 1, by - 1, sz + 2, sz + 2, ring);
    }

    if (checked) {
        Color mark = (accent.r + accent.g + accent.b > 1.5f)
            ? cfromColor8({20, 20, 20, 255}) : cfromColor8({255, 255, 255, 255});
        float pad = sz * 0.2f;
        float x1 = bx + pad, y1 = by + sz * 0.5f;
        float x2 = bx + sz * 0.4f, y2 = by + sz - pad;
        float x3 = bx + sz - pad, y3 = by + pad;
        renderer_->drawLine(x1, y1, x2, y2, mark, 2.0f);
        renderer_->drawLine(x2, y2, x3, y3, mark, 2.0f);
    }
}

void ElInput::drawRadio_(float x, float y, float w, float h) {
    float sz = std::min(w, h);
    float r = sz / 2;
    float cx = x + w / 2, cy = y + h / 2;

    bool dark = darkScheme_();
    bool checked = elem_ && elem_->hasAttribute("checked");
    Color accent = accentColor_();
    Color box = dark ? cfromColor8({43, 47, 56, 255}) : cfromColor8({255, 255, 255, 255});
    Color border = dark ? cfromColor8({110, 118, 130, 255}) : cfromColor8({118, 118, 118, 255});

    renderer_->drawCircle(cx, cy, r, box, checked ? accent : border, checked ? 2.0f : 1.0f);
    if (focused_) {
        Color ring = {accent.r, accent.g, accent.b, 128.0f / 255.0f};
        renderer_->drawCircle(cx, cy, r + 1, cfromColor8({0, 0, 0, 0}), ring, 1.0f);
    }
    if (checked) {
        renderer_->drawCircle(cx, cy, r * 0.45f, accent, cfromColor8({0, 0, 0, 0}), 0.0f);
    }
}

float ElInput::rangeThumbRadius(float h) {
    // Scale with element height, but keep the thumb inside the hit box
    // (diameter <= h). 0.4 * h gives ~8 at the default 20px height.
    float r = std::clamp(h * 0.4f, 2.0f, 10.0f);
    return std::min(r, h * 0.5f);
}

float ElInput::rangeTrackHeight(float h) {
    return std::clamp(h * 0.2f, 2.0f, 6.0f);
}

void ElInput::drawRange_(float x, float y, float w, float h) {
    float trackH = rangeTrackHeight(h);
    float trackY = y + (h - trackH) / 2;
    float thumbR = rangeThumbRadius(h);
    float trackPad = thumbR;

    // Accent color — honor CSS accent-color for the filled track and thumb,
    // falling back to Windows-blue when unset.
    Color accent = accentColor_();
    // Darken the accent ~18% in linear space (was uint8 *0.82 — equivalent
    // multiplicative scale, now correctly applied in linear-light).
    bromath::Color accentDark = {
        accent.r * 0.82f, accent.g * 0.82f, accent.b * 0.82f, accent.a
    };
    bromath::Color focusRing = {accent.r, accent.g, accent.b, 128.0f/255.0f};

    bool dark = darkScheme_();
    Color trackBg = dark ? cfromColor8({52, 58, 68, 255}) : cfromColor8({200, 200, 200, 255});
    renderer_->fillRoundRect(x + trackPad, trackY, w - trackPad * 2, trackH,
                            2, 2, trackBg);

    float mn = rangeMin(), mx = rangeMax();
    float val = rangeValue();
    float span = w - trackPad * 2;
    float pct = (mx > mn) ? (val - mn) / (mx - mn) : 0.0f;
    pct = std::clamp(pct, 0.0f, 1.0f);
    float thumbX = x + trackPad + pct * span;
    float thumbY = y + h / 2;

    // Fill origin: a signed range (min < 0 < max) is a bipolar control — its
    // resting point is 0, not the left edge, so the accent fill grows from the
    // zero position toward the thumb in either direction. A slider at 0 shows
    // no fill at all, which is exactly the "this control is neutral" signal.
    // One-signed ranges keep the usual fill-from-min.
    float fillFrom = x + trackPad;
    if (mn < 0.0f && mx > 0.0f) {
        float zeroPct = (0.0f - mn) / (mx - mn);
        fillFrom = x + trackPad + zeroPct * span;
        // A faint zero tick so the resting point stays visible while dragging.
        Color tick = dark ? cfromColor8({110, 118, 130, 255}) : cfromColor8({140, 140, 140, 255});
        renderer_->fillRect(fillFrom - 0.5f, trackY - 2, 1, trackH + 4, tick);
    }
    float fx0 = std::min(fillFrom, thumbX), fx1 = std::max(fillFrom, thumbX);
    if (fx1 > fx0) renderer_->fillRoundRect(fx0, trackY, fx1 - fx0, trackH, 2, 2, accent);

    bromath::Color thumbFill = dragging_ ? accentDark : accent;
    Color thumbRim = dark ? cfromColor8({16, 18, 24, 255}) : cfromColor8({255, 255, 255, 255});
    renderer_->drawCircle(thumbX, thumbY, thumbR, thumbFill, thumbRim, 1.5f);

    if (focused_) {
        renderer_->drawCircle(thumbX, thumbY, thumbR + 2, cfromColor8({0, 0, 0, 0}), focusRing, 1.5f);
    }
}

void ElInput::drawColor_(float x, float y, float w, float h) {
    std::string val = getAttr("value");
    bromath::Color swatch = cfromColor8({0, 0, 0, 255});
    if (val.size() == 7 && val[0] == '#') {
        swatch = bromath::cfromHex(val.c_str());
    }

    float pad = 3.0f;
    renderer_->drawRect(x, y, w, h, cfromColor8({118, 118, 118, 255}));
    renderer_->fillRect(x + pad, y + pad, w - pad * 2, h - pad * 2, swatch);
    if (focused_) {
        renderer_->drawRect(x - 1, y - 1, w + 2, h + 2, cfromColor8({0, 120, 215, 255}));
    }
}

} // namespace bro::layout
