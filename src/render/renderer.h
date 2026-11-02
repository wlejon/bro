#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string_view>
#include <string>
#include <span>
#include <vector>

#include <bromath/color.h>

// For TextDirection. bidi.h is deliberately ICU-free (see its header comment),
// so this costs nothing but an enum.
#include "render/bidi.h"

class SkCanvas;
class SkSurface;
class SkTextBlob;

namespace bro::render {

class TextShapingEngine;
class SkiaGpu;
class ShapedRun;

// Color is bromath::Color — linear-float RGBA in [0,1]. Per-namespace alias
// would be a header-level `using` (banned). Spell `bromath::Color` everywhere
// in headers; .cpp files may `using bromath::Color;` for ergonomics.

struct TextMetrics {
    float width = 0.0f;
    float height = 0.0f;
    float ascent = 0.0f;   // distance from baseline to top (positive value)
    float descent = 0.0f;  // distance from baseline to bottom (positive value)
    float leading = 0.0f;  // recommended extra line gap (SkFontMetrics::fLeading)
    float xHeight = 0.0f;  // height of lowercase 'x' (SkFontMetrics::fXHeight); 0 if unknown
};

// Derived font line metrics for text layout and vertical centering.
// Use lineHeight() (ascent + descent) for centering, not TextMetrics::height
// which is the tight bounding box of a specific glyph.
struct LineMetrics {
    float ascent;
    float descent;
    float leading = 0.0f;

    float lineHeight() const { return ascent + descent; }

    // Compute baseline Y for text vertically centered in a box of height h at position y
    float baselineY(float y, float h) const {
        return y + (h - lineHeight()) / 2.0f + ascent;
    }

