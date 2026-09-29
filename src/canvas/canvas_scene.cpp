// CanvasScene: construction, the backing surface, and the recording half of
// the Canvas 2D API (rects, paths, transforms, images, pixel access, reset).
// Drawing state, paints and text are in canvas_scene_state.cpp; the worker
// thread, replay and compositing upload are in canvas_scene_raster.cpp.

#include "canvas/canvas_scene.h"

#include <include/core/SkColorSpace.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkBlendMode.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkRRect.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/GrBackendSurface.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <include/gpu/ganesh/gl/GrGLBackendSurface.h>

#include <glad/gl.h>

#include <cmath>
#include <cstring>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace bro::canvas {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CanvasScene::State CanvasScene::defaultState() {
    // Black fill and stroke, the one piece of the default state the member
    // initializers cannot spell. Everything else is State's own defaults.
    State s;
    s.fillPaint.setAntiAlias(true);
    s.fillPaint.setStyle(SkPaint::kFill_Style);
    s.fillPaint.setColor(SK_ColorBLACK);

    s.strokePaint.setAntiAlias(true);
    s.strokePaint.setStyle(SkPaint::kStroke_Style);
    s.strokePaint.setColor(SK_ColorBLACK);
    s.strokePaint.setStrokeWidth(1.0f);
    return s;
}

CanvasScene::CanvasScene(render::Renderer* renderer)
    : renderer_(renderer)
{
    applyFont();
}

CanvasScene::~CanvasScene() {
    // Threaded scenes have their GPU resources freed on the shared worker
    // (CanvasRasterThread::releaseScene) before the engine destroys them — we
    // must not touch GL from here (wrong thread/context). Non-threaded scenes
    // own their surface on this thread, so clean up directly.
    if (!threaded_) cleanup();
}

void CanvasScene::cleanup() {
    surface_.reset();
    surfWidth_ = surfHeight_ = 0;
    if (gpuFBO_) {
        glDeleteFramebuffers(1, &gpuFBO_);
        gpuFBO_ = 0;
    }
    if (glTexture_) {
        glDeleteTextures(1, &glTexture_);
        glTexture_ = 0;
    }
    texWidth_ = texHeight_ = 0;
    fontCache_.clear();
    // The shaped-run cache keys on the font descriptor, and the faces those
    // descriptors resolve to are exactly what just went away.
    shaper_.clear();
}

// ---------------------------------------------------------------------------
// Surface management
// ---------------------------------------------------------------------------

int CanvasScene::queryLayoutWidth() const {
    // Intrinsic bitmap size (canvas.width attribute) wins — the layout
    // thread can lag a frame or two behind JS attribute mutation, and
    // sourcing surface size from it would race draw commands recorded at
    // the new size against a still-old surface.
    int iw = intrinsicW_.load(std::memory_order_relaxed);
    if (iw > 0) return iw;
    if (layoutCb_) {
        float ox, oy, ow = 0, oh = 0;
        layoutCb_(layoutUd_, ox, oy, ow, oh);
        if (ow > 0) return static_cast<int>(ow);
    }
    return surfWidth_ > 0 ? surfWidth_ : 300;
}

int CanvasScene::queryLayoutHeight() const {
    int ih = intrinsicH_.load(std::memory_order_relaxed);
    if (ih > 0) return ih;
    if (layoutCb_) {
        float ox, oy, ow = 0, oh = 0;
        layoutCb_(layoutUd_, ox, oy, ow, oh);
        if (oh > 0) return static_cast<int>(oh);
    }
    return surfHeight_ > 0 ? surfHeight_ : 150;
}

