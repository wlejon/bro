#include "canvas/canvas_scene.h"
#include "render/gl_context.h"
#include "render/skia_backend.h"
#include "util/log.h"

#include <SDL3/SDL.h>

#include <include/core/SkColorSpace.h>
#include <include/core/SkData.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkFontMetrics.h>
#include <include/core/SkFontStyle.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkMaskFilter.h>
#include <include/effects/SkDashPathEffect.h>
#include <include/core/SkBlendMode.h>
#include <include/core/SkBlurTypes.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkSamplingOptions.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/GrBackendSurface.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <include/gpu/ganesh/gl/GrGLBackendSurface.h>

#ifdef _WIN32
#include <include/ports/SkTypeface_win.h>
#elif defined(__APPLE__)
#include <include/ports/SkFontMgr_mac_ct.h>
#else
#include <include/ports/SkFontMgr_fontconfig.h>
#endif

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

CanvasScene::CanvasScene(render::Renderer* renderer)
    : renderer_(renderer)
{
    // Initialize default fill (black) and stroke (black) paints
    state_.fillPaint.setAntiAlias(true);
    state_.fillPaint.setStyle(SkPaint::kFill_Style);
    state_.fillPaint.setColor(SK_ColorBLACK);

    state_.strokePaint.setAntiAlias(true);
    state_.strokePaint.setStyle(SkPaint::kStroke_Style);
    state_.strokePaint.setColor(SK_ColorBLACK);
    state_.strokePaint.setStrokeWidth(1.0f);

    // Default font
    applyFont();
}

CanvasScene::~CanvasScene() {
    stopThread();
    cleanup();
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
}

// ---------------------------------------------------------------------------
// Threading
// ---------------------------------------------------------------------------

void CanvasScene::startThread(SDL_GLContext glCtx, SDL_Window* win) {
    if (threaded_ || !glCtx || !win) return;
    canvasGLContext_ = glCtx;
    threaded_ = true;
    canvasShared_.state.store(kCanvasIdle, std::memory_order_relaxed);
    canvasReady_.store(false, std::memory_order_relaxed);

    canvasThread_ = std::thread(&CanvasScene::canvasThreadFunc, this, win);

    // Block until the worker has completed SDL_GL_MakeCurrent on its context.
    // This is the serialization Windows/NVIDIA drivers need (no concurrent
    // wgl*Context calls against the same HDC) and costs nothing on other
    // platforms. It is NOT a mutex — just a one-shot atomic handshake.
    canvasReady_.wait(false, std::memory_order_acquire);
}

void CanvasScene::stopThread() {
    if (!threaded_) return;
    canvasShared_.state.store(kCanvasShutdown, std::memory_order_release);
    canvasShared_.state.notify_one();
    if (canvasThread_.joinable()) {
        canvasThread_.join();
    }
    threadGrContext_.reset();
    if (canvasGLContext_) {
        SDL_GL_DestroyContext(canvasGLContext_);
        canvasGLContext_ = nullptr;
    }
    threaded_ = false;
}

void CanvasScene::canvasThreadFunc(SDL_Window* win) {
    // The context was created by the main thread (required on macOS where
    // SDL_GL_CreateContext calls AppKit). Here we only make it current on
    // this worker thread — a thread-local GL operation on every platform.
    SDL_GL_MakeCurrent(win, canvasGLContext_);

    // Signal main before doing anything more so it can proceed to the next
    // canvas. After this point the main thread may call SDL_GL_CreateContext
    // again for another canvas; that's safe because our MakeCurrent has
    // completed and Skia's per-thread GL state tracking only touches our
    // own context from here on.
    canvasReady_.store(true, std::memory_order_release);
    canvasReady_.notify_one();

    threadGrContext_ = render::SkiaRenderer::createGrContext();
    if (!threadGrContext_) {
        LOG_ERROR("Canvas thread: failed to create GrDirectContext");
        return;
    }
    // Wire this thread's GrContext for surface creation
    grContext_ = threadGrContext_.get();

    LOG_INFO("Canvas thread started");

    while (true) {
        canvasShared_.state.wait(kCanvasIdle, std::memory_order_acquire);
        uint32_t s = canvasShared_.state.load(std::memory_order_acquire);

        if (s == kCanvasShutdown) break;
        if (s != kCanvasCommandsReady) continue;

        canvasShared_.state.store(kCanvasBusy, std::memory_order_relaxed);

        int cw = canvasShared_.canvasWidth.load(std::memory_order_relaxed);
        int ch = canvasShared_.canvasHeight.load(std::memory_order_relaxed);

        ensureSurface(cw, ch);

        if (threadGrContext_) {
            threadGrContext_->resetContext();
        }

        flushStagedCommands();

        if (dirty_) {
            dirty_ = false;
            if (threadGrContext_) {
                threadGrContext_->flushAndSubmit();
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }
        }

        // GL fence sync — ensures GPU commands complete before main thread
        // samples the texture for compositing.
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();

        canvasShared_.fenceSync.store(reinterpret_cast<uintptr_t>(fence),
                                       std::memory_order_relaxed);
        // Transition to textureReady — use compare_exchange to avoid overwriting
        // a pending kCanvasShutdown signal from the main thread.
        uint32_t expected = kCanvasBusy;
        if (canvasShared_.state.compare_exchange_strong(
                expected, kCanvasTextureReady,
                std::memory_order_release, std::memory_order_acquire)) {
            canvasShared_.state.notify_one();
        } else {
            // State was changed (shutdown) — delete the fence and exit
            glDeleteSync(fence);
            break;
        }
    }

    // Cleanup GL resources on this thread's context
    surface_.reset();
    threadGrContext_->flushAndSubmit();
    grContext_ = nullptr;

    SDL_GL_MakeCurrent(win, nullptr);
    LOG_INFO("Canvas thread stopped");
}