    static LineMetrics from(const TextMetrics& tm) {
        return {
            tm.ascent > 0 ? tm.ascent : tm.height * 0.8f,
            tm.descent > 0 ? tm.descent : tm.height * 0.2f,
            tm.leading
        };
    }
};

struct PointF {
    float x = 0;
    float y = 0;
};

// Per-corner border radii (CSS border-radius). Each corner has independent
// horizontal (x) and vertical (y) radii. Order matches SkRRect corner enum:
// [0]=top-left, [1]=top-right, [2]=bottom-right, [3]=bottom-left.
struct Radii {
    float x[4] = {0, 0, 0, 0};
    float y[4] = {0, 0, 0, 0};
    bool isZero() const {
        return x[0] == 0 && x[1] == 0 && x[2] == 0 && x[3] == 0 &&
               y[0] == 0 && y[1] == 0 && y[2] == 0 && y[3] == 0;
    }
};

struct ColorStop {
    float offset;   // 0.0–1.0
    bromath::Color color;
};

// Value-typed font reference — replaces the opaque `uint64_t font_handle` that
// drawText/measureText used to take. Each renderer caches by content
// internally, so the same FontRef across calls hits the same cached SkFont
// without needing a per-renderer handle to be threaded through layout. The
// `family` view must outlive the call but does not need to outlive the
// returned cache entry — renderer implementations copy the family into their
// own keys on cache miss.
struct FontRef {
    std::string_view family;
    float            size = 14.0f;
    int              weight = 400;   // CSS numeric (400 = normal, 700 = bold)
    bool             italic = false;
    // Ligature features (liga, clig, calt, ...). False draws one glyph per
    // character, as `font-variant-ligatures: none` does (the terminal grid).
    bool             ligatures = true;
    // OpenType features from font-variant-numeric / font-feature-settings, in
    // htmlayout's canonical form ("tnum=1,zero=1"; see css/font_features.h).
    // Empty = none, which shapes exactly as before. Like `family`, the view
    // must outlive the call; the shaper copies it into its cache key. Part of
    // what a run measures, so layout's TextMetrics carries the same string.
    std::string_view features;
    // Glyphs as geometry, the way SVG text is drawn: unhinted outlines and
    // advances (no grid fitting, no baseline snap), so a run measures, fills,
    // strokes and bends along a textPath as its outlines are, and scales
    // with its transform. HTML text leaves it off and keeps the hinted font.
    bool             geometric = false;
};

// CSS filter primitive — a single function in a `filter:` chain. The list is
// applied in order to produce the final image filter. Backends are responsible
// for translating these descriptors into their native filter representation
// (e.g. SkImageFilter on Skia backends). DrawTraversal records descriptors
// only; it does not construct backend filter objects.
struct CssFilterParams {
    enum Kind {
        Blur,         // a = sigma (px)
        Brightness,   // a = factor
        Contrast,     // a = factor
        Grayscale,    // a = amount [0..1]
        Sepia,        // a = amount [0..1]
        Saturate,     // a = factor
        HueRotate,    // a = degrees
        Invert,       // a = amount [0..1]
        Opacity,      // a = amount [0..1]
        DropShadow,   // dx, dy, blur (px); shadowColor
    };
    Kind kind = Blur;
    float a = 0;
    float dx = 0;
    float dy = 0;
    float blur = 0;
    bromath::Color shadowColor = {0, 0, 0, 1};
};

// CSS mix-blend-mode values. Backends map these to their native blend modes
// (e.g. SkBlendMode on Skia backends). `Normal` is plain source-over.
enum class BlendMode {
    Normal,
    Multiply,
    Screen,
    Overlay,
    Darken,
    Lighten,
    ColorDodge,
    ColorBurn,
    HardLight,
    SoftLight,
    Difference,
    Exclusion,
    Hue,
    Saturation,
    Color,
    Luminosity,
};

// SVG paint server — a resolved fill or stroke paint in USER (local) space, as
// produced by the SVG paint traversal. Gradient stops are passed as a separate
// span<ColorStop> so this stays a trivially-copyable POD (it is stored inline in
// recorded draw commands). Gradient geometry is already resolved to user space;
// `transform` carries the SVG gradientTransform (CSS matrix a,b,c,d,e,f).
struct GradientPaint {
    enum class Kind : uint8_t { None, Solid, Linear, Radial };
    enum class Spread : uint8_t { Pad, Reflect, Repeat };
    Kind   kind = Kind::Solid;
    Spread spread = Spread::Pad;
    bromath::Color color{0, 0, 0, 1};          // Solid
    float  p0[2] = {0, 0};                      // Linear: start point
    float  p1[2] = {0, 0};                      // Linear: end point
    float  center[2] = {0, 0};                  // Radial: circle center
    float  radius = 0.0f;                        // Radial: circle radius
    float  focal[2] = {0, 0};                    // Radial: focal point
    bool   hasFocal = false;                     // focal != center
    float  transform[6] = {1, 0, 0, 1, 0, 0};    // gradientTransform (a,b,c,d,e,f)
};

// SVG stroke styling. The dash array is passed as a separate span<float> (even
// length ≥ 2, or empty for a solid stroke).
struct StrokeStyle {
    enum class Cap : uint8_t { Butt, Round, Square };
    enum class Join : uint8_t { Miter, Round, Bevel };
    float width = 1.0f;
    float miterLimit = 4.0f;
    float dashOffset = 0.0f;
    Cap   cap = Cap::Butt;
    Join  join = Join::Miter;
};

enum class PathFillRule : uint8_t { NonZero, EvenOdd };

// How an SVG mask layer turns what is drawn into it into coverage
// (Renderer::saveLayerMask): its luminance times its alpha (mask-type:
// luminance, the SVG default) or its alpha alone.
enum class MaskMode : uint8_t { Luminance, Alpha };

// One primitive of an SVG <filter>, resolved to user space by the SVG paint
// traversal (layout/svg_filter.cpp). A filter is a list of these in document
// order; `in`/`in2` name an earlier primitive by index, or one of the
// standard inputs below. Variable-length data (merge inputs, matrix values,
// transfer tables) lives in a float span passed beside the
// list, addressed by (dataOffset, dataLen). Trivially copyable: stored inline
// in recorded draw commands.
struct SvgFilterPrimitive {
    enum class Kind : uint8_t {
        GaussianBlur,      // p0, p1 = stdDeviation x, y
        Offset,            // p0, p1 = dx, dy
        ColorMatrix,       // data = 20 values (row-major 4x5, offsets in 0..1)
        Blend,             // mode = BlendMode
        Composite,         // mode = CompositeOp; p0..p3 = k1..k4 (arithmetic)
        Flood,             // color
        Merge,             // data = input indices
        DropShadow,        // p0, p1 = dx, dy; p2, p3 = stdDeviation x, y; color
        ComponentTransfer, // data = 4 transfer functions (see layout/svg_filter.cpp)
        Morphology,        // mode = 0 erode, 1 dilate; p0, p1 = radius x, y
        Turbulence,        // mode = 0 turbulence, 1 fractalNoise; p0, p1 = base
                           // frequency; p2 = octaves; p3 = seed; p4 = stitch
        Tile,              // tiles `in`'s subregion over this one
        DisplacementMap,   // mode = x channel | y channel << 4 (0 R, 1 G, 2 B, 3 A); p0 = scale
        DiffuseLighting,   // p0 = surfaceScale, p1 = diffuseConstant; color; light below
        SpecularLighting,  // p0 = surfaceScale, p1 = specularConstant, p2 = exponent; color
        Image,             // p0..p3 = dst x, y, w, h; p4, p5 = src w, h; data = packed RGBA8
        Passthrough,       // `in`, unchanged (unknown primitives)
    };
    enum CompositeOp : uint8_t { Over, In, Out, Atop, Xor, Arithmetic };
    static constexpr int16_t kSourceGraphic = -1;
    static constexpr int16_t kSourceAlpha = -2;