void CanvasScene::ensureSurface(int w, int h) {
    if (surface_ && surfWidth_ == w && surfHeight_ == h) return;

    bool isResize = (surface_ != nullptr);
    snapshotValid_ = false;
    snapshotImageValid_ = false;
    snapshotImage_.reset();

    if (grContext_) {
        // Resizing: drop the old SkSurface, drain Ganesh, and recreate the
        // texture/FBO from scratch instead of reusing the names. Two reasons:
        //   1) Skia's Ganesh wraps the FBO by ID and caches GL state for it;
        //      reallocating the texture's storage in place (glTexImage2D on
        //      the same name) leaves Skia's cached state pointing at a now-
        //      orphaned wrapping. The next draw against the new wrapping
        //      replays a stale clear.
        //   2) On macOS Metal-backed OpenGL, the FBO attachment caches an
        //      internal GLDTextureRec*; that pointer goes stale when the
        //      attached texture is reallocated underneath, and the next
        //      glClear hits a NULL deref inside gleUpdateDrawFramebufferState.
        // The original Windows symptom (canvas keeps drawing the previous
        // asset after a resize) and the macOS release-build crash both come
        // from this same surface-reuse race.
        if (isResize) {
            surface_.reset();
            grContext_->flushAndSubmit();
            if (gpuFBO_)    { glDeleteFramebuffers(1, &gpuFBO_); gpuFBO_ = 0; }
            if (glTexture_) { glDeleteTextures(1, &glTexture_);  glTexture_ = 0; }
        }
    }

    surfWidth_ = w;
    surfHeight_ = h;

    if (grContext_) {
        // GPU path: create FBO + texture, wrap with Skia Ganesh
        if (!glTexture_) {
            glGenTextures(1, &glTexture_);
        }
        glBindTexture(GL_TEXTURE_2D, glTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        texWidth_ = w;
        texHeight_ = h;

        if (!gpuFBO_) {
            glGenFramebuffers(1, &gpuFBO_);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, gpuFBO_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, glTexture_, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Tell Skia to discard its cached view of GL state — the FBO/texture
        // names may have been recycled to fresh objects above, and Ganesh's
        // bind cache from the prior wrapping must not be applied to the new
        // one.
        grContext_->resetContext();

        GrGLFramebufferInfo fbInfo;
        fbInfo.fFBOID = gpuFBO_;
        fbInfo.fFormat = GL_RGBA8;
        auto backendRT = GrBackendRenderTargets::MakeGL(w, h, 0, 0, fbInfo);
        surface_ = SkSurfaces::WrapBackendRenderTarget(
            grContext_, backendRT,
            kTopLeft_GrSurfaceOrigin,
            kRGBA_8888_SkColorType,
            SkColorSpace::MakeSRGB(), nullptr);
    } else {
        // CPU fallback (headless --no-gpu)
        auto info = SkImageInfo::MakeN32Premul(w, h);
        surface_ = SkSurfaces::Raster(info);
    }

    // Clear to transparent (canvas default)
    if (surface_) {
        // The drawing state's clip and transform live one save level above
        // the SkCanvas's base, so that kReset can drop a clip() made outside
        // any save() by restoring to the base and opening the level again —
        // SkCanvas has no other way to take a clip back.
        surface_->getCanvas()->save();
        surface_->getCanvas()->clear(SK_ColorTRANSPARENT);
        dirty_ = true;
    snapshotValid_ = false;
    snapshotImageValid_ = false;
    }
}

SkCanvas* CanvasScene::skCanvas() {
    int w = queryLayoutWidth();
    int h = queryLayoutHeight();
    ensureSurface(w, h);
    return surface_ ? surface_->getCanvas() : nullptr;
}

// ---------------------------------------------------------------------------
// Drawing methods
// ---------------------------------------------------------------------------

void CanvasScene::fillRect(float x, float y, float w, float h) {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kFillRect;
    cmd.paint = makeFillPaint();
    cmd.p[0] = x; cmd.p[1] = y; cmd.p[2] = w; cmd.p[3] = h;
    recordDraw(std::move(cmd));
}

void CanvasScene::strokeRect(float x, float y, float w, float h) {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kStrokeRect;
    cmd.paint = makeStrokePaint();
    cmd.p[0] = x; cmd.p[1] = y; cmd.p[2] = w; cmd.p[3] = h;
    recordDraw(std::move(cmd));
}

void CanvasScene::clearRect(float x, float y, float w, float h) {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kClearRect;
    cmd.p[0] = x; cmd.p[1] = y; cmd.p[2] = w; cmd.p[3] = h;
    commands_.push_back(std::move(cmd));
    dirty_ = true;
    snapshotValid_ = false;
    snapshotImageValid_ = false;
}

// ---------------------------------------------------------------------------
// Path API
// ---------------------------------------------------------------------------

void CanvasScene::beginPath() {
    pathBuilder_.reset();
}

void CanvasScene::moveTo(float x, float y) {
    pathBuilder_.moveTo(x, y);
}

void CanvasScene::lineTo(float x, float y) {
    pathBuilder_.lineTo(x, y);
}

void CanvasScene::polyline(const float* coords, int numPoints) {
    if (numPoints < 1) return;
    pathBuilder_.moveTo(coords[0], coords[1]);
    for (int i = 1; i < numPoints; ++i) {
        pathBuilder_.lineTo(coords[i * 2], coords[i * 2 + 1]);
    }
}

void CanvasScene::closePath() {
    pathBuilder_.close();
}

void CanvasScene::stroke() {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kStrokePath;
    cmd.paint = makeStrokePaint();
    cmd.path = pathBuilder_.snapshot();
    recordDraw(std::move(cmd));
}

void CanvasScene::stroke(const SkPath& path) {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kStrokePath;
    cmd.paint = makeStrokePaint();
    cmd.path = path;
    recordDraw(std::move(cmd));
}

void CanvasScene::fill(const std::string& fillRule) {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kFillPath;
    cmd.paint = makeFillPaint();
    cmd.path = pathBuilder_.snapshot();
    if (fillRule == "evenodd") {
        cmd.path.setFillType(SkPathFillType::kEvenOdd);
    } else {
        cmd.path.setFillType(SkPathFillType::kWinding);
    }
    recordDraw(std::move(cmd));
}

void CanvasScene::fill(const SkPath& path, const std::string& fillRule) {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kFillPath;
    cmd.paint = makeFillPaint();
    cmd.path = path;
    if (fillRule == "evenodd") {
        cmd.path.setFillType(SkPathFillType::kEvenOdd);
    } else {
        cmd.path.setFillType(SkPathFillType::kWinding);
    }
    recordDraw(std::move(cmd));
}

void CanvasScene::clip(const std::string& fillRule) {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kClipPath;
    cmd.path = pathBuilder_.snapshot();
    if (fillRule == "evenodd") {
        cmd.path.setFillType(SkPathFillType::kEvenOdd);
    } else {
        cmd.path.setFillType(SkPathFillType::kWinding);
    }
    commands_.push_back(std::move(cmd));
}

void CanvasScene::clip(const SkPath& path, const std::string& fillRule) {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kClipPath;
    cmd.path = path;
    if (fillRule == "evenodd") {
        cmd.path.setFillType(SkPathFillType::kEvenOdd);
    } else {
        cmd.path.setFillType(SkPathFillType::kWinding);
    }
    commands_.push_back(std::move(cmd));
}

void CanvasScene::arc(float cx, float cy, float radius, float startAngle, float endAngle, bool acw) {
    float startDeg = startAngle * 180.0f / static_cast<float>(M_PI);
    float endDeg = endAngle * 180.0f / static_cast<float>(M_PI);

    float sweep = endDeg - startDeg;
    if (acw && sweep > 0) sweep -= 360.0f;
    else if (!acw && sweep < 0) sweep += 360.0f;

    SkRect oval = SkRect::MakeXYWH(cx - radius, cy - radius, radius * 2, radius * 2);

    // Line from current point to start of arc (or moveTo if no current point)
    float sx = cx + radius * std::cos(startAngle);
    float sy = cy + radius * std::sin(startAngle);

    SkPath current = pathBuilder_.snapshot();
    if (current.isEmpty()) {
        pathBuilder_.moveTo(sx, sy);
    } else {
        pathBuilder_.lineTo(sx, sy);
    }

    // Skia treats exactly ±360° sweep as degenerate (start==end).
    // For full circles, use addOval instead.
    if (std::abs(sweep) >= 360.0f) {
        pathBuilder_.addOval(oval, acw ? SkPathDirection::kCCW : SkPathDirection::kCW);
    } else {
        pathBuilder_.arcTo(oval, startDeg, sweep, false);
    }
}

void CanvasScene::arcTo(float x1, float y1, float x2, float y2, float radius) {
    pathBuilder_.arcTo(SkPoint::Make(x1, y1), SkPoint::Make(x2, y2), radius);
}

void CanvasScene::bezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) {
    pathBuilder_.cubicTo(cp1x, cp1y, cp2x, cp2y, x, y);
}

void CanvasScene::quadraticCurveTo(float cpx, float cpy, float x, float y) {
    pathBuilder_.quadTo(cpx, cpy, x, y);
}

void CanvasScene::ellipse(float cx, float cy, float rx, float ry, float rotation,
                          float startAngle, float endAngle, bool acw) {
    float startDeg = startAngle * 180.0f / static_cast<float>(M_PI);
    float endDeg = endAngle * 180.0f / static_cast<float>(M_PI);
    float sweep = endDeg - startDeg;
    if (acw && sweep > 0) sweep -= 360.0f;
    else if (!acw && sweep < 0) sweep += 360.0f;

    // Build a temporary path for the ellipse arc, then transform and append
    SkPathBuilder tmp;
    SkRect oval = SkRect::MakeXYWH(-rx, -ry, rx * 2, ry * 2);
    float sx = rx * std::cos(startAngle);
    float sy = ry * std::sin(startAngle);
    tmp.moveTo(sx, sy);
    // Skia treats exactly ±360° sweep as degenerate; use addOval for full ellipses.
    if (std::abs(sweep) >= 360.0f) {
        tmp.addOval(oval, acw ? SkPathDirection::kCCW : SkPathDirection::kCW);
    } else {
        tmp.arcTo(oval, startDeg, sweep, false);
    }

    SkPath tmpPath = tmp.detach();
    SkMatrix mat;
    mat.setRotate(rotation * 180.0f / static_cast<float>(M_PI));
    mat.postTranslate(cx, cy);
    tmpPath = tmpPath.makeTransform(mat);

    pathBuilder_.addPath(tmpPath);
}

void CanvasScene::rect(float x, float y, float w, float h) {
    pathBuilder_.addRect(SkRect::MakeXYWH(x, y, w, h));
}

void CanvasScene::roundRect(float x, float y, float w, float h, const SkVector radii[4]) {
    SkRRect rrect;
    SkRect r = SkRect::MakeXYWH(x, y, w, h).makeSorted();
    rrect.setRectRadii(r, radii);
    pathBuilder_.addRRect(rrect);
}

bool CanvasScene::isPointInPath(float x, float y, const std::string& fillRule) {
    SkPath p = pathBuilder_.snapshot();
    if (fillRule == "evenodd") {
        p.setFillType(SkPathFillType::kEvenOdd);
    } else {
        p.setFillType(SkPathFillType::kWinding);
    }
    return p.contains(x, y);
}

bool CanvasScene::isPointInPath(const SkPath& path, float x, float y, const std::string& fillRule) {
    SkPath p = path;
    if (fillRule == "evenodd") {
        p.setFillType(SkPathFillType::kEvenOdd);
    } else {
        p.setFillType(SkPathFillType::kWinding);
    }
    return p.contains(x, y);
}

// ---------------------------------------------------------------------------
// Transform
// ---------------------------------------------------------------------------

void CanvasScene::save() {
    stateStack_.push_back(state_);
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kSave;
    commands_.push_back(std::move(cmd));
}

void CanvasScene::restore() {
    // restore() with nothing saved does nothing, and must not record a
    // kRestore either: the replay would pop the save level the drawing
    // state's own clip lives on (see ensureSurface).
    if (stateStack_.empty()) return;
    state_ = stateStack_.back();
    stateStack_.pop_back();
    applyFont();
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kRestore;
    commands_.push_back(std::move(cmd));
}

void CanvasScene::translate(float tx, float ty) {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kTranslate;
    cmd.p[0] = tx; cmd.p[1] = ty;
    commands_.push_back(std::move(cmd));
}

void CanvasScene::rotate(float angle) {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kRotate;
    cmd.p[0] = angle * 180.0f / static_cast<float>(M_PI);
    commands_.push_back(std::move(cmd));
}

void CanvasScene::scale(float sx, float sy) {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kScale;
    cmd.p[0] = sx; cmd.p[1] = sy;
    commands_.push_back(std::move(cmd));
}

void CanvasScene::setTransform(float a, float b, float c, float d, float e, float f) {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kSetTransform;
    cmd.p[0] = a; cmd.p[1] = b; cmd.p[2] = c;
    cmd.p[3] = d; cmd.p[4] = e; cmd.p[5] = f;
    commands_.push_back(std::move(cmd));
}

void CanvasScene::resetTransform() {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kResetTransform;
    commands_.push_back(std::move(cmd));
}

void CanvasScene::transform(float a, float b, float c, float d, float e, float f) {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kConcatTransform;
    cmd.p[0] = a; cmd.p[1] = b; cmd.p[2] = c;
    cmd.p[3] = d; cmd.p[4] = e; cmd.p[5] = f;
    commands_.push_back(std::move(cmd));
}

// ---------------------------------------------------------------------------
// Image drawing
// ---------------------------------------------------------------------------

void CanvasScene::drawImage(const void* rgbaData, int imgW, int imgH,
                            float sx, float sy, float sw, float sh,
                            float dx, float dy, float dw, float dh) {
    if (!rgbaData || imgW <= 0 || imgH <= 0) return;

    auto info = SkImageInfo::Make(imgW, imgH, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    sk_sp<SkData> data = SkData::MakeWithCopy(rgbaData, imgW * imgH * 4);
    auto img = SkImages::RasterFromData(info, data, imgW * 4);
    if (!img) return;

    CanvasCmd cmd;
    cmd.type = CanvasCmd::kDrawImage;
    cmd.paint = makeImagePaint();
    cmd.img = std::move(img);
    cmd.src = SkRect::MakeXYWH(sx, sy, sw, sh);
    cmd.dst = SkRect::MakeXYWH(dx, dy, dw, dh);
    cmd.samp = imageSampling();
    recordDraw(std::move(cmd));
}

// ---------------------------------------------------------------------------
// Pixel manipulation
// ---------------------------------------------------------------------------

std::vector<uint8_t> CanvasScene::getImageData(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return {};
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4, 0);

    if (threaded_) {
        // Defer the readback to the canvas worker so it runs on the same
        // GL/GrContext that owns surface_. Re-uses the cached snapshot when
        // valid, otherwise round-trips through the worker via flushSync.
        int sw = queryLayoutWidth();
        int sh = queryLayoutHeight();
        if (sw <= 0 || sh <= 0) return pixels;
        if (!snapshotValid_ || snapshotW_ != sw || snapshotH_ != sh) {
            snapshotRequested_.store(true, std::memory_order_release);
            flushSync();
        }
        if (!snapshotValid_ || snapshot_.empty()) return pixels;

        // 64-bit edges: x + w must not overflow for a rectangle a script
        // placed near INT_MAX.
        const int x0 = static_cast<int>(std::clamp<int64_t>(x, 0, sw));
        const int x1 = static_cast<int>(std::clamp<int64_t>(int64_t{x} + w, 0, sw));
        const int y0 = static_cast<int>(std::clamp<int64_t>(y, 0, sh));
        const int y1 = static_cast<int>(std::clamp<int64_t>(int64_t{y} + h, 0, sh));
        if (x1 <= x0 || y1 <= y0) return pixels;
        const int copyW = x1 - x0;
        for (int row = y0; row < y1; ++row) {
            int dstRow = row - y;
            int dstCol = x0 - x;
            std::memcpy(&pixels[(static_cast<size_t>(dstRow) * w + dstCol) * 4],
                        &snapshot_[(static_cast<size_t>(row) * sw + x0) * 4],
                        static_cast<size_t>(copyW) * 4);
        }
        return pixels;
    }

    // Non-threaded path: surface lives on the calling thread, safe to read
    // here directly.
    flushCommands();
    if (!surface_) return pixels;
    if (grContext_) grContext_->resetContext();
    auto info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    surface_->readPixels(info, pixels.data(), w * 4, x, y);
    return pixels;
}

void CanvasScene::drawImage(sk_sp<SkImage> img,
                            float sx, float sy, float sw, float sh,
                            float dx, float dy, float dw, float dh) {
    if (!img) return;
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kDrawImage;
    cmd.paint = makeImagePaint();
    cmd.img = std::move(img);
    cmd.src = SkRect::MakeXYWH(sx, sy, sw, sh);
    cmd.dst = SkRect::MakeXYWH(dx, dy, dw, dh);
    cmd.samp = imageSampling();
    recordDraw(std::move(cmd));
}

sk_sp<SkImage> CanvasScene::snapshotImage() {
    if (snapshotImageValid_ && snapshotImage_) return snapshotImage_;

    int w = queryLayoutWidth();
    int h = queryLayoutHeight();
    if (w <= 0 || h <= 0) return nullptr;

    if (threaded_) {
        // Worker reads pixels on its own GL/GrContext and builds a portable
        // raster SkImage in snapshotImage_. Round-trip via flushSync.
        snapshotRequested_.store(true, std::memory_order_release);
        flushSync();
        return snapshotImageValid_ ? snapshotImage_ : nullptr;
    }

    // Non-threaded: read directly. A raster-backed SkImage is portable —
    // makeImageSnapshot would tie the result to this scene's grContext.
    // An undrawn canvas gets its (transparent) surface made to read from.
    flushCommands();
    if (!surface_) skCanvas();
    if (!surface_) return nullptr;
    if (grContext_) grContext_->resetContext();
    auto info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    snapshot_.assign(static_cast<size_t>(w) * h * 4, 0);
    if (!surface_->readPixels(info, snapshot_.data(), w * 4, 0, 0)) {
        snapshot_.clear();
        snapshotValid_ = false;
        snapshotImageValid_ = false;
        return nullptr;
    }
    snapshotW_ = w;
    snapshotH_ = h;
    snapshotValid_ = true;
    auto data = SkData::MakeWithCopy(snapshot_.data(), snapshot_.size());
    snapshotImage_ = SkImages::RasterFromData(info, data, w * 4);
    snapshotImageValid_ = static_cast<bool>(snapshotImage_);
    return snapshotImage_;
}

const uint8_t* CanvasScene::snapshotPixels(int w, int h) {
    if (snapshotValid_ && snapshotW_ == w && snapshotH_ == h && !snapshot_.empty()) {
        return snapshot_.data();
    }

    if (threaded_) {
        snapshotRequested_.store(true, std::memory_order_release);
        flushSync();
        if (!snapshotValid_ || snapshotW_ != w || snapshotH_ != h || snapshot_.empty()) {
            return nullptr;
        }
        return snapshot_.data();
    }

    // Non-threaded path. A canvas nothing has drawn to yet has no surface
    // (flushCommands creates it only for a command); its bitmap is still
    // transparent black of its size, so make the surface to read that from.
    flushCommands();
    if (!surface_) skCanvas();
    if (!surface_ || w <= 0 || h <= 0) {
        snapshotValid_ = false;
        return nullptr;
    }
    if (grContext_) grContext_->resetContext();
    auto info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    snapshot_.assign(static_cast<size_t>(w) * h * 4, 0);
    if (!surface_->readPixels(info, snapshot_.data(), w * 4, 0, 0)) {
        snapshot_.clear();
        snapshotValid_ = false;
        return nullptr;
    }
    snapshotW_ = w;
    snapshotH_ = h;
    snapshotValid_ = true;
    return snapshot_.data();
}

void CanvasScene::putImageData(const uint8_t* data, int w, int h, int dx, int dy) {
    putImageData(data, w, h, dx, dy, 0, 0, w, h);
}

void CanvasScene::putImageData(const uint8_t* data, int w, int h, int dx, int dy,
                               int dirtyX, int dirtyY, int dirtyWidth, int dirtyHeight) {
    if (!data || w <= 0 || h <= 0) return;

    // The dirty rectangle is a script's four numbers: clamp it in 64 bits, so
    // an edge near INT_MAX cannot overflow on the way into [0, w] x [0, h].
    int64_t rx = dirtyX, ry = dirtyY, rw = dirtyWidth, rh = dirtyHeight;
    if (rw < 0) { rx += rw; rw = -rw; }
    if (rh < 0) { ry += rh; rh = -rh; }
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > w) rw = w - rx;
    if (ry + rh > h) rh = h - ry;
    if (rw <= 0 || rh <= 0) return;
    dirtyX = static_cast<int>(rx);
    dirtyY = static_cast<int>(ry);
    dirtyWidth = static_cast<int>(rw);
    dirtyHeight = static_cast<int>(rh);

    auto info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    sk_sp<SkData> skData = SkData::MakeWithCopy(data, static_cast<size_t>(w) * h * 4);
    auto img = SkImages::RasterFromData(info, skData, w * 4);
    if (!img) return;

    // putImageData ignores transforms and compositing — write directly
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kPutImageData;
    cmd.paint.setBlendMode(SkBlendMode::kSrc);
    cmd.img = std::move(img);
    cmd.p[0] = static_cast<float>(dx);
    cmd.p[1] = static_cast<float>(dy);
    if (dirtyX != 0 || dirtyY != 0 || dirtyWidth != w || dirtyHeight != h) {
        cmd.src = SkRect::MakeXYWH(static_cast<float>(dirtyX), static_cast<float>(dirtyY),
                                  static_cast<float>(dirtyWidth), static_cast<float>(dirtyHeight));
        cmd.dst = SkRect::MakeXYWH(static_cast<float>(dx + dirtyX), static_cast<float>(dy + dirtyY),
                                  static_cast<float>(dirtyWidth), static_cast<float>(dirtyHeight));
    }
    commands_.push_back(std::move(cmd));
    dirty_ = true;
    snapshotValid_ = false;
    snapshotImageValid_ = false;
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

void CanvasScene::reset() {
    // Nothing recorded before a reset can show through it, so drop it here
    // rather than replay it only to clear it: the kReset below restores the
    // SkCanvas to its base level, which undoes any save or clip those
    // commands (or earlier frames) left behind.
    commands_.clear();
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kReset;
    commands_.push_back(std::move(cmd));
    dirty_ = true;
    snapshotValid_ = false;
    snapshotImageValid_ = false;

    state_ = defaultState();
    stateStack_.clear();
    pathBuilder_.reset();
    applyFont();
    if (resetHook_) resetHook_();
}

} // namespace bro::canvas