void CanvasScene::prepareAndSignal() {
    if (!threaded_) return;

    // Only signal when canvas thread is idle
    uint32_t s = canvasShared_.state.load(std::memory_order_acquire);
    if (s != kCanvasIdle) return;

    // Check if element was removed from the DOM
    if (detachedCb_ && detachedCb_(detachedUd_)) {
        detached_ = true;
        return;
    }

    // Query element layout position and size (main thread, DOM access)
    float layoutX = 0, layoutY = 0, layoutW = 0, layoutH = 0;
    if (layoutCb_) {
        layoutCb_(layoutUd_, layoutX, layoutY, layoutW, layoutH);
        if (layoutW <= 0 || layoutH <= 0) return;
    }

    screenX_ = layoutX;
    screenY_ = layoutY - viewportScrollY_;

    int canvasW = static_cast<int>(layoutW);
    int canvasH = static_cast<int>(layoutH);

    // Only signal if there's work to do (commands or resize)
    bool needsResize = (canvasW != surfWidth_ || canvasH != surfHeight_);
    if (commands_.empty() && !needsResize) return;

    // Swap commands to staged buffer (main thread only touches both when idle)
    std::swap(commands_, stagedCommands_);

    // Publish dimensions and signal
    canvasShared_.canvasWidth.store(canvasW, std::memory_order_relaxed);
    canvasShared_.canvasHeight.store(canvasH, std::memory_order_relaxed);
    canvasShared_.state.store(kCanvasCommandsReady, std::memory_order_release);
    canvasShared_.state.notify_one();
}

void CanvasScene::consumeFence() {
    if (!threaded_) return;

    uint32_t s = canvasShared_.state.load(std::memory_order_acquire);
    if (s == kCanvasTextureReady) {
        auto fence = reinterpret_cast<GLsync>(
            canvasShared_.fenceSync.exchange(0, std::memory_order_relaxed));
        if (fence) {
            glWaitSync(fence, 0, GL_TIMEOUT_IGNORED);
            glDeleteSync(fence);
        }
        canvasShared_.state.store(kCanvasIdle, std::memory_order_release);
    } else if (s == kCanvasBusy) {
        // Canvas thread still working — wait for it to finish
        canvasShared_.state.wait(kCanvasBusy, std::memory_order_acquire);
        // Recurse once to consume the fence (now should be kCanvasTextureReady)
        consumeFence();
    }
}

void CanvasScene::flushSync() {
    if (!threaded_) {
        flushCommands();
        return;
    }

    // If canvas thread is busy, wait for it to finish first
    uint32_t s = canvasShared_.state.load(std::memory_order_acquire);
    if (s == kCanvasBusy) {
        canvasShared_.state.wait(kCanvasBusy, std::memory_order_acquire);
        s = canvasShared_.state.load(std::memory_order_acquire);
    }
    if (s == kCanvasTextureReady) {
        consumeFence();
        s = canvasShared_.state.load(std::memory_order_acquire);
    }

    // Now idle — swap commands and signal
    if (s == kCanvasIdle && !commands_.empty()) {
        int cw = queryLayoutWidth();
        int ch = queryLayoutHeight();
        std::swap(commands_, stagedCommands_);
        canvasShared_.canvasWidth.store(cw, std::memory_order_relaxed);
        canvasShared_.canvasHeight.store(ch, std::memory_order_relaxed);
        canvasShared_.state.store(kCanvasCommandsReady, std::memory_order_release);
        canvasShared_.state.notify_one();

        // Wait for completion
        canvasShared_.state.wait(kCanvasCommandsReady, std::memory_order_acquire);
        canvasShared_.state.wait(kCanvasBusy, std::memory_order_acquire);
        consumeFence();
    }
}

