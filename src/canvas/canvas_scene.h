#pragma once

#include "canvas/canvas2d.h"
#include "render/renderer.h"
#include "render/frame_worker.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>

#include <include/core/SkCanvas.h>
#include <include/core/SkFont.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkShader.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTypeface.h>
#include <include/gpu/ganesh/GrDirectContext.h>

#include <glad/gl.h>

class GrDirectContext;
struct SDL_Window;
typedef struct SDL_GLContextState* SDL_GLContext;

namespace bro::render { class GLContext; }

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
    sk_sp<SkImage> img;
    SkRect src{}, dst{};
    SkSamplingOptions samp;
};

/// Per-canvas Skia-backed renderer.  Each CanvasScene owns an SkSurface.
/// Draw operations are recorded into a command buffer during JS execution and
/// replayed onto the SkCanvas during rasterize(), keeping Skia rendering cost
/// out of the JS phase.  The raster pixels are then uploaded to a GL texture
/// for compositing by the engine.
class CanvasScene {
public:
    explicit CanvasScene(render::Renderer* renderer);
    ~CanvasScene();

    CanvasScene(const CanvasScene&) = delete;
    CanvasScene& operator=(const CanvasScene&) = delete;

    // --- Layout / lifecycle callbacks (unchanged) ---

    using LayoutCallback = void(*)(void* userdata, float& outX, float& outY, float& outW, float& outH);
    void setLayoutCallback(LayoutCallback cb, void* ud) { layoutCb_ = cb; layoutUd_ = ud; }

    using DetachedCallback = bool(*)(void* userdata);
    void setDetachedCallback(DetachedCallback cb, void* ud) { detachedCb_ = cb; detachedUd_ = ud; }

    void init(render::GLContext* gl) { gl_ = gl; }

    /// Set Ganesh GPU context for GPU-accelerated canvas rendering.
    /// If set, Skia draws directly to GPU (no CPU raster + upload).
    /// Invalidates existing surface so it's recreated with the new context.
    void setGrContext(GrDirectContext* ctx) {
        if (grContext_ != ctx) {
            surface_.reset();
            grContext_ = ctx;
        }
    }

    void cleanup();

    // --- Threading (windowed GPU mode) ---

    /// Start a dedicated canvas thread using the given shared GL context.
    /// The context must be created on the main thread (via
    /// Window::createSharedContext()) — macOS requires SDL_GL_CreateContext
    /// on the main thread because the Cocoa backend calls AppKit during
    /// context creation. startThread() blocks the caller until the worker
    /// has completed SDL_GL_MakeCurrent on the given context, so subsequent
    /// SDL_GL_CreateContext calls on the main thread never overlap with a
    /// worker's wgl*Context calls (Windows/NVIDIA requirement).
    void startThread(SDL_GLContext glCtx, SDL_Window* win);

    /// Stop the canvas thread and destroy the GL context.
    void stopThread();

    /// Main thread: query layout, swap commands, signal canvas thread.
    /// No-op if not threaded or no work to do.
    void prepareAndSignal();

    /// Main thread: if canvas thread has a texture ready, consume the GL fence.
    void consumeFence();

    /// Synchronously flush all pending commands (blocks until canvas thread idle).
    /// Used by getImageData() which needs immediate surface access.
    void flushSync();

    bool isThreaded() const { return threaded_; }

    render::Renderer* renderer() const { return renderer_; }
    int width() const { return queryLayoutWidth(); }
    int height() const { return queryLayoutHeight(); }
    SkSurface* surface() const { return surface_.get(); }

    /// Set the canvas's intrinsic bitmap size (HTML canvas.width/height).
    /// When non-zero, takes precedence over layout-derived size — the surface
    /// resizes the moment the JS attribute is set, without waiting for the
    /// layout thread to publish a new content rect. Pass 0 to clear back to
    /// layout-driven sizing. Atomic so the canvas thread reads consistently.
    void setIntrinsicSize(int w, int h) {
        intrinsicW_.store(w, std::memory_order_relaxed);
        intrinsicH_.store(h, std::memory_order_relaxed);
    }
    void setIntrinsicWidth(int w)  { intrinsicW_.store(w, std::memory_order_relaxed); }
    void setIntrinsicHeight(int h) { intrinsicH_.store(h, std::memory_order_relaxed); }

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
    const std::string& fontString() const { return fontString_; }

    void setTextAlign(int align);   // 0=start/left, 1=center, 2=right, 3=end
    int textAlign() const { return textAlign_; }
    void setTextBaseline(int bl);   // 0=alphabetic, 1=top, 2=middle, 3=bottom, 4=hanging, 5=ideographic
    int textBaseline() const { return textBaseline_; }

    void setShadowBlur(float blur);
    float shadowBlur() const { return shadowBlur_; }
    void setShadowColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void getShadowColor(uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const;
    void setShadowOffsetX(float x);
    float shadowOffsetX() const { return shadowOffsetX_; }
    void setShadowOffsetY(float y);
    float shadowOffsetY() const { return shadowOffsetY_; }

    void setImageSmoothingEnabled(bool v);
    bool imageSmoothingEnabled() const { return imageSmoothingEnabled_; }

    void setLineDash(const std::vector<float>& segments);
    const std::vector<float>& lineDash() const;
    void setLineDashOffset(float off);
    float lineDashOffset() const;

    // --- Drawing methods ---

    void fillRect(float x, float y, float w, float h);
    void strokeRect(float x, float y, float w, float h);
    void clearRect(float x, float y, float w, float h);
    void fillText(const std::string& text, float x, float y);
    void strokeText(const std::string& text, float x, float y);
    render::TextMetrics measureText(const std::string& text);

    // --- Path API ---

    void beginPath();
    void moveTo(float x, float y);
    void lineTo(float x, float y);
    void polyline(const float* coords, int numPoints);  // batch [x0,y0,x1,y1,...]
    void closePath();
    void stroke();
    void fill();
    void clip();
    void arc(float cx, float cy, float radius, float startAngle, float endAngle, bool acw);
    void arcTo(float x1, float y1, float x2, float y2, float radius);
    void bezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y);
    void quadraticCurveTo(float cpx, float cpy, float x, float y);
    void ellipse(float cx, float cy, float rx, float ry, float rotation,
                 float startAngle, float endAngle, bool acw);
    void rect(float x, float y, float w, float h);
    bool isPointInPath(float x, float y);

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

