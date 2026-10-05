#pragma once

#include "canvas/canvas2d.h"
#include "render/font_fallback.h"
#include "render/renderer.h"
#include "render/shaped_run.h"
#include "render/skia_gpu.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include <include/core/SkCanvas.h>
#include <include/core/SkFont.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageFilter.h>
#include <include/core/SkM44.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkShader.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTileMode.h>
#include <include/core/SkTypeface.h>

namespace bro::canvas {


/// Deferred canvas command — recorded during JS, replayed during rasterize().
struct CanvasCmd {
    enum Type : uint8_t {
        kFillRect, kStrokeRect, kClearRect,
        kStrokePath, kFillPath, kClipPath,
        kFillText, kStrokeText,
        kDrawImage, kPutImageData,
        kSave, kRestore,
        kTranslate, kRotate, kScale,
        kSetTransform, kResetTransform, kConcatTransform,
        kReset
    };
    Type type;
    SkPaint paint;
    SkPath path;
    float p[6] = {};
    std::string text;
    SkFont font;
    // Text commands carry an already-shaped blob. Shaping happens on the JS
    // thread when the command is recorded, against the scene's own shaper, so
    // the canvas worker replays glyphs and never touches a shaping cache that
    // belongs to another thread. `text` and `font` are kept as the fallback
    // for the case where shaping produced nothing.
    sk_sp<SkTextBlob> blob;
    sk_sp<SkImage> img;
    SkRect src{}, dst{};
    SkSamplingOptions samp;
    // ctx.filter, resolved to an immutable SkImageFilter on the JS thread when
    // the draw was recorded. Non-null means the replay draws this command into
    // a layer that the filter is applied to on restore.
    sk_sp<SkImageFilter> filter;
    // The canvas shadow, as a shadow-only image filter (offset, blur, colour,
    // and `filter` as its input, since the spec shadows the filtered image).
    // Non-null means the replay first draws the command into a layer this
    // filter turns into just the shadow, composited on its own, and then
    // draws the command itself.
    sk_sp<SkImageFilter> shadow;
    // Set when `filter` or `shadow` is: globalAlpha and the composite
    // operation then ride here rather than on `paint`, because the spec
    // applies them after the filter, and separately to the shadow and to the
    // shape.
    float layerAlpha = 1.0f;
    SkBlendMode layerBlend = SkBlendMode::kSrcOver;
};

/// The state behind a CanvasPattern: an immutable image snapshot, the
/// repetition as a pair of tile modes (kDecal for a non-repeating axis), and
/// the pattern transform. Shared between the JS CanvasPattern object and every
/// canvas state whose fill/stroke style names it, because setTransform() after
/// assignment must reach the next draw. Only ever read and written on the JS
/// thread: the shader is built when a draw is recorded, and replay sees
/// only that shader.
struct CanvasPatternData {
    sk_sp<SkImage> image;
    SkTileMode tileX = SkTileMode::kRepeat;
    SkTileMode tileY = SkTileMode::kRepeat;
    SkMatrix transform;  // identity
};

/// Canvas2D TextMetrics. Every distance is measured from the *alignment point*
/// — the (x, y) that would be passed to fillText — which is why textAlign and
/// textBaseline change these numbers even though they do not change the glyphs.
/// Positive means right of, or above, that point.
struct CanvasTextMetrics {
    float width = 0.0f;
    // Ink extents, horizontally. `left` is positive when ink reaches left of
    // the alignment point (an italic overhang, or any centred/right-aligned
    // text).
    float actualLeft = 0.0f;
    float actualRight = 0.0f;
    // Ink extents, vertically — the tight bounding box of the glyphs actually
    // drawn, so "abc" and "ABC" report different ascents.
    float actualAscent = 0.0f;
    float actualDescent = 0.0f;
    // The font's own line box, independent of which glyphs were asked for.
    float fontAscent = 0.0f;
    float fontDescent = 0.0f;
    // The em square.
    float emAscent = 0.0f;
    float emDescent = 0.0f;
    // Where the named baselines sit relative to the alignment point.
    float hangingBaseline = 0.0f;
    float alphabeticBaseline = 0.0f;
    float ideographicBaseline = 0.0f;
};

/// Per-canvas Skia-backed renderer.  Each CanvasScene owns an SkSurface:
/// a GPU one on its renderer's SkiaGpu when it has one, else CPU raster.
/// Draw operations are recorded into a command buffer during JS execution and
/// replayed onto the SkCanvas during rasterize(), keeping Skia rendering cost
/// out of the JS phase.  The engine composites the surface — its GPU image in
/// place, or its pixels. Every method that reaches a GPU surface takes the
/// SkiaGpu lock itself.
class CanvasScene {
public:
    explicit CanvasScene(render::Renderer* renderer);
    ~CanvasScene();