void CanvasScene::stageCommandsForRaster() {
    if (commands_.empty()) return;
    if (stagedCommands_.empty()) {
        std::swap(commands_, stagedCommands_);
    } else {
        stagedCommands_.insert(stagedCommands_.end(),
            std::make_move_iterator(commands_.begin()),
            std::make_move_iterator(commands_.end()));
        commands_.clear();
    }
}

void CanvasScene::flushStagedCommands() {
    if (stagedCommands_.empty()) return;

    auto* c = skCanvas();
    if (!c) { stagedCommands_.clear(); return; }

    for (auto& cmd : stagedCommands_) {
        switch (cmd.type) {
        case CanvasCmd::kFillRect:
        case CanvasCmd::kStrokeRect:
            c->drawRect(SkRect::MakeXYWH(cmd.p[0], cmd.p[1], cmd.p[2], cmd.p[3]), cmd.paint);
            break;
        case CanvasCmd::kClearRect: {
            SkPaint clr;
            clr.setBlendMode(SkBlendMode::kClear);
            c->drawRect(SkRect::MakeXYWH(cmd.p[0], cmd.p[1], cmd.p[2], cmd.p[3]), clr);
            break;
        }
        case CanvasCmd::kStrokePath:
        case CanvasCmd::kFillPath:
            c->drawPath(cmd.path, cmd.paint);
            break;
        case CanvasCmd::kClipPath:
            c->clipPath(cmd.path, true);
            break;
        case CanvasCmd::kFillText:
        case CanvasCmd::kStrokeText:
            c->drawSimpleText(cmd.text.data(), cmd.text.size(), SkTextEncoding::kUTF8,
                              cmd.p[0], cmd.p[1], cmd.font, cmd.paint);
            break;
        case CanvasCmd::kDrawImage:
            c->drawImageRect(cmd.img, cmd.src, cmd.dst, cmd.samp, &cmd.paint,
                             SkCanvas::kStrict_SrcRectConstraint);
            break;
        case CanvasCmd::kPutImageData:
            c->save();
            c->resetMatrix();
            c->drawImage(cmd.img, cmd.p[0], cmd.p[1], cmd.samp, &cmd.paint);
            c->restore();
            break;
        case CanvasCmd::kSave:    c->save(); break;
        case CanvasCmd::kRestore: c->restore(); break;
        case CanvasCmd::kTranslate: c->translate(cmd.p[0], cmd.p[1]); break;
        case CanvasCmd::kRotate:    c->rotate(cmd.p[0]); break;
        case CanvasCmd::kScale:     c->scale(cmd.p[0], cmd.p[1]); break;
        case CanvasCmd::kSetTransform: {
            c->resetMatrix();
            SkMatrix m;
            m.setAll(cmd.p[0], cmd.p[2], cmd.p[4], cmd.p[1], cmd.p[3], cmd.p[5], 0, 0, 1);
            c->concat(m);
            break;
        }
        case CanvasCmd::kResetTransform:
            c->resetMatrix();
            break;
        case CanvasCmd::kConcatTransform: {
            SkMatrix m;
            m.setAll(cmd.p[0], cmd.p[2], cmd.p[4], cmd.p[1], cmd.p[3], cmd.p[5], 0, 0, 1);
            c->concat(m);
            break;
        }
        case CanvasCmd::kReset:
            c->clear(SK_ColorTRANSPARENT);
            break;
        }
    }
    stagedCommands_.clear();
}

// ---------------------------------------------------------------------------
// Surface management
// ---------------------------------------------------------------------------

int CanvasScene::queryLayoutWidth() const {
    if (layoutCb_) {
        float ox, oy, ow = 0, oh = 0;
        layoutCb_(layoutUd_, ox, oy, ow, oh);
        if (ow > 0) return static_cast<int>(ow);
    }
    return surfWidth_ > 0 ? surfWidth_ : 300;
}