    Kind    kind = Kind::Passthrough;
    uint8_t mode = 0;
    bool    linearRGB = true;          // color-interpolation-filters
    int16_t in = kSourceGraphic;
    int16_t in2 = kSourceGraphic;
    float   x = 0, y = 0, w = 0, h = 0;  // primitive subregion, user space
    float   p[6] = {0, 0, 0, 0, 0, 0};
    bromath::Color color{0, 0, 0, 1};
    // Lighting: light kind (0 distant, 1 point, 2 spot) and its parameters
    // (distant: azimuth, elevation; point: x, y, z; spot: x, y, z, pointsAt
    // x, y, z, specularExponent, limitingConeAngle).
    uint8_t lightKind = 0;
    float   light[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t dataOffset = 0, dataLen = 0;
};

// How a scaled image is sampled (CSS image-rendering). Smooth is bilinear,
// mipmapped where the image is drawn smaller than it is (auto, smooth,
// high-quality); Pixelated is nearest-neighbour (pixelated, crisp-edges).
enum class ImageSampling : uint8_t { Smooth, Pixelated };

// Immutable RGBA8 pixels drawn by reference (Renderer::drawSharedPixels):
// a recording keeps `owner`, never a copy, and a backend uploads them once
// per `id` and reuses the texture until they stop being drawn. `id` must be
// process-unique for these exact pixels (a new buffer, a new id).
struct SharedPixels {
    uint64_t id = 0;
    int width = 0, height = 0;
    const uint8_t* rgba = nullptr;  // width * height * 4, rows packed, straight alpha
    std::shared_ptr<const void> owner;  // keeps `rgba` alive
};

class Renderer {
public:
    virtual ~Renderer() = default;

    virtual void clear(bromath::Color color) = 0;

    virtual void drawRect(float x, float y, float w, float h, bromath::Color color) = 0;
    virtual void drawRoundRect(float x, float y, float w, float h, float rx, float ry, bromath::Color color) = 0;
    virtual void fillRect(float x, float y, float w, float h, bromath::Color color) = 0;
    virtual void fillRoundRect(float x, float y, float w, float h, float rx, float ry, bromath::Color color) = 0;

