#pragma once

#include "render/renderer.h"

#include <cstdint>
#include <variant>

namespace bro::render {

// One draw command in a CommandBuffer. There is exactly one struct per
// `Renderer` virtual method (plus LayerBreak, which is not on Renderer but is
// emitted by DrawTraversal's layer-break callback when it crosses a
// canvas/WebGL boundary). All payloads are POD; variable-length data (strings,
// color stops, polygon points, filter chains, image bytes) is referenced by
// (offset, len) into the buffer's side arena.

struct Cmd_Clear              { Color color; };
struct Cmd_FillRect           { float x, y, w, h; Color color; };
struct Cmd_DrawRect           { float x, y, w, h; Color color; };
struct Cmd_FillRoundRect      { float x, y, w, h, rx, ry; Color color; };
struct Cmd_DrawRoundRect      { float x, y, w, h, rx, ry; Color color; };
struct Cmd_FillRoundRectRadii { float x, y, w, h; Radii r; Color color; };
struct Cmd_DrawRoundRectRadii { float x, y, w, h; Radii r; float strokeWidth; Color color; };
struct Cmd_DrawBoxShadow      { float x, y, w, h, rx, ry, offsetX, offsetY, blur, spread; Color color; bool inset; };
struct Cmd_DrawBoxShadowRadii { float x, y, w, h; Radii r; float offsetX, offsetY, blur, spread; Color color; bool inset; };

// Text. The string and font family live in the arena. The recording renderer
// embeds the full font descriptor here so the raster thread can recreate the
// font against its own renderer at replay time — fonts are never shared
// across renderers.
struct Cmd_DrawText {
    uint32_t textOffset, textLen;     // arena: char[]
    uint32_t familyOffset, familyLen; // arena: char[]
    float x, y;
    float fontSize;
    int   fontWeight;
    bool  fontItalic;
    float letterSpacing;              // 0 -> plain drawText path
    float blur;                       // text-shadow halo; 0 if none
    Color color;
};

struct Cmd_DrawLine        { float x1, y1, x2, y2; Color color; float thickness; };
struct Cmd_DrawImage       { uint32_t dataOffset, dataLen; float x, y, w, h; };  // arena: encoded bytes
struct Cmd_DrawPixelsRGBA  { uint32_t pixelsOffset; int srcW, srcH, stride; float x, y, w, h; }; // arena: rgba8
struct Cmd_DrawCircle      { float cx, cy, r; Color fill; Color stroke; float strokeWidth; };
struct Cmd_DrawEllipse     { float cx, cy, rx, ry; Color fill; Color stroke; float strokeWidth; };
struct Cmd_DrawPath        { uint32_t pathOffset, pathLen; Color fill; Color stroke; float strokeWidth; }; // arena: char[] (svg path)
struct Cmd_DrawPolygon     { uint32_t pointsOffset, pointsLen; Color fill; Color stroke; float strokeWidth; }; // arena: PointF[]
struct Cmd_DrawPolyline    { uint32_t pointsOffset, pointsLen; Color stroke; float strokeWidth; }; // arena: PointF[]

struct Cmd_Save              {};
struct Cmd_Restore           {};
struct Cmd_SaveLayerAlpha    { uint8_t alpha; };
struct Cmd_SaveLayerWithFilter { uint32_t filtersOffset, filtersLen; float x, y, w, h; }; // arena: CssFilterParams[]
struct Cmd_Translate         { float dx, dy; };
struct Cmd_Scale             { float sx, sy; };
struct Cmd_Rotate            { float degrees; };
struct Cmd_Concat            { float a, b, c, d, e, f; };
struct Cmd_Concat4x4         { float m[16]; };
struct Cmd_SetClip           { float x, y, w, h; };
struct Cmd_SetClipRRect      { float x, y, w, h; Radii r; };
struct Cmd_ResetClip         {};
struct Cmd_SetClipPolygon    { uint32_t pointsOffset, pointsLen; };  // arena: PointF[]

struct Cmd_FillLinearGradient { float x, y, w, h, startX, startY, endX, endY; uint32_t stopsOffset, stopsLen; }; // arena: ColorStop[]
struct Cmd_FillRadialGradient { float x, y, w, h, cx, cy, rx, ry;             uint32_t stopsOffset, stopsLen; }; // arena: ColorStop[]
struct Cmd_FillConicGradient  { float x, y, w, h, cx, cy, angleDeg;            uint32_t stopsOffset, stopsLen; }; // arena: ColorStop[]

struct Cmd_BeginFrame { int width, height; };
struct Cmd_EndFrame   {};

// Emitted by DrawTraversal's layer-break callback. The replayer flushes the
// current Skia surface into a UILayer, pushes a Canvas/WebGL UILayer, and
// allocates a fresh Skia surface for subsequent HTML commands.
// `canvasScene` is an opaque pointer (canvas::CanvasScene*) that survives the
// emit→replay window — CanvasScenes are owned by the engine across frames.
struct Cmd_LayerBreak {
    enum LayerKind {
        Canvas2D,        // flush surface + push HTML layer + push Canvas2D layer + new surface
        WebGL,           // flush surface + push HTML layer + push WebGL layer + new surface
        HtmlSurface,     // flush surface + push HTML layer + new surface (panel boundary)
    };
    int kind;                    // LayerKind
    void* canvasScene;           // canvas::CanvasScene* when kind==Canvas2D
    unsigned int directTexture;  // GL texture id when kind==WebGL
    float x, y, w, h;            // ignored for HtmlSurface
};

// System-panel canvas: composite the canvas scene's snapshot onto the current
// surface (no layer split). Replayer: scene->flushStaged(), snapshot, drawImage.
struct Cmd_BlitCanvasInline {
    void* canvasScene;           // canvas::CanvasScene*
    float x, y, w, h;
};

using DrawCommand = std::variant<
    Cmd_Clear,
    Cmd_FillRect,
    Cmd_DrawRect,
    Cmd_FillRoundRect,
    Cmd_DrawRoundRect,
    Cmd_FillRoundRectRadii,
    Cmd_DrawRoundRectRadii,
    Cmd_DrawBoxShadow,
    Cmd_DrawBoxShadowRadii,
    Cmd_DrawText,
    Cmd_DrawLine,
    Cmd_DrawImage,
    Cmd_DrawPixelsRGBA,
    Cmd_DrawCircle,
    Cmd_DrawEllipse,
    Cmd_DrawPath,
    Cmd_DrawPolygon,
    Cmd_DrawPolyline,
    Cmd_Save,
    Cmd_Restore,
    Cmd_SaveLayerAlpha,
    Cmd_SaveLayerWithFilter,
    Cmd_Translate,
    Cmd_Scale,
    Cmd_Rotate,
    Cmd_Concat,
    Cmd_Concat4x4,
    Cmd_SetClip,
    Cmd_SetClipRRect,
    Cmd_ResetClip,
    Cmd_SetClipPolygon,
    Cmd_FillLinearGradient,
    Cmd_FillRadialGradient,
    Cmd_FillConicGradient,
    Cmd_BeginFrame,
    Cmd_EndFrame,
    Cmd_LayerBreak,
    Cmd_BlitCanvasInline
>;

} // namespace bro::render
