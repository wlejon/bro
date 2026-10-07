#include "render/software_cursor.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkBlurTypes.h>

namespace bro::render {

namespace {

void drawVectorCursor(SkCanvas* canvas, float x, float y, float scale,
                      float hotX, float hotY,
                      const SkPath& fillPath, const SkPath& strokePath,
                      SkColor fillColor = SK_ColorWHITE,
                      SkColor strokeColor = SK_ColorBLACK,
                      float strokeWidth = 1.25f,
                      bool hasShadow = true) {
    if (!canvas) return;

    canvas->save();
    canvas->translate(x - hotX * scale, y - hotY * scale);
    canvas->scale(scale, scale);

    if (hasShadow && !fillPath.isEmpty()) {
        SkPaint shadowPaint;
        shadowPaint.setColor(SkColorSetARGB(80, 0, 0, 0));
        shadowPaint.setStyle(SkPaint::kFill_Style);
        shadowPaint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 1.5f));
        shadowPaint.setAntiAlias(true);
        canvas->save();
        canvas->translate(1.0f, 1.5f);
        canvas->drawPath(fillPath, shadowPaint);
        canvas->restore();
    }

    if (fillColor != SK_ColorTRANSPARENT && !fillPath.isEmpty()) {
        SkPaint fillPaint;
        fillPaint.setColor(fillColor);
        fillPaint.setStyle(SkPaint::kFill_Style);
        fillPaint.setAntiAlias(true);
        canvas->drawPath(fillPath, fillPaint);
    }

    if (strokeColor != SK_ColorTRANSPARENT && strokeWidth > 0.0f) {
        SkPaint strokePaint;
        strokePaint.setColor(strokeColor);
        strokePaint.setStyle(SkPaint::kStroke_Style);
        strokePaint.setStrokeWidth(strokeWidth);
        strokePaint.setStrokeJoin(SkPaint::kRound_Join);
        strokePaint.setStrokeCap(SkPaint::kRound_Cap);
        strokePaint.setAntiAlias(true);
        canvas->drawPath(strokePath.isEmpty() ? fillPath : strokePath, strokePaint);
    }

    canvas->restore();
}

void drawArrow(SkCanvas* canvas, float x, float y, float scale) {
    SkPathBuilder b;
    b.moveTo(0.0f, 0.0f);
    b.lineTo(0.0f, 17.0f);
    b.lineTo(4.2f, 13.0f);
    b.lineTo(7.5f, 20.0f);
    b.lineTo(10.0f, 18.8f);
    b.lineTo(6.8f, 12.0f);
    b.lineTo(12.0f, 12.0f);
    b.close();
    SkPath p = b.detach();

    drawVectorCursor(canvas, x, y, scale, 0.0f, 0.0f, p, p,
                     SK_ColorWHITE, SK_ColorBLACK, 1.25f, true);
}

void drawPointer(SkCanvas* canvas, float x, float y, float scale) {
    SkPathBuilder b;
    b.moveTo(4.5f, 1.0f);
    b.lineTo(7.0f, 1.0f);
    b.lineTo(7.0f, 9.0f);
    b.lineTo(9.5f, 9.0f);
    b.lineTo(9.5f, 11.0f);
    b.lineTo(12.0f, 11.0f);
    b.lineTo(12.0f, 13.5f);
    b.lineTo(14.0f, 13.5f);
    b.lineTo(14.0f, 18.0f);
    b.lineTo(11.5f, 21.5f);
    b.lineTo(4.5f, 21.5f);
    b.lineTo(1.5f, 17.0f);
    b.lineTo(2.8f, 12.0f);
    b.lineTo(4.5f, 13.5f);
    b.close();
    SkPath p = b.detach();

    drawVectorCursor(canvas, x, y, scale, 4.5f, 1.0f, p, p,
                     SK_ColorWHITE, SK_ColorBLACK, 1.25f, true);
}