    // Per-corner asymmetric variants (CSS border-radius full support).
    // Default forwards to the symmetric path using the average corner radius
    // so backends can adopt incrementally.
    virtual void fillRoundRectRadii(float x, float y, float w, float h,
                                    const Radii& r, bromath::Color color) {
        float avg = (r.x[0] + r.x[1] + r.x[2] + r.x[3] +
                     r.y[0] + r.y[1] + r.y[2] + r.y[3]) / 8.0f;
        fillRoundRect(x, y, w, h, avg, avg, color);
    }
    virtual void drawRoundRectRadii(float x, float y, float w, float h,
                                    const Radii& r, float strokeWidth, bromath::Color color) {
        (void)strokeWidth;
        float avg = (r.x[0] + r.x[1] + r.x[2] + r.x[3] +
                     r.y[0] + r.y[1] + r.y[2] + r.y[3]) / 8.0f;
        drawRoundRect(x, y, w, h, avg, avg, color);
    }
    // Clip subsequent draw calls to a rounded rect (caller must save/restore).
    virtual void setClipRRect(float x, float y, float w, float h, const Radii& r) {
        (void)r;
        setClip(x, y, w, h);
    }
    // Box shadow with per-corner radii. Default forwards to symmetric path.
    virtual void drawBoxShadowRadii(float x, float y, float w, float h,
                                    const Radii& r,
                                    float offsetX, float offsetY,
                                    float blur, float spread,
                                    bromath::Color color, bool inset) {
        float avg = (r.x[0] + r.x[1] + r.x[2] + r.x[3] +
                     r.y[0] + r.y[1] + r.y[2] + r.y[3]) / 8.0f;
        drawBoxShadow(x, y, w, h, avg, avg, offsetX, offsetY, blur, spread, color, inset);
    }

    // `direction` is the BASE direction bidi resolves this string against —
    // not a claim about what the string contains. Passing LTR does not force
    // Latin order: a run of Arabic still resolves to level 1 and is reordered
    // (that is UAX #9 P2/P3 doing its job). What the base actually decides is
    // where neutrals at the edges land, which way multiple direction runs are
    // ordered relative to each other, and how numbers attach. Callers that
    // know a paragraph's direction — CSS `direction`, SVG `direction`, the
    // Canvas2D `direction` attribute — must say so; everything else can leave
    // it LTR, which is what it was before this parameter existed.
    virtual void drawText(std::string_view text, float x, float y, FontRef font,
                          bromath::Color color,
                          TextDirection direction = TextDirection::LTR) = 0;
    virtual TextMetrics measureText(std::string_view text, FontRef font,
                                    TextDirection direction = TextDirection::LTR) = 0;

    // Extended text draw: letter-spacing applies a per-character advance on
    // top of the natural glyph advance (matching CSS letter-spacing);
    // word-spacing widens each space character's advance (CSS word-spacing).
    // When blur > 0 the glyphs are blurred — used to render text-shadow
    // halos. Default forwards to plain drawText so backends can adopt
    // incrementally.
    virtual void drawTextEx(std::string_view text, float x, float y,
                            FontRef font, bromath::Color color,
                            float letterSpacing, float blur,
                            float wordSpacing = 0.0f,
                            TextDirection direction = TextDirection::LTR) {
        (void)letterSpacing; (void)blur; (void)wordSpacing;
        drawText(text, x, y, font, color, direction);
    }

    // A font file's bytes, shared rather than copied: every renderer that
    // registers the face (the UI, layout and raster renderers) holds `keep`,
    // which owns the memory `data` points into (typically a read-only file
    // mapping, so the pages are shared with the page cache and other processes).
    struct FontBytes {
        std::shared_ptr<const void> keep;
        const void* data = nullptr;
        size_t size = 0;
    };

    // Register a custom font from file data (for @font-face support).
    // Returns true on success. The font will be used when a FontRef matches
    // the given family name, weight, and style.
    virtual bool registerCustomFont(const std::string& family, const FontBytes& bytes,
                                    int weight, bool italic) {
        (void)family; (void)bytes; (void)weight; (void)italic;
        return false;
    }

    // Bumped whenever a custom font is registered. The same FontRef can resolve
    // to a different face across that event, so anything holding on to a
    // measurement taken from this renderer keys on the generation and throws the
    // measurement away when it moves. See layout::SkiaTextMetrics.
    uint64_t fontGeneration() const { return fontGeneration_; }