    CanvasScene(const CanvasScene&) = delete;
    CanvasScene& operator=(const CanvasScene&) = delete;

    /// Process-unique, never-recycled id. Composited UILayers and recorded
    /// Cmd_LayerBreak commands name this scene by id and resolve it through
    /// the engine's registry at use time, so a layer recorded before the
    /// scene was destroyed resolves to null instead of dangling — no
    /// pointer-scrubbing pass over the layer buffers required.
    uint64_t sceneId() const { return sceneId_; }

    // --- Layout / lifecycle callbacks (unchanged) ---

    using LayoutCallback = void(*)(void* userdata, float& outX, float& outY, float& outW, float& outH);
    void setLayoutCallback(LayoutCallback cb, void* ud) { layoutCb_ = cb; layoutUd_ = ud; }

    using DetachedCallback = bool(*)(void* userdata);
    void setDetachedCallback(DetachedCallback cb, void* ud) { detachedCb_ = cb; detachedUd_ = ud; }

    // Liveness predicate for the backing Element. The detached/layout
    // callbacks above hold a raw dom::Element* as their userdata; that Element
    // can be freed (deferred-free / pointer reuse) while this scene survives.
    // This predicate answers "is the Element pointer still alive?" via a
    // pointer-value lookup that never dereferences it (Document::isNodeLive),
    // so the per-frame callbacks can be gated on it instead of trusting the
    // raw pointer. liveUd_ is the owning Document.
    using LiveCheckCallback = bool(*)(void* doc, void* node);
    void setLiveCheck(LiveCheckCallback cb, void* doc) { liveCb_ = cb; liveUd_ = doc; }

    void init() {}

    void cleanup();

    render::Renderer* renderer() const { return renderer_; }
    int width() const { return queryLayoutWidth(); }
    int height() const { return queryLayoutHeight(); }
    SkSurface* surface() const { return surface_.surface.get(); }
    /// The GPU surface's image, ready to sample after rasterize() (null for
    /// a CPU surface).
    render::SkiaImageRef gpuImage() const { return surface_.image; }

    /// Set the canvas's intrinsic bitmap size (HTML canvas.width/height).
    /// When non-zero, takes precedence over layout-derived size — the surface
    /// resizes the moment the JS attribute is set, without waiting for the
    /// layout thread to publish a new content rect. Pass 0 to clear back to
    /// layout-driven sizing. Atomic so the raster thread reads consistently.
    void setIntrinsicSize(int w, int h) {
        intrinsicW_.store(w, std::memory_order_relaxed);
        intrinsicH_.store(h, std::memory_order_relaxed);
    }
    void setIntrinsicWidth(int w)  { intrinsicW_.store(w, std::memory_order_relaxed); }
    void setIntrinsicHeight(int h) { intrinsicH_.store(h, std::memory_order_relaxed); }
    void ensureSurface(int w, int h);

    void setViewportScroll(float scrollY) { viewportScrollY_ = scrollY; }
    bool isDetached() const { return detached_; }

    // --- Canvas 2D state setters (called from JS bindings) ---