void drawIBeam(SkCanvas* canvas, float x, float y, float scale) {
    SkPathBuilder b;
    // Top bar
    b.moveTo(1.0f, 1.0f);
    b.lineTo(7.0f, 1.0f);
    // Vertical stem
    b.moveTo(4.0f, 1.0f);
    b.lineTo(4.0f, 17.0f);
    // Bottom bar
    b.moveTo(1.0f, 17.0f);
    b.lineTo(7.0f, 17.0f);
    SkPath p = b.detach();

    // Draw dark outer stroke, then white inner stroke for high contrast
    canvas->save();
    canvas->translate(x - 4.0f * scale, y - 9.0f * scale);
    canvas->scale(scale, scale);

    SkPaint bgStroke;
    bgStroke.setColor(SK_ColorBLACK);
    bgStroke.setStyle(SkPaint::kStroke_Style);
    bgStroke.setStrokeWidth(3.0f);
    bgStroke.setStrokeCap(SkPaint::kRound_Cap);
    bgStroke.setAntiAlias(true);
    canvas->drawPath(p, bgStroke);

    SkPaint fgStroke;
    fgStroke.setColor(SK_ColorWHITE);
    fgStroke.setStyle(SkPaint::kStroke_Style);
    fgStroke.setStrokeWidth(1.2f);
    fgStroke.setStrokeCap(SkPaint::kRound_Cap);
    fgStroke.setAntiAlias(true);
    canvas->drawPath(p, fgStroke);

    canvas->restore();
}

void drawCrosshair(SkCanvas* canvas, float x, float y, float scale) {
    canvas->save();
    canvas->translate(x, y);
    canvas->scale(scale, scale);

    SkPathBuilder b;
    // Center circle
    b.addCircle(0.0f, 0.0f, 4.5f);
    // 4 ticks
    b.moveTo(0.0f, -8.0f); b.lineTo(0.0f, -4.5f);
    b.moveTo(0.0f, 4.5f);  b.lineTo(0.0f, 8.0f);
    b.moveTo(-8.0f, 0.0f); b.lineTo(-4.5f, 0.0f);
    b.moveTo(4.5f, 0.0f);  b.lineTo(8.0f, 0.0f);
    SkPath p = b.detach();

    SkPaint bg;
    bg.setColor(SK_ColorBLACK);
    bg.setStyle(SkPaint::kStroke_Style);
    bg.setStrokeWidth(3.0f);
    bg.setAntiAlias(true);
    canvas->drawPath(p, bg);

    SkPaint fg;
    fg.setColor(SK_ColorWHITE);
    fg.setStyle(SkPaint::kStroke_Style);
    fg.setStrokeWidth(1.2f);
    fg.setAntiAlias(true);
    canvas->drawPath(p, fg);

    canvas->restore();
}

void drawMove(SkCanvas* canvas, float x, float y, float scale) {
    SkPathBuilder b;
    // North arrow
    b.moveTo(9.0f, 1.0f); b.lineTo(6.5f, 4.0f); b.lineTo(8.0f, 4.0f);
    b.lineTo(8.0f, 8.0f);
    // West arrow
    b.lineTo(4.0f, 8.0f); b.lineTo(4.0f, 6.5f); b.lineTo(1.0f, 9.0f);
    b.lineTo(4.0f, 11.5f); b.lineTo(4.0f, 10.0f); b.lineTo(8.0f, 10.0f);
    // South arrow
    b.lineTo(8.0f, 14.0f); b.lineTo(6.5f, 14.0f); b.lineTo(9.0f, 17.0f);
    b.lineTo(11.5f, 14.0f); b.lineTo(10.0f, 14.0f); b.lineTo(10.0f, 10.0f);
    // East arrow
    b.lineTo(14.0f, 10.0f); b.lineTo(14.0f, 11.5f); b.lineTo(17.0f, 9.0f);
    b.lineTo(14.0f, 6.5f); b.lineTo(14.0f, 8.0f); b.lineTo(10.0f, 8.0f);
    b.lineTo(10.0f, 4.0f); b.lineTo(11.5f, 4.0f);
    b.close();
    SkPath p = b.detach();

    drawVectorCursor(canvas, x, y, scale, 9.0f, 9.0f, p, p,
                     SK_ColorWHITE, SK_ColorBLACK, 1.25f, true);
}