int CanvasScene::queryLayoutHeight() const {
    if (layoutCb_) {
        float ox, oy, ow = 0, oh = 0;
        layoutCb_(layoutUd_, ox, oy, ow, oh);
        if (oh > 0) return static_cast<int>(oh);
    }
    return surfHeight_ > 0 ? surfHeight_ : 150;
}

void CanvasScene::ensureSurface(int w, int h) {
    if (surface_ && surfWidth_ == w && surfHeight_ == h) return;

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
        surface_->getCanvas()->clear(SK_ColorTRANSPARENT);
        dirty_ = true;
    }
}

SkCanvas* CanvasScene::skCanvas() {
    int w = queryLayoutWidth();
    int h = queryLayoutHeight();
    ensureSurface(w, h);
    return surface_ ? surface_->getCanvas() : nullptr;
}

// ---------------------------------------------------------------------------
// State setters
// ---------------------------------------------------------------------------

void CanvasScene::setFillColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    state_.fillPaint.setColor(SkColorSetARGB(a, r, g, b));
}

void CanvasScene::getFillColor(uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const {
    SkColor c = state_.fillPaint.getColor();
    a = SkColorGetA(c); r = SkColorGetR(c); g = SkColorGetG(c); b = SkColorGetB(c);
}

void CanvasScene::setStrokeColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    state_.strokePaint.setColor(SkColorSetARGB(a, r, g, b));
}

void CanvasScene::getStrokeColor(uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const {
    SkColor c = state_.strokePaint.getColor();
    a = SkColorGetA(c); r = SkColorGetR(c); g = SkColorGetG(c); b = SkColorGetB(c);
}

void CanvasScene::setLineWidth(float w) {
    state_.lineWidthVal = w;
    state_.strokePaint.setStrokeWidth(w);
}

float CanvasScene::lineWidth() const { return state_.lineWidthVal; }

void CanvasScene::setGlobalAlpha(float a) {
    state_.globalAlphaVal = std::clamp(a, 0.0f, 1.0f);
}

float CanvasScene::globalAlpha() const { return state_.globalAlphaVal; }

void CanvasScene::setLineCap(int cap) {
    state_.lineCapVal = cap;
    static const SkPaint::Cap caps[] = { SkPaint::kButt_Cap, SkPaint::kRound_Cap, SkPaint::kSquare_Cap };
    if (cap >= 0 && cap <= 2) state_.strokePaint.setStrokeCap(caps[cap]);
}

int CanvasScene::lineCap() const { return state_.lineCapVal; }

void CanvasScene::setLineJoin(int join) {
    state_.lineJoinVal = join;
    static const SkPaint::Join joins[] = { SkPaint::kMiter_Join, SkPaint::kRound_Join, SkPaint::kBevel_Join };
    if (join >= 0 && join <= 2) state_.strokePaint.setStrokeJoin(joins[join]);
}

int CanvasScene::lineJoin() const { return state_.lineJoinVal; }

void CanvasScene::setMiterLimit(float limit) {
    state_.miterLimitVal = limit;
    state_.strokePaint.setStrokeMiter(limit);
}

float CanvasScene::miterLimit() const { return state_.miterLimitVal; }

static SkBlendMode blendModeFromOp(int op) {
    switch (op) {
    case 0:  return SkBlendMode::kSrcOver;
    case 1:  return SkBlendMode::kSrcIn;
    case 2:  return SkBlendMode::kSrcOut;
    case 3:  return SkBlendMode::kSrcATop;
    case 4:  return SkBlendMode::kDstOver;
    case 5:  return SkBlendMode::kDstIn;
    case 6:  return SkBlendMode::kDstOut;
    case 7:  return SkBlendMode::kDstATop;
    case 8:  return SkBlendMode::kLighten;
    case 9:  return SkBlendMode::kDarken;
    case 10: return SkBlendMode::kXor;
    case 11: return SkBlendMode::kPlus;
    case 12: return SkBlendMode::kMultiply;
    case 13: return SkBlendMode::kScreen;
    case 14: return SkBlendMode::kOverlay;
    case 15: return SkBlendMode::kColorDodge;
    case 16: return SkBlendMode::kColorBurn;
    case 17: return SkBlendMode::kHardLight;
    case 18: return SkBlendMode::kSoftLight;
    case 19: return SkBlendMode::kDifference;
    case 20: return SkBlendMode::kExclusion;
    default: return SkBlendMode::kSrcOver;
    }
}

void CanvasScene::setGlobalCompositeOperation(int op) {
    state_.compositeOp = op;
}

int CanvasScene::globalCompositeOperation() const { return state_.compositeOp; }

void CanvasScene::setFont(const std::string& fontStr) {
    state_.fontStr = fontStr;
    fontString_ = fontStr;
    applyFont();
}

void CanvasScene::setTextAlign(int align) {
    state_.textAlignVal = align;
    textAlign_ = align;
}

void CanvasScene::setTextBaseline(int bl) {
    state_.textBaselineVal = bl;
    textBaseline_ = bl;
}

void CanvasScene::setShadowBlur(float blur) {
    state_.shadowBlurVal = blur;
    shadowBlur_ = blur;
}

void CanvasScene::setShadowColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    state_.shadowR = r; state_.shadowG = g; state_.shadowB = b; state_.shadowA = a;
    shadowR_ = r; shadowG_ = g; shadowB_ = b; shadowA_ = a;
}