    // Shape `text` in `font` and return the cached run, or null if this
    // backend has no shaper. Exposed on the interface so RecordingRenderer can
    // shape at RECORD time through the very renderer it already delegates
    // measureText to — same thread, same cache, so a string measured during
    // layout is shaped once and the raster thread only replays glyphs.
    //
    // The result is owned by that renderer's cache and is invalidated by the
    // next shapeText() call that misses. Use it and drop it.
    virtual const ShapedRun* shapeText(std::string_view /*text*/, FontRef /*font*/,
                                       bool /*disableLigatures*/ = false,
                                       TextDirection /*direction*/ = TextDirection::LTR) {
        return nullptr;
    }

    // The shaping engine behind shapeText(), for cache instrumentation.
    virtual TextShapingEngine* textEngine() { return nullptr; }

    // Draw an already-shaped blob whose glyph positions are relative to
    // (x, y). `blur` > 0 renders a text-shadow halo. Returns false if this
    // backend can't draw blobs, in which case the caller re-draws from the
    // source string. This is the replay path for recorded text.
    virtual bool drawTextBlob(const SkTextBlob* /*blob*/, float /*x*/, float /*y*/,
                              bromath::Color /*color*/, float /*blur*/) {
        return false;
    }

    virtual void drawLine(float x1, float y1, float x2, float y2, bromath::Color color, float thickness) = 0;

    // Draw an encoded image (PNG/JPEG/etc) into the rect (x, y, w, h).
    // `imageId` is a stable, process-unique id for the source: backends that
    // cache decoded images key on it so the bytes are decoded exactly once
    // rather than on every frame. Pass 0 for one-off images that should not be
    // retained (the bytes are then decoded fresh on each call).
    virtual void drawImage(const void* data, size_t len, float x, float y, float w, float h,
                           uint64_t imageId = 0) = 0;

    // Draw a raw RGBA8 buffer. Unlike drawImage(), no codec decode happens —
    // `rgba` must be tightly-packed or use `stride` for row padding. Used by
    // video playback where decoding PNG/JPEG per frame would be absurd.
    // Default implementation is a no-op so backends can adopt incrementally.
    virtual void drawPixelsRGBA(const uint8_t* /*rgba*/,
                                int /*srcW*/, int /*srcH*/, int /*stride*/,
                                float /*x*/, float /*y*/, float /*w*/, float /*h*/) {}

    // Draw the source rect (sx, sy, sw, sh) of shared pixels into the rect
    // (x, y, w, h), filtered by `sampling`, never sampling outside a source
    // rect smaller than the image (so pieces of one image drawn side by side
    // meet without seams).
    virtual void drawSharedPixels(const SharedPixels& /*px*/, float /*sx*/, float /*sy*/, float /*sw*/,
                                  float /*sh*/, float /*x*/, float /*y*/, float /*w*/, float /*h*/,
                                  ImageSampling /*sampling*/ = ImageSampling::Smooth) {}

    // SVG drawing primitives
    virtual void drawCircle(float cx, float cy, float r,
                            bromath::Color fill, bromath::Color stroke, float strokeWidth) = 0;
    virtual void drawEllipse(float cx, float cy, float rx, float ry,
                             bromath::Color fill, bromath::Color stroke, float strokeWidth) = 0;
    virtual void drawPath(std::string_view svgPathData,
                          bromath::Color fill, bromath::Color stroke, float strokeWidth) = 0;
    virtual void drawPolygon(std::span<const PointF> points,
                             bromath::Color fill, bromath::Color stroke, float strokeWidth) = 0;
    virtual void drawPolyline(std::span<const PointF> points,
                              bromath::Color stroke, float strokeWidth) = 0;