    // --- Reset (discard content) ---

    void reset();

    // --- Compositing support ---

    /// Ensure the backing surface matches the layout size and upload to GL.
    /// Call once per frame before compositing.
    void rasterize(render::GLContext* gl);

    GLuint texture() const { return glTexture_; }

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

    /// Main thread: move recorded commands into the staged buffer so the raster
    /// thread can replay them via flushStaged(). Used by non-threaded scenes
    /// (system panels) to avoid racing on commands_ while JS is still running.
    /// Must only be called when the raster thread is idle.
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

private:
    /// Canvas thread entry point.
    void canvasThreadFunc(SDL_Window* win);

    /// Replay all deferred commands onto the Skia canvas.
    void flushCommands();
    /// Replay staged commands (canvas thread path).
    void flushStagedCommands();

    int queryLayoutWidth() const;
    int queryLayoutHeight() const;
    void ensureSurface(int w, int h);
    SkCanvas* skCanvas();
    SkPaint makeFillPaint() const;
    SkPaint makeStrokePaint() const;
    void applyFont();
    void applyShadow(SkPaint& paint) const;
    float adjustTextX(float x, float textWidth) const;
    float adjustTextY(float y) const;

    render::Renderer* renderer_;
    render::GLContext* gl_ = nullptr;
    GrDirectContext* grContext_ = nullptr;  // GPU Skia context (null = CPU fallback)
    GLuint gpuFBO_ = 0;                    // FBO for GPU-backed canvas surface
    LayoutCallback layoutCb_ = nullptr;
    void* layoutUd_ = nullptr;
    DetachedCallback detachedCb_ = nullptr;
    void* detachedUd_ = nullptr;
    float viewportScrollY_ = 0;
    bool detached_ = false;
    bool everAttached_ = false;

    // Skia surface (raster, RGBA premul)
    sk_sp<SkSurface> surface_;
    int surfWidth_ = 0, surfHeight_ = 0;

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
    // canvas thread reads these from queryLayoutWidth/Height while the
    // main thread writes them from JS attribute setters.
    std::atomic<int> intrinsicW_{0};
    std::atomic<int> intrinsicH_{0};

    // GL texture for compositing
    GLuint glTexture_ = 0;
    int texWidth_ = 0, texHeight_ = 0;  // current GL texture dimensions
    bool dirty_ = false;  // surface pixels changed, need GL re-upload

    // Screen-space position for compositing
    float screenX_ = 0, screenY_ = 0;

    // --- Canvas 2D state ---

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
        float shadowBlurVal = 0;
        uint8_t shadowR = 0, shadowG = 0, shadowB = 0, shadowA = 0;
        float shadowOX = 0, shadowOY = 0;
        bool imgSmooth = true;
        std::vector<float> lineDash;
        float lineDashOffset = 0;
    };

    State state_;
    std::vector<State> stateStack_;

    // Deferred command buffer — recorded during JS, replayed during rasterize()
    std::vector<CanvasCmd> commands_;

    // Current path (built incrementally, snapshot()'d for drawing)
    SkPathBuilder pathBuilder_;

    // Current font
    SkFont font_;
    std::string fontString_ = "16px sans-serif";
    int textAlign_ = 0;
    int textBaseline_ = 0;
    float shadowBlur_ = 0;
    uint8_t shadowR_ = 0, shadowG_ = 0, shadowB_ = 0, shadowA_ = 0;
    float shadowOffsetX_ = 0, shadowOffsetY_ = 0;
    bool imageSmoothingEnabled_ = true;

    // Font cache (CSS string -> SkFont)
    struct FontCacheEntry {
        sk_sp<SkTypeface> typeface;
        SkFont font;
    };
    std::unordered_map<std::string, FontCacheEntry> fontCache_;

    // --- Canvas thread state ---
    bool threaded_ = false;
    render::FrameWorker worker_;
    // Snapshot fields the main thread publishes before signaling the worker.
    // The FrameWorker state release/acquire pair orders these with respect
    // to the worker reading them.
    std::atomic<int> sharedCanvasWidth_{0};
    std::atomic<int> sharedCanvasHeight_{0};
    // Main thread sets before signaling; worker reads under acquire and
    // refreshes snapshot_/snapshotImage_ on the worker thread (where the
    // surface's GrContext lives). Cleared by the worker once the snapshot
    // has been written.
    std::atomic<bool> snapshotRequested_{false};
    std::atomic<bool> canvasReady_{false};
    std::thread canvasThread_;
    SDL_GLContext canvasGLContext_ = nullptr;
    sk_sp<GrDirectContext> threadGrContext_;
    std::vector<CanvasCmd> stagedCommands_;
};

} // namespace bro::canvas