void CanvasScene::getShadowColor(uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const {
    r = shadowR_; g = shadowG_; b = shadowB_; a = shadowA_;
}

void CanvasScene::setShadowOffsetX(float x) { state_.shadowOX = x; shadowOffsetX_ = x; }
void CanvasScene::setShadowOffsetY(float y) { state_.shadowOY = y; shadowOffsetY_ = y; }

void CanvasScene::setImageSmoothingEnabled(bool v) {
    state_.imgSmooth = v;
    imageSmoothingEnabled_ = v;
}

// ---------------------------------------------------------------------------
// Font management
// ---------------------------------------------------------------------------

void CanvasScene::applyFont() {
    auto it = fontCache_.find(fontString_);
    if (it != fontCache_.end()) {
        font_ = it->second.font;
        return;
    }

    auto pf = parseCSSFont(fontString_);

#ifdef _WIN32
    auto mgr = SkFontMgr_New_DirectWrite();
#elif defined(__APPLE__)
    auto mgr = SkFontMgr_New_CoreText(nullptr);
#else
    auto mgr = SkFontMgr_New_FontConfig(nullptr);
#endif

    // Resolve CSS generic family names to platform font names
    auto resolveGeneric = [](const std::string& name) -> const char* {
#ifdef _WIN32
        if (name == "sans-serif")  return "Arial";
        if (name == "serif")       return "Times New Roman";
        if (name == "monospace")   return "Consolas";
        if (name == "cursive")     return "Comic Sans MS";
        if (name == "fantasy")     return "Impact";
        if (name == "system-ui")   return "Segoe UI";
#elif defined(__APPLE__)
        if (name == "sans-serif")  return "Helvetica";
        if (name == "serif")       return "Times";
        if (name == "monospace")   return "Menlo";
        if (name == "cursive")     return "Apple Chancery";
        if (name == "fantasy")     return "Papyrus";
        if (name == "system-ui")   return "-apple-system";
#else
        if (name == "sans-serif")  return "Liberation Sans";
        if (name == "serif")       return "Liberation Serif";
        if (name == "monospace")   return "Liberation Mono";
        if (name == "cursive")     return "DejaVu Sans";
        if (name == "fantasy")     return "DejaVu Sans";
        if (name == "system-ui")   return "Liberation Sans";
#endif
        return nullptr;
    };

    const char* resolved = resolveGeneric(pf.family);
    const char* familyName = resolved ? resolved : pf.family.c_str();

    SkFontStyle style(
        pf.weight,
        SkFontStyle::kNormal_Width,
        pf.italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);

    sk_sp<SkTypeface> tf = mgr->matchFamilyStyle(familyName, style);
    if (!tf && resolved) tf = mgr->matchFamilyStyle(pf.family.c_str(), style);
    if (!tf) tf = mgr->matchFamilyStyle(nullptr, style);

    SkFont f(tf, pf.size);
    f.setEdging(SkFont::Edging::kSubpixelAntiAlias);
    f.setSubpixel(true);

    fontCache_[fontString_] = {tf, f};
    font_ = f;
}

// ---------------------------------------------------------------------------
// Paint helpers
// ---------------------------------------------------------------------------

SkPaint CanvasScene::makeFillPaint() const {
    SkPaint p = state_.fillPaint;
    p.setAlphaf(p.getAlphaf() * state_.globalAlphaVal);
    p.setBlendMode(blendModeFromOp(state_.compositeOp));
    return p;
}

SkPaint CanvasScene::makeStrokePaint() const {
    SkPaint p = state_.strokePaint;
    p.setAlphaf(p.getAlphaf() * state_.globalAlphaVal);
    p.setBlendMode(blendModeFromOp(state_.compositeOp));
    if (!state_.lineDash.empty()) {
        // HTML5 spec: odd-length segment arrays are doubled.
        std::vector<float> segs = state_.lineDash;
        if (segs.size() % 2 == 1) segs.insert(segs.end(), state_.lineDash.begin(), state_.lineDash.end());
        p.setPathEffect(SkDashPathEffect::Make(
            SkSpan<const SkScalar>(segs.data(), segs.size()),
            state_.lineDashOffset));
    }
    return p;
}

void CanvasScene::setLineDash(const std::vector<float>& segments) {
    state_.lineDash.clear();
    state_.lineDash.reserve(segments.size());
    for (float v : segments) {
        if (!(v >= 0) || !std::isfinite(v)) { state_.lineDash.clear(); return; }
        state_.lineDash.push_back(v);
    }
}

const std::vector<float>& CanvasScene::lineDash() const { return state_.lineDash; }

void CanvasScene::setLineDashOffset(float off) {
    if (std::isfinite(off)) state_.lineDashOffset = off;
}

float CanvasScene::lineDashOffset() const { return state_.lineDashOffset; }

void CanvasScene::applyShadow(SkPaint& paint) const {
    if (shadowA_ > 0 && (shadowBlur_ > 0 || shadowOffsetX_ != 0 || shadowOffsetY_ != 0)) {
        // Skia doesn't have a direct shadow — we'd need a separate draw pass.
        // For now, apply a blur mask filter as an approximation when blur > 0.
        if (shadowBlur_ > 0) {
            paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, shadowBlur_ / 2.0f));
        }
    }
}