    void setFillColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void getFillColor(uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const;

    void setStrokeColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void getStrokeColor(uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const;

    // Gradient / pattern shaders. Pass nullptr to clear back to solid color.
    // Setting a solid color via setFillColor/setStrokeColor also clears
    // any shader (matches the Canvas 2D spec — fillStyle = "red" replaces
    // the previous gradient).
    void setFillShader(sk_sp<SkShader> shader);
    void setStrokeShader(sk_sp<SkShader> shader);

    // CanvasPattern styles. The shader is built per draw from the pattern's
    // current transform and the current image-smoothing state, so neither is
    // frozen at assignment time. Setting a color or a gradient clears it.
    void setFillPattern(std::shared_ptr<CanvasPatternData> pattern);
    void setStrokePattern(std::shared_ptr<CanvasPatternData> pattern);
    bool hasFillShader() const   { return static_cast<bool>(state_.fillPaint.getShader()); }
    bool hasStrokeShader() const { return static_cast<bool>(state_.strokePaint.getShader()); }

    void setLineWidth(float w);
    float lineWidth() const;

    void setGlobalAlpha(float a);
    float globalAlpha() const;

    void setLineCap(int cap);     // 0=butt, 1=round, 2=square
    int lineCap() const;
    void setLineJoin(int join);   // 0=miter, 1=round, 2=bevel
    int lineJoin() const;
    void setMiterLimit(float limit);
    float miterLimit() const;

    void setGlobalCompositeOperation(int op);
    int globalCompositeOperation() const;

    void setFont(const std::string& fontStr);
    const std::string& fontString() const { return state_.fontStr; }

    // 0=start, 1=center, 2=right, 3=end, 4=left. `start`/`end` are
    // direction-relative and resolve against direction(); `left`/`right` are
    // absolute. They are distinct codes because the getter has to round-trip
    // what was assigned.
    void setTextAlign(int align);
    int textAlign() const { return state_.textAlignVal; }
    void setTextBaseline(int bl);   // 0=alphabetic, 1=top, 2=middle, 3=bottom, 4=hanging, 5=ideographic
    int textBaseline() const { return state_.textBaselineVal; }

    // Canvas2D `direction`: 0=ltr, 1=rtl, 2=inherit. This is the base
    // direction text is shaped against and what `start`/`end` alignment mean.
    void setDirection(int dir);
    int direction() const { return state_.directionVal; }

    // Shadows. Negative or non-finite blur and non-finite offsets are
    // ignored, as the spec says for these attributes. A shadow is drawn only
    // while the colour is not fully transparent and the blur or an offset is
    // non-zero; the offsets are canvas pixels, unaffected by the transform,
    // and the blur is a Gaussian of sigma shadowBlur / 2.
    void setShadowBlur(float blur);
    float shadowBlur() const { return state_.shadowBlurVal; }
    void setShadowColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void getShadowColor(uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const;
    void setShadowOffsetX(float x);
    float shadowOffsetX() const { return state_.shadowOX; }
    void setShadowOffsetY(float y);
    float shadowOffsetY() const { return state_.shadowOY; }

    void setImageSmoothingEnabled(bool v);
    bool imageSmoothingEnabled() const { return state_.imgSmooth; }
    // 0=low, 1=medium, 2=high.
    void setImageSmoothingQuality(int q);
    int imageSmoothingQuality() const { return state_.smoothQuality; }

    // ctx.filter: a CSS <filter-value-list> or "none". Answers false and keeps
    // the current filter when the string does not parse, which is what the
    // spec says an invalid assignment does. `currentColor` is what a
    // drop-shadow() with no colour (or `currentcolor`) paints in, fixed at the
    // time of the assignment. `lengths` carries the root font size and the
    // viewport; the em size is this context's own font, also as of now.
    bool setFilter(const std::string& filter, FilterColor currentColor = {},
                   FilterLengthContext lengths = {});
    const std::string& filterString() const { return state_.filterStr; }

    void setLineDash(const std::vector<float>& segments);
    const std::vector<float>& lineDash() const;
    void setLineDashOffset(float off);
    float lineDashOffset() const;

    // --- Drawing methods ---

    void fillRect(float x, float y, float w, float h);
    void strokeRect(float x, float y, float w, float h);
    void clearRect(float x, float y, float w, float h);
    void fillText(const std::string& text, float x, float y, float maxWidth = -1.0f);
    void strokeText(const std::string& text, float x, float y, float maxWidth = -1.0f);
    CanvasTextMetrics measureText(const std::string& text);

    // --- Path API ---

    void beginPath();
    void moveTo(float x, float y);
    void lineTo(float x, float y);
    void polyline(const float* coords, int numPoints);  // batch [x0,y0,x1,y1,...]
    void closePath();
    void stroke();
    void stroke(const SkPath& path);
    void fill(const std::string& fillRule = "nonzero");
    void fill(const SkPath& path, const std::string& fillRule = "nonzero");
    void clip(const std::string& fillRule = "nonzero");
    void clip(const SkPath& path, const std::string& fillRule = "nonzero");
    void arc(float cx, float cy, float radius, float startAngle, float endAngle, bool acw);
    void arcTo(float x1, float y1, float x2, float y2, float radius);
    void bezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y);
    void quadraticCurveTo(float cpx, float cpy, float x, float y);
    void ellipse(float cx, float cy, float rx, float ry, float rotation,
                 float startAngle, float endAngle, bool acw);
    void rect(float x, float y, float w, float h);
    void roundRect(float x, float y, float w, float h, const SkVector radii[4]);
    bool isPointInPath(float x, float y, const std::string& fillRule = "nonzero");
    bool isPointInPath(const SkPath& path, float x, float y, const std::string& fillRule = "nonzero");

    // --- Transform ---

    void save();
    void restore();
    void translate(float tx, float ty);
    void rotate(float angle);
    void scale(float sx, float sy);
    void setTransform(float a, float b, float c, float d, float e, float f);
    void resetTransform();
    void transform(float a, float b, float c, float d, float e, float f);

    // --- Image ---

    void drawImage(const void* rgbaData, int imgW, int imgH,
                   float sx, float sy, float sw, float sh,
                   float dx, float dy, float dw, float dh);

    /// Overload for the canvas-as-CanvasImageSource fast path. Avoids the
    /// per-call rgba copy + SkImage allocation that the rgbaData overload
    /// pays — instead the caller provides an already-built SkImage (typically
    /// one shared across many blits, e.g. the snapshot of a sprite atlas).
    void drawImage(sk_sp<SkImage> img,
                   float sx, float sy, float sw, float sh,
                   float dx, float dy, float dw, float dh);

    // --- Pixel manipulation ---

    std::vector<uint8_t> getImageData(int x, int y, int w, int h);
    void putImageData(const uint8_t* data, int w, int h, int dx, int dy);
    void putImageData(const uint8_t* data, int w, int h, int dx, int dy,
                      int dirtyX, int dirtyY, int dirtyWidth, int dirtyHeight);

    /// Cached pixel snapshot of the surface's (0,0,w,h) region, suitable for
    /// drawImage(<canvas>) sources. Returns a pointer into a buffer owned by
    /// this scene; the pointer is valid until the next mutation (any new draw
    /// command, reset, or surface resize) or scene destruction. Returns
    /// nullptr only if `surface_` cannot be created (e.g. zero-sized canvas).
    ///
    /// Re-reads from the surface only when the cache is stale. Lets a scene
    /// be used as a sprite atlas without paying a GPU readback per blit.
    const uint8_t* snapshotPixels(int w, int h);

    /// Cached SkImage snapshot of the surface — the canvas-source fast path
    /// for drawImage(<canvas>). Stays alive (and Ganesh sees a single texture
    /// across many blits) until any new draw command lands, the surface
    /// resizes, or reset() runs. Returns null when there's no surface yet.
    sk_sp<SkImage> snapshotImage();

    // --- Reset ---

    /// ctx.reset(), and what setting canvas.width/height does: clear the
    /// bitmap to transparent black, empty the current path and the state
    /// stack, and put the transform, clip, line dash and every attribute back
    /// to its default.
    void reset();

    /// Called at the end of every reset(). The JS binding keeps a little
    /// drawing state of its own (the transform it answers getTransform()
    /// from, a gradient or pattern style object), and this is how a reset
    /// that did not come through ctx.reset() — a canvas.width assignment —
    /// reaches it. JS thread only.
    void setResetHook(std::function<void()> hook) { resetHook_ = std::move(hook); }

    // --- Compositing support ---

    /// Bring the backing surface to the layout size and replay the frame's
    /// commands onto it. Call once per frame before compositing.
    void rasterize();

    void getScreenRect(float& x, float& y, float& w, float& h) const {
        x = screenX_; y = screenY_;
        w = static_cast<float>(surfWidth_);
        h = static_cast<float>(surfHeight_);
    }

    /// Flush any pending deferred commands onto the backing SkSurface.
    /// Used by the main-thread system-panel layer-break callback (headless),
    /// which snapshots this surface and blits it straight onto the enclosing
    /// panel's target Skia canvas. Safe no-op if there are no pending commands.
    void flush() { flushCommands(); }

    /// Main thread: move recorded commands into the staged buffer so the
    /// engine's raster thread can replay them via flushStaged() (system panels,
    /// iframes) without racing on commands_ while JS is still running. Must
    /// only be called when the raster thread is idle.
    void stageCommandsForRaster();

    /// Raster thread: replay staged commands onto the backing SkSurface.
    /// Counterpart to stageCommandsForRaster(). Safe no-op if nothing staged.
    void flushStaged() { flushStagedCommands(); }

    /// Mark the canvas as dirty (needing re-rasterization).
    void markDirty() { dirty_ = true; }
    bool isDirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }
    void checkDetached() {
        if (!detachedCb_) return;
        // The Element backing detachedUd_ may have been freed out from under us
        // (deferred-free / pointer reuse). Finalize instead of dereferencing a
        // dead pointer.
        if (!backingElementAlive()) { onElementFinalized(); return; }
        bool orphaned = detachedCb_(detachedUd_);
        // An offscreen canvas (document.createElement('canvas') with no
        // appendChild) reads as orphaned from frame one, but the JS side is
        // still using it as a sprite atlas / readback target. Only mark for
        // cleanup once we've actually seen it attached to the document — that
        // distinguishes "removed from the DOM" from "intentionally offscreen".
        if (!orphaned) everAttached_ = true;
        if (orphaned && everAttached_) detached_ = true;
    }

    /// Called from the JS HTMLCanvasElement finalizer when its wrapper is
    /// GC'd. The element it pointed at is about to be freed, so we drop the
    /// callback userdata that aimed at it and mark the scene detached — the
    /// engine's next per-frame cleanup pass collects it.
    void onElementFinalized() {
        detached_ = true;
        detachedCb_ = nullptr;
        detachedUd_ = nullptr;
        layoutCb_ = nullptr;
        layoutUd_ = nullptr;
    }

    /// Trampoline suitable as dom::Element's canvas-scene on-destroy hook
    /// (Element holds the scene as an opaque void*). Invoked from ~Element.
    static void onBackingElementDestroyed(void* scene) {
        if (scene) static_cast<CanvasScene*>(scene)->onElementFinalized();
    }

    /// The Element this scene's callbacks point at (the detached/layout
    /// userdata), or null once the Element has been finalized. The engine uses
    /// this to clear the Element's back-pointer before reclaiming the scene, so
    /// a later ~Element never calls onElementFinalized() on freed memory.
    void* backingElement() const { return detachedUd_; }

    /// True if the backing Element pointer is safe to dereference — it is still
    /// a live node owned-or-pending in its Document. Returns false once the
    /// Element has been finalized (null userdata) or freed (not live). The
    /// liveness check never dereferences the pointer, so this is safe to call
    /// even if the Element was already destroyed. With no predicate set
    /// (headless tests that never wire one), falls back to the null check.
    bool backingElementAlive() const {
        if (!detachedUd_) return false;
        if (liveCb_ && !liveCb_(liveUd_, detachedUd_)) return false;
        return true;
    }

private:
    /// Replay all deferred commands onto the Skia canvas.
    void flushCommands();
    /// Replay staged commands (raster thread path).
    void flushStagedCommands();

    int queryLayoutWidth() const;
    int queryLayoutHeight() const;
    SkCanvas* skCanvas();
    SkPaint makeFillPaint() const;
    SkPaint makeStrokePaint() const;
    // The paint for a drawImage: globalAlpha and the composite op, unless a
    // filter or a shadow moves both onto the replay layer.
    SkPaint makeImagePaint() const;
    // True while a draw has to be replayed through layers: a filter is set,
    // or a shadow would be drawn.
    bool drawIsLayered() const;
    bool shadowActive() const;
    // The shadow-only image filter for the current shadow state, or null.
    sk_sp<SkImageFilter> shadowFilter() const;
    // The alpha multiplier and blend mode a draw's own paint carries — 1 and
    // source-over while the draw is layered, since the layers apply them.
    float drawAlpha() const;
    SkBlendMode drawBlend() const;
    // imageSmoothingEnabled + imageSmoothingQuality as Skia sampling.
    SkSamplingOptions imageSampling() const;
    void applyPattern(SkPaint& p, const std::shared_ptr<CanvasPatternData>& pat) const;
    // Every drawing command funnels through here: stamps the current filter
    // onto the command and marks the canvas dirty.
    void recordDraw(CanvasCmd&& cmd);
    // One replay loop for both the inline and the worker path.
    static void replayCommands(SkCanvas* c, std::vector<CanvasCmd>& cmds);
    static void replayOne(SkCanvas* c, CanvasCmd& cmd);
    // Replay `cmd` into a layer `filter` is applied to, opened under the
    // identity matrix so the filter's lengths are canvas pixels.
    static void replayThroughFilter(SkCanvas* c, CanvasCmd& cmd, const SkM44& ctm,
                                    const sk_sp<SkImageFilter>& filter);
    void applyFont();
    float adjustTextX(float x, float textWidth) const;
    float adjustTextY(float y) const;

    static uint64_t nextSceneId() {
        static std::atomic<uint64_t> counter{0};
        return ++counter;
    }
    uint64_t sceneId_ = nextSceneId();

    render::Renderer* renderer_;
    LayoutCallback layoutCb_ = nullptr;
    void* layoutUd_ = nullptr;
    DetachedCallback detachedCb_ = nullptr;
    void* detachedUd_ = nullptr;
    LiveCheckCallback liveCb_ = nullptr;
    void* liveUd_ = nullptr;
    float viewportScrollY_ = 0;
    bool detached_ = false;
    bool everAttached_ = false;

    // Skia surface (RGBA premul): GPU on the renderer's SkiaGpu, else raster.
    // On the GPU, unfinished_ marks a surface drawn (or read) since the last
    // finish, which rasterize() leaves ready to sample again.
    render::LayerSurface surface_;
    int surfWidth_ = 0, surfHeight_ = 0;
    bool unfinished_ = false;
    render::SkiaGpu* gpu() const { return renderer_ ? renderer_->skiaGpu() : nullptr; }
    render::SkiaGpu::Lock lockGpu() const {
        render::SkiaGpu* g = gpu();
        return g ? g->lock() : render::SkiaGpu::Lock();
    }

    // Snapshot cache for drawImage(<canvas>) sources — see snapshotPixels()
    // and snapshotImage(). Invalidated whenever a new draw command lands,
    // reset() runs, or the surface resizes. Cleared lazily on next call.
    std::vector<uint8_t> snapshot_;
    int snapshotW_ = 0, snapshotH_ = 0;
    bool snapshotValid_ = false;
    sk_sp<SkImage> snapshotImage_;
    bool snapshotImageValid_ = false;

    // Intrinsic bitmap size set via canvas.width / canvas.height attribute.
    // When non-zero, overrides layout-derived sizing. Atomic because the
    // raster thread reads these from queryLayoutWidth/Height while the
    // main thread writes them from JS attribute setters.
    std::atomic<int> intrinsicW_{0};
    std::atomic<int> intrinsicH_{0};

    bool dirty_ = false;  // surface pixels changed since the compositor last took them

    // Screen-space position for compositing
    float screenX_ = 0, screenY_ = 0;

    // --- Canvas 2D state ---

    // Everything save()/restore() carries and reset() puts back. The clip and
    // the transform are the exception: they live in the recorded command
    // stream, as kSave/kRestore/kClipPath and the transform commands the
    // SkCanvas replays.
    struct State {
        SkPaint fillPaint;
        SkPaint strokePaint;
        float lineWidthVal = 1.0f;
        float globalAlphaVal = 1.0f;
        int lineCapVal = 0;    // SkPaint::kButt_Cap
        int lineJoinVal = 0;   // SkPaint::kMiter_Join
        float miterLimitVal = 10.0f;
        int compositeOp = 0;   // source-over
        std::string fontStr = "16px sans-serif";
        int textAlignVal = 0;
        int textBaselineVal = 0;
        int directionVal = 0;
        float shadowBlurVal = 0;
        uint8_t shadowR = 0, shadowG = 0, shadowB = 0, shadowA = 0;
        float shadowOX = 0, shadowOY = 0;
        bool imgSmooth = true;
        int smoothQuality = 0;  // low
        std::vector<float> lineDash;
        float lineDashOffset = 0;
        std::shared_ptr<CanvasPatternData> fillPattern;
        std::shared_ptr<CanvasPatternData> strokePattern;
        std::string filterStr = "none";
        sk_sp<SkImageFilter> filter;
    };

    static State defaultState();

    State state_ = defaultState();
    std::vector<State> stateStack_;
    std::function<void()> resetHook_;

    // Deferred command buffer — recorded during JS, replayed during rasterize()
    std::vector<CanvasCmd> commands_;

    // Current path (built incrementally, snapshot()'d for drawing)
    SkPathBuilder pathBuilder_;

    // Current font, resolved from state_.fontStr by applyFont().
    SkFont font_;

    // Font cache (CSS string -> SkFont). The parsed family and style ride
    // along because the shaper needs what was *asked for*, not just what was
    // resolved: font fallback for a codepoint the chosen face lacks is driven
    // by the requested family list and style.
    struct FontCacheEntry {
        sk_sp<SkTypeface> typeface;
        SkFont font;
        std::string family;
        SkFontStyle style;
    };
    std::unordered_map<std::string, FontCacheEntry> fontCache_;
    std::string fontFamily_ = "sans-serif";
    SkFontStyle fontStyle_;

    // Shaping state, owned by this scene and touched only from the JS thread
    // that records commands — the same thread-affine-by-ownership rule the
    // renderers follow for their own shapers. The canvas worker only ever
    // replays an SkTextBlob, which is immutable and refcounted.
    sk_sp<SkFontMgr> fontMgr_;
    render::FontFallbackCache fallbackCache_;
    render::TextShapingEngine shaper_;
    SkFontMgr* ensureFontMgr();
    // Shape `text` with the current font against the current direction, or
    // null when there is nothing to draw. The pointer belongs to shaper_ and
    // dies at the next shape() that misses — use it and drop it.
    const render::ShapedRun* shapeCurrent(std::string_view text);
    render::TextDirection baseDirection() const;
    // Normalized OS/2 typographic ascent/descent — the em box. Returns false
    // when it had to fall back to the hhea ratio.
    bool typoMetrics(float& ascent, float& descent) const;
    void recordText(bool stroke, const std::string& text, float x, float y, float maxWidth = -1.0f);

    // Commands handed to the engine's raster thread (stageCommandsForRaster).
    std::vector<CanvasCmd> stagedCommands_;
};

} // namespace bro::canvas