void drawResizeEW(SkCanvas* canvas, float x, float y, float scale) {
    SkPathBuilder b;
    b.moveTo(1.0f, 5.0f);
    b.lineTo(4.5f, 2.0f); b.lineTo(4.5f, 4.0f);
    b.lineTo(13.5f, 4.0f); b.lineTo(13.5f, 2.0f);
    b.lineTo(17.0f, 5.0f);
    b.lineTo(13.5f, 8.0f); b.lineTo(13.5f, 6.0f);
    b.lineTo(4.5f, 6.0f); b.lineTo(4.5f, 8.0f);
    b.close();
    SkPath p = b.detach();

    drawVectorCursor(canvas, x, y, scale, 9.0f, 5.0f, p, p,
                     SK_ColorWHITE, SK_ColorBLACK, 1.25f, true);
}

void drawResizeNS(SkCanvas* canvas, float x, float y, float scale) {
    SkPathBuilder b;
    b.moveTo(5.0f, 1.0f);
    b.lineTo(8.0f, 4.5f); b.lineTo(6.0f, 4.5f);
    b.lineTo(6.0f, 13.5f); b.lineTo(8.0f, 13.5f);
    b.lineTo(5.0f, 17.0f);
    b.lineTo(2.0f, 13.5f); b.lineTo(4.0f, 13.5f);
    b.lineTo(4.0f, 4.5f); b.lineTo(2.0f, 4.5f);
    b.close();
    SkPath p = b.detach();

    drawVectorCursor(canvas, x, y, scale, 5.0f, 9.0f, p, p,
                     SK_ColorWHITE, SK_ColorBLACK, 1.25f, true);
}

void drawNotAllowed(SkCanvas* canvas, float x, float y, float scale) {
    canvas->save();
    canvas->translate(x, y);
    canvas->scale(scale, scale);

    SkPaint bg;
    bg.setColor(SK_ColorBLACK);
    bg.setStyle(SkPaint::kStroke_Style);
    bg.setStrokeWidth(3.0f);
    bg.setAntiAlias(true);
    canvas->drawCircle(0.0f, 0.0f, 7.0f, bg);
    canvas->drawLine(-4.9f, -4.9f, 4.9f, 4.9f, bg);

    SkPaint fg;
    fg.setColor(SkColorSetRGB(220, 40, 40));
    fg.setStyle(SkPaint::kStroke_Style);
    fg.setStrokeWidth(1.8f);
    fg.setAntiAlias(true);
    canvas->drawCircle(0.0f, 0.0f, 7.0f, fg);
    canvas->drawLine(-4.9f, -4.9f, 4.9f, 4.9f, fg);

    canvas->restore();
}

} // namespace

void drawSoftwareCursor(SkCanvas* canvas, float x, float y, const std::string& shape, float scale) {
    if (!canvas || shape == "none" || shape.empty()) return;

    if (scale <= 0.0f) scale = 1.0f;

    if (shape == "pointer" || shape == "hand") {
        drawPointer(canvas, x, y, scale);
    } else if (shape == "text" || shape == "vertical-text" || shape == "xterm") {
        drawIBeam(canvas, x, y, scale);
    } else if (shape == "crosshair" || shape == "cell") {
        drawCrosshair(canvas, x, y, scale);
    } else if (shape == "move") {
        drawMove(canvas, x, y, scale);
    } else if (shape == "ew-resize" || shape == "col-resize") {
        drawResizeEW(canvas, x, y, scale);
    } else if (shape == "ns-resize" || shape == "row-resize") {
        drawResizeNS(canvas, x, y, scale);
    } else if (shape == "not-allowed" || shape == "no-drop") {
        drawNotAllowed(canvas, x, y, scale);
    } else {
        // "default", "auto", "wait", "progress", etc.
        drawArrow(canvas, x, y, scale);
    }
}

} // namespace bro::render