float CanvasScene::adjustTextX(float x, float textWidth) const {
    switch (textAlign_) {
    case 1: return x - textWidth / 2.0f;  // center
    case 2: case 3: return x - textWidth; // right / end
    default: return x;                     // left / start
    }
}

float CanvasScene::adjustTextY(float y) const {
    SkFontMetrics metrics;
    font_.getMetrics(&metrics);
    switch (textBaseline_) {
    case 1: return y - metrics.fAscent;           // top
    case 2: return y - (metrics.fAscent + metrics.fDescent) / 2.0f; // middle
    case 3: return y - metrics.fDescent;          // bottom
    case 4: return y - metrics.fAscent * 0.8f;    // hanging (approximation)
    case 5: return y - metrics.fDescent;          // ideographic ≈ bottom
    default: return y;                             // alphabetic (baseline)
    }
}

// ---------------------------------------------------------------------------
// Drawing methods
// ---------------------------------------------------------------------------

void CanvasScene::fillRect(float x, float y, float w, float h) {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kFillRect;
    cmd.paint = makeFillPaint();
    cmd.p[0] = x; cmd.p[1] = y; cmd.p[2] = w; cmd.p[3] = h;
    commands_.push_back(std::move(cmd));
    dirty_ = true;
}

void CanvasScene::strokeRect(float x, float y, float w, float h) {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kStrokeRect;
    cmd.paint = makeStrokePaint();
    cmd.p[0] = x; cmd.p[1] = y; cmd.p[2] = w; cmd.p[3] = h;
    commands_.push_back(std::move(cmd));
    dirty_ = true;
}

void CanvasScene::clearRect(float x, float y, float w, float h) {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kClearRect;
    cmd.p[0] = x; cmd.p[1] = y; cmd.p[2] = w; cmd.p[3] = h;
    commands_.push_back(std::move(cmd));
    dirty_ = true;
}

void CanvasScene::fillText(const std::string& text, float x, float y) {
    if (text.empty()) return;
    float tw = font_.measureText(text.data(), text.size(), SkTextEncoding::kUTF8);
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kFillText;
    cmd.paint = makeFillPaint();
    cmd.p[0] = adjustTextX(x, tw);
    cmd.p[1] = adjustTextY(y);
    cmd.text = text;
    cmd.font = font_;
    commands_.push_back(std::move(cmd));
    dirty_ = true;
}

void CanvasScene::strokeText(const std::string& text, float x, float y) {
    if (text.empty()) return;
    float tw = font_.measureText(text.data(), text.size(), SkTextEncoding::kUTF8);
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kStrokeText;
    cmd.paint = makeStrokePaint();
    cmd.p[0] = adjustTextX(x, tw);
    cmd.p[1] = adjustTextY(y);
    cmd.text = text;
    cmd.font = font_;
    commands_.push_back(std::move(cmd));
    dirty_ = true;
}