    // Fill and/or stroke an SVG path with full paint-server + stroke styling.
    // `d` is SVG path data in the current user space (the CTM carries the
    // viewBox and element transforms). Fill and stroke may each be solid or a
    // gradient (kind == None skips that pass). `fillStops`/`strokeStops` supply
    // the gradient stops; `dash` is the stroke dash array (even length, empty =
    // solid). This is the primitive the native SVG traversal emits.
    // Default fallback draws solid colors only (loses gradients/dash/fill-rule)
    // so backends can adopt incrementally.
    virtual void drawSvgPath(std::string_view d, PathFillRule /*rule*/,
                             const GradientPaint& fill, std::span<const ColorStop> /*fillStops*/,
                             const GradientPaint& stroke, std::span<const ColorStop> /*strokeStops*/,
                             const StrokeStyle& strokeStyle, std::span<const float> /*dash*/) {
        bromath::Color f = (fill.kind == GradientPaint::Kind::Solid)
                               ? fill.color : bromath::Color{0, 0, 0, 0};
        bromath::Color s = (stroke.kind == GradientPaint::Kind::Solid)
                               ? stroke.color : bromath::Color{0, 0, 0, 0};
        drawPath(d, f, s, strokeStyle.width);
    }

    // SVG text painted like drawSvgPath: `text` shaped as drawText shapes it,
    // its glyphs filled and stroked with solid or gradient paint (so text stays
    // text: glyph caches, not outlines). Default draws a solid fill as text.
    virtual void drawSvgText(std::string_view text, float x, float y, FontRef font,
                             TextDirection direction,
                             const GradientPaint& fill, std::span<const ColorStop> /*fillStops*/,
                             const GradientPaint& /*stroke*/, std::span<const ColorStop> /*strokeStops*/,
                             const StrokeStyle& /*strokeStyle*/, std::span<const float> /*dash*/) {
        if (fill.kind == GradientPaint::Kind::Solid) drawText(text, x, y, font, fill.color, direction);
    }
    // The recorded form of drawSvgText: glyphs shaped at record time.
    virtual void drawSvgTextBlob(const SkTextBlob* /*blob*/, float /*x*/, float /*y*/,
                                 const GradientPaint& /*fill*/, std::span<const ColorStop> /*fillStops*/,
                                 const GradientPaint& /*stroke*/, std::span<const ColorStop> /*strokeStops*/,
                                 const StrokeStyle& /*strokeStyle*/, std::span<const float> /*dash*/) {}

    // Intersect the clip with an arbitrary SVG path (for SVG clipPath). Caller
    // must save/restore. Default is a no-op (no clipping) so backends can adopt
    // incrementally — used from Increment 2 onward.
    virtual void clipSvgPath(std::string_view /*d*/, PathFillRule /*rule*/) {}

    // Box shadow: draw a shadow behind a rect (or rounded rect if rx > 0)
    virtual void drawBoxShadow(float x, float y, float w, float h,
                               float rx, float ry,
                               float offsetX, float offsetY,
                               float blur, float spread,
                               bromath::Color color, bool inset) = 0;

    // Canvas state (for SVG coordinate transforms)
    virtual void save() = 0;
    virtual void restore() = 0;

    // Save a layer with the given opacity (0-255). Everything drawn until
    // the matching restore() is composited at this opacity.
    virtual void saveLayerAlpha(uint8_t alpha) = 0;
    virtual void translate(float dx, float dy) = 0;
    virtual void scale(float sx, float sy) = 0;
    virtual void rotate(float degrees) = 0;

    // Concatenate a 2D affine matrix [a b c d e f] (column-major: a,b = first col,
    // c,d = second col, e,f = translation).  Maps to CSS matrix(a,b,c,d,e,f).
    virtual void concat(float a, float b, float c, float d, float e, float f) = 0;

    // Concatenate a 4x4 matrix in column-major form (16 floats; m[0..3] = col 0,
    // m[4..7] = col 1, ...). Used for CSS 3D transforms with perspective.
    virtual void concat4x4(const float m[16]) = 0;

    // Save a layer with a CSS filter chain applied. Everything drawn until the
    // matching restore() passes through the filter. The backend constructs the
    // native filter from the descriptor list. An empty span behaves like
    // save().
    virtual void saveLayerWithFilter(std::span<const CssFilterParams> filters,
                                     float x, float y, float w, float h) = 0;

