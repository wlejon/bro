#pragma once

#include "render/layer_source.h"
#include "render/renderer.h"

#include <cstdint>
#include <variant>

namespace bro::render {

// One draw command in a CommandBuffer. There is exactly one struct per
// `Renderer` virtual method (plus the layer/surface breaks and the inline
// canvas blit, which are not on Renderer: the engine records them where a
// document reaches separately composited content). All payloads are POD;
// variable-length data (strings, color stops, polygon points, filter chains,
// image bytes) is referenced by (offset, len) into the buffer's side arena.

struct Cmd_Clear              { bromath::Color color; };
struct Cmd_FillRect           { float x, y, w, h; bromath::Color color; };
struct Cmd_DrawRect           { float x, y, w, h; bromath::Color color; };
struct Cmd_FillRoundRect      { float x, y, w, h, rx, ry; bromath::Color color; };
struct Cmd_DrawRoundRect      { float x, y, w, h, rx, ry; bromath::Color color; };
struct Cmd_FillRoundRectRadii { float x, y, w, h; Radii r; bromath::Color color; };
struct Cmd_DrawRoundRectRadii { float x, y, w, h; Radii r; float strokeWidth; bromath::Color color; };
struct Cmd_DrawBoxShadow      { float x, y, w, h, rx, ry, offsetX, offsetY, blur, spread; bromath::Color color; bool inset; };
struct Cmd_DrawBoxShadowRadii { float x, y, w, h; Radii r; float offsetX, offsetY, blur, spread; bromath::Color color; bool inset; };

// Text. The string and font family live in the arena. The recording renderer
// embeds the full font descriptor here so the raster thread can recreate the
// font against its own renderer at replay time — fonts are never shared
// across renderers.
struct Cmd_DrawText {
    // Shaped path: `blobIndex` names a pre-shaped SkTextBlob in the buffer's
    // blob table and the replayer just draws it — no shaping, no font
    // fallback, no re-measure on the raster thread. The string/font fields
    // stay as the fallback for backends with no shaper.
    uint32_t blobIndex;               // CommandBuffer::kNoTextBlob if unshaped
    uint32_t textOffset, textLen;     // arena: char[]
    uint32_t familyOffset, familyLen; // arena: char[]
    uint32_t featuresOffset, featuresLen; // arena: char[] (FontRef::features; len 0 = none)
    float x, y;
    float fontSize;
    int   fontWeight;
    bool  fontItalic;
    bool  fontLigatures;              // FontRef::ligatures
    float letterSpacing;              // 0 -> plain drawText path
    float wordSpacing;                // extra advance per space char; 0 if none
    float blur;                       // text-shadow halo; 0 if none
    bromath::Color color;
};

struct Cmd_DrawLine        { float x1, y1, x2, y2; bromath::Color color; float thickness; };
struct Cmd_DrawImage       { uint32_t dataOffset, dataLen; float x, y, w, h; uint64_t imageId; };  // arena: encoded bytes
struct Cmd_DrawPixelsRGBA  { uint32_t pixelsOffset; int srcW, srcH, stride; float x, y, w, h; }; // arena: rgba8
struct Cmd_DrawSvgMarkup   { uint32_t dataOffset, dataLen; float x, y, w, h; };  // arena: utf8 markup
// Shared pixels by reference: `index` names an entry of the buffer's
// SharedPixels table (CommandBuffer::pushSharedPixels), which keeps them alive.
struct Cmd_DrawSharedPixels { uint32_t index; float sx, sy, sw, sh, x, y, w, h; };
struct Cmd_DrawCircle      { float cx, cy, r; bromath::Color fill; bromath::Color stroke; float strokeWidth; };
struct Cmd_DrawEllipse     { float cx, cy, rx, ry; bromath::Color fill; bromath::Color stroke; float strokeWidth; };
struct Cmd_DrawPath        { uint32_t pathOffset, pathLen; bromath::Color fill; bromath::Color stroke; float strokeWidth; }; // arena: char[] (svg path)
struct Cmd_DrawPolygon     { uint32_t pointsOffset, pointsLen; bromath::Color fill; bromath::Color stroke; float strokeWidth; }; // arena: PointF[]
struct Cmd_DrawPolyline    { uint32_t pointsOffset, pointsLen; bromath::Color stroke; float strokeWidth; }; // arena: PointF[]
// SVG path with paint-server fill/stroke + full stroke styling. Path 'd',
// gradient stop lists, and the dash array all live in the arena.
struct Cmd_DrawSvgPath {
    uint32_t pathOffset, pathLen;               // arena: char[] (svg path 'd')
    uint32_t fillStopsOffset, fillStopsLen;     // arena: ColorStop[]
    uint32_t strokeStopsOffset, strokeStopsLen; // arena: ColorStop[]
    uint32_t dashArrOffset, dashArrLen;         // arena: float[]
    GradientPaint fill;
    GradientPaint stroke;
    StrokeStyle strokeStyle;
    PathFillRule rule;
};
struct Cmd_ClipSvgPath { uint32_t pathOffset, pathLen; PathFillRule rule; }; // arena: char[]

struct Cmd_Save              {};
struct Cmd_Restore           {};
struct Cmd_SaveLayerAlpha    { uint8_t alpha; };
struct Cmd_SaveLayerWithFilter { uint32_t filtersOffset, filtersLen; float x, y, w, h; }; // arena: CssFilterParams[]
struct Cmd_SaveLayerWithBlend  { BlendMode mode; };
struct Cmd_DrawBackdropFilter  { uint32_t filtersOffset, filtersLen; float x, y, w, h; Radii r; float opacity; }; // arena: CssFilterParams[]
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

// Emitted by DrawTraversal's layer-break callback where it reaches content
// composited as its own layer (a canvas, WebGL, a 3D scene, an iframe). The
// replayer's handler ends the current HTML surface, records the layer, and
// starts a fresh surface for the HTML painted after it. `source` names what
// the layer shows by handle (see layer_source.h), never by pointer.
struct Cmd_LayerBreak {
    LayerSource source;
    LayerQuad quad;
};

// A boundary between HTML surfaces with no layer in between (one system panel
// ending, the next beginning): the handler ends the current surface and
// starts a fresh one.
struct Cmd_SurfaceBreak {};

// System-panel canvas: composite the canvas scene's snapshot onto the current
// surface (no layer split). Replayer: scene->flushStaged(), snapshot, drawImage.
// Keeps a raw pointer (unlike Cmd_LayerBreak's handle) because replay
// dereferences the scene on the raster thread; safety comes from deferred destruction — scenes
// are only freed at frame top with the raster worker idle, and a detached
// scene is skipped at record time so it never enters a fresh buffer.
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
    Cmd_DrawSvgMarkup,
    Cmd_DrawSharedPixels,
    Cmd_DrawCircle,
    Cmd_DrawEllipse,
    Cmd_DrawPath,
    Cmd_DrawPolygon,
    Cmd_DrawPolyline,
    Cmd_DrawSvgPath,
    Cmd_ClipSvgPath,
    Cmd_Save,
    Cmd_Restore,
    Cmd_SaveLayerAlpha,
    Cmd_SaveLayerWithFilter,
    Cmd_SaveLayerWithBlend,
    Cmd_DrawBackdropFilter,
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
    Cmd_SurfaceBreak,
    Cmd_BlitCanvasInline
>;

} // namespace bro::render