render::TextMetrics CanvasScene::measureText(const std::string& text) {
    SkFontMetrics fm;
    font_.getMetrics(&fm);
    float w = font_.measureText(text.data(), text.size(), SkTextEncoding::kUTF8);
    return { w, -fm.fAscent + fm.fDescent, -fm.fAscent, fm.fDescent };
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
    commands_.push_back(std::move(cmd));
    dirty_ = true;
}

void CanvasScene::fill() {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kFillPath;
    cmd.paint = makeFillPaint();
    cmd.path = pathBuilder_.snapshot();
    commands_.push_back(std::move(cmd));
    dirty_ = true;
}

void CanvasScene::clip() {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kClipPath;
    cmd.path = pathBuilder_.snapshot();
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
    tmp.arcTo(oval, startDeg, sweep, false);

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

bool CanvasScene::isPointInPath(float x, float y) {
    return pathBuilder_.snapshot().contains(x, y);
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
    if (!stateStack_.empty()) {
        state_ = stateStack_.back();
        stateStack_.pop_back();
        // Sync convenience members from restored state
        fontString_ = state_.fontStr;
        textAlign_ = state_.textAlignVal;
        textBaseline_ = state_.textBaselineVal;
        shadowBlur_ = state_.shadowBlurVal;
        shadowR_ = state_.shadowR; shadowG_ = state_.shadowG;
        shadowB_ = state_.shadowB; shadowA_ = state_.shadowA;
        shadowOffsetX_ = state_.shadowOX; shadowOffsetY_ = state_.shadowOY;
        imageSmoothingEnabled_ = state_.imgSmooth;
        applyFont();
    }
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
    cmd.paint.setAlphaf(state_.globalAlphaVal);
    cmd.paint.setBlendMode(blendModeFromOp(state_.compositeOp));
    cmd.img = std::move(img);
    cmd.src = SkRect::MakeXYWH(sx, sy, sw, sh);
    cmd.dst = SkRect::MakeXYWH(dx, dy, dw, dh);
    cmd.samp = imageSmoothingEnabled_
        ? SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear)
        : SkSamplingOptions(SkFilterMode::kNearest, SkMipmapMode::kNone);
    commands_.push_back(std::move(cmd));
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// Pixel manipulation
// ---------------------------------------------------------------------------

std::vector<uint8_t> CanvasScene::getImageData(int x, int y, int w, int h) {
    // Flush deferred commands so pixel state is current
    if (threaded_) {
        flushSync();
    } else {
        flushCommands();
    }

    std::vector<uint8_t> pixels(w * h * 4, 0);
    if (!surface_) return pixels;

    auto info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    surface_->readPixels(info, pixels.data(), w * 4, x, y);
    return pixels;
}

void CanvasScene::putImageData(const uint8_t* data, int w, int h, int dx, int dy) {
    if (!data) return;

    auto info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    sk_sp<SkData> skData = SkData::MakeWithCopy(data, w * h * 4);
    auto img = SkImages::RasterFromData(info, skData, w * 4);
    if (!img) return;

    // putImageData ignores transforms and compositing — write directly
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kPutImageData;
    cmd.paint.setBlendMode(SkBlendMode::kSrc);
    cmd.img = std::move(img);
    cmd.p[0] = static_cast<float>(dx);
    cmd.p[1] = static_cast<float>(dy);
    commands_.push_back(std::move(cmd));
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

void CanvasScene::reset() {
    CanvasCmd cmd;
    cmd.type = CanvasCmd::kReset;
    commands_.push_back(std::move(cmd));
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// Command buffer replay
// ---------------------------------------------------------------------------

void CanvasScene::flushCommands() {
    if (commands_.empty()) return;

    auto* c = skCanvas();
    if (!c) { commands_.clear(); return; }

    for (auto& cmd : commands_) {
        switch (cmd.type) {
        case CanvasCmd::kFillRect:
        case CanvasCmd::kStrokeRect:
            c->drawRect(SkRect::MakeXYWH(cmd.p[0], cmd.p[1], cmd.p[2], cmd.p[3]), cmd.paint);
            break;
        case CanvasCmd::kClearRect: {
            SkPaint clr;
            clr.setBlendMode(SkBlendMode::kClear);
            c->drawRect(SkRect::MakeXYWH(cmd.p[0], cmd.p[1], cmd.p[2], cmd.p[3]), clr);
            break;
        }
        case CanvasCmd::kStrokePath:
        case CanvasCmd::kFillPath:
            c->drawPath(cmd.path, cmd.paint);
            break;
        case CanvasCmd::kClipPath:
            c->clipPath(cmd.path, true);
            break;
        case CanvasCmd::kFillText:
        case CanvasCmd::kStrokeText:
            c->drawSimpleText(cmd.text.data(), cmd.text.size(), SkTextEncoding::kUTF8,
                              cmd.p[0], cmd.p[1], cmd.font, cmd.paint);
            break;
        case CanvasCmd::kDrawImage:
            c->drawImageRect(cmd.img, cmd.src, cmd.dst, cmd.samp, &cmd.paint,
                             SkCanvas::kStrict_SrcRectConstraint);
            break;
        case CanvasCmd::kPutImageData:
            c->save();
            c->resetMatrix();
            c->drawImage(cmd.img, cmd.p[0], cmd.p[1], cmd.samp, &cmd.paint);
            c->restore();
            break;
        case CanvasCmd::kSave:    c->save(); break;
        case CanvasCmd::kRestore: c->restore(); break;
        case CanvasCmd::kTranslate: c->translate(cmd.p[0], cmd.p[1]); break;
        case CanvasCmd::kRotate:    c->rotate(cmd.p[0]); break;
        case CanvasCmd::kScale:     c->scale(cmd.p[0], cmd.p[1]); break;
        case CanvasCmd::kSetTransform: {
            c->resetMatrix();
            SkMatrix m;
            m.setAll(cmd.p[0], cmd.p[2], cmd.p[4], cmd.p[1], cmd.p[3], cmd.p[5], 0, 0, 1);
            c->concat(m);
            break;
        }
        case CanvasCmd::kResetTransform:
            c->resetMatrix();
            break;
        case CanvasCmd::kConcatTransform: {
            SkMatrix m;
            m.setAll(cmd.p[0], cmd.p[2], cmd.p[4], cmd.p[1], cmd.p[3], cmd.p[5], 0, 0, 1);
            c->concat(m);
            break;
        }
        case CanvasCmd::kReset:
            c->clear(SK_ColorTRANSPARENT);
            break;
        }
    }
    commands_.clear();
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// Compositing — upload raster pixels to GL texture
// ---------------------------------------------------------------------------

void CanvasScene::rasterize(render::GLContext* gl) {
    if (!gl) return;

    // Check if element was removed from the DOM
    if (detachedCb_ && detachedCb_(detachedUd_)) {
        detached_ = true;
        return;
    }

    // Query element layout position and size
    float layoutX = 0, layoutY = 0, layoutW = 0, layoutH = 0;
    if (layoutCb_) {
        layoutCb_(layoutUd_, layoutX, layoutY, layoutW, layoutH);
        if (layoutW <= 0 || layoutH <= 0) return;
    }

    screenX_ = layoutX;
    screenY_ = layoutY - viewportScrollY_;

    int canvasW = static_cast<int>(layoutW);
    int canvasH = static_cast<int>(layoutH);
    ensureSurface(canvasW, canvasH);

    // Replay deferred canvas commands onto the Skia surface
    if (grContext_) {
        // Tell Ganesh to re-sync with actual GL state before drawing,
        // since the engine's raw GL operations may have changed it.
        grContext_->resetContext();
    }

    flushCommands();

    if (!dirty_) return;
    dirty_ = false;

    if (grContext_) {
        // GPU path: Skia already drew to the FBO texture via Ganesh.
        // Flush GPU commands and restore default framebuffer.
        grContext_->flushAndSubmit();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    // CPU fallback: upload raster pixels to GL texture
    if (!surface_) return;

    SkPixmap pixmap;
    if (!surface_->peekPixels(&pixmap)) return;

    bool needsAlloc = false;
    if (!glTexture_) {
        glGenTextures(1, &glTexture_);
        glBindTexture(GL_TEXTURE_2D, glTexture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        needsAlloc = true;
    } else {
        glBindTexture(GL_TEXTURE_2D, glTexture_);
        if (canvasW != texWidth_ || canvasH != texHeight_) needsAlloc = true;
    }

    if (needsAlloc) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, canvasW, canvasH, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, pixmap.addr());
        texWidth_ = canvasW;
        texHeight_ = canvasH;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvasW, canvasH,
                        GL_BGRA, GL_UNSIGNED_BYTE, pixmap.addr());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace bro::canvas