    // Save a layer composited onto the backdrop with the given CSS
    // mix-blend-mode. Everything drawn until the matching restore() is grouped
    // and blended against what's already on the canvas. The layer bounds are
    // the current clip. Default falls back to a plain save() (normal
    // source-over) so backends can adopt incrementally.
    virtual void saveLayerWithBlend(BlendMode mode) { (void)mode; save(); }

    // SVG mask: a layer whose content, on the matching restore(), becomes the
    // coverage of what is already in the enclosing layer (destination-in),
    // through `mode`. The SVG traversal draws the masked content into a
    // saveLayerAlpha(255) group, then the mask's children into this layer
    // inside that group, then restores both. Default: the mask content is
    // drawn invisibly and nothing is masked.
    virtual void saveLayerMask(MaskMode mode) { (void)mode; saveLayerAlpha(0); }

    // SVG filter: a layer passed through the filter graph `prims` (data:
    // their variable-length parameters) on the matching restore(). (x, y, w,
    // h) is the filter region in the current user space; the result is
    // clipped to it. Default: unfiltered.
    virtual void saveLayerWithSvgFilter(std::span<const SvgFilterPrimitive> prims,
                                        std::span<const float> data,
                                        float x, float y, float w, float h) {
        (void)prims; (void)data; (void)x; (void)y; (void)w; (void)h;
        save();
    }

    // CSS backdrop-filter: replace what is already drawn under the box
    // (x, y, w, h), clipped to its border radii, with that content passed
    // through the filter chain. The element's own background then paints on
    // top. The backdrop is what this surface holds, so content composited as
    // a separate layer (a canvas, WebGL, an iframe, a promoted subtree) is
    // not part of it. `opacity` is the element's: the filtered backdrop is
    // mixed over the unfiltered one by it. Default: no-op, the box shows its
    // unfiltered backdrop.
    virtual void drawBackdropFilter(std::span<const CssFilterParams> filters,
                                    float x, float y, float w, float h, const Radii& r,
                                    float opacity) {
        (void)filters; (void)x; (void)y; (void)w; (void)h; (void)r; (void)opacity;
    }

    virtual void setClip(float x, float y, float w, float h) = 0;
    virtual void resetClip() = 0;

    // Clip subsequent draw calls to an arbitrary polygon (caller must save/restore).
    // Default implementation is a no-op so backends can adopt incrementally —
    // callers should still see un-clipped output rather than a crash.
    virtual void setClipPolygon(std::span<const PointF> /*points*/) {}

    // Gradient fills (color stops must be sorted by offset 0..1)
    virtual void fillLinearGradient(float x, float y, float w, float h,
                                    float startX, float startY, float endX, float endY,
                                    std::span<const ColorStop> stops) = 0;
    virtual void fillRadialGradient(float x, float y, float w, float h,
                                    float cx, float cy, float rx, float ry,
                                    std::span<const ColorStop> stops) = 0;
    virtual void fillConicGradient(float x, float y, float w, float h,
                                   float cx, float cy, float angleDeg,
                                   std::span<const ColorStop> stops) = 0;

    virtual void beginFrame(int width, int height) = 0;
    virtual void endFrame() = 0;

    // Screenshot support — access the underlying Skia surface/canvas.
    virtual SkCanvas* getCanvas() const { return nullptr; }
    virtual SkSurface* surface() const { return nullptr; }
    /// The GPU Skia context this renderer draws with, if it draws on the GPU.
    /// Canvases created against the renderer draw on it too.
    virtual SkiaGpu* skiaGpu() const { return nullptr; }
    /// Device pixels per CSS px of the surfaces it draws (HiDPI); 1 where it
    /// has no such scale.
    virtual float deviceScale() const { return 1.0f; }
    virtual bool saveScreenshot(const std::string& path) { return false; }

    /// Capture the surface as RGBA pixels (w x h x 4). Returns empty on failure.
    virtual std::vector<uint8_t> capturePixels() { return {}; }

protected:
    // Implementations call this from registerCustomFont() on success.
    void noteFontRegistered() { ++fontGeneration_; }

private:
    uint64_t fontGeneration_ = 0;
};

} // namespace bro::render
