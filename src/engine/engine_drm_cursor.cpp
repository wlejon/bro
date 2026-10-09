// The DRM display's pointer on the KMS cursor plane: drawn by the display,
// not into the frame, so a moving pointer redraws nothing and a client
// scanned out directly stays scanned out. The cursor shape is rasterized
// once per shape and scale into the plane's buffer; a shape too big for the
// plane, or a driver that refuses the plane, falls back to drawing the
// cursor into the frame (renderAndPresentFrame).
#include "engine/engine.h"
#include "render/software_cursor.h"
#include "render/vulkan_presenter.h"
#if BRO_WITH_DMABUF
#include "render/kms_direct_presenter.h"
#endif

#if BRO_WITH_COMPOSITOR
#include "compositor/wayland_compositor.h"
#endif

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkSurface.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <vector>

namespace bro::engine {

#if BRO_WITH_COMPOSITOR
namespace {

// The picture of the client that has the pointer, when it set one
// (wl_pointer.set_cursor with a surface): where the cursor is the client's
// (screenCursorShape), and not while the shell's drag is under way.
bool clientCursor(DrmPlatformContext* ctx, bool shellApp, compositor::WaylandCompositor::CursorPixels& out) {
    if (!ctx || !ctx->compositor || ctx->shellDrag != 0) return false;
    if (shellApp && !ctx->pointerOnClient) return false;
    return ctx->compositor->acquireClientCursor(out) && out.bgra && out.pixelW > 0 && out.pixelH > 0;
}

}  // namespace
#endif

bool Engine::drmPlaceHardwareCursor() {
#if BRO_WITH_DMABUF
    auto* kms = vulkanPresenter_ ? vulkanPresenter_->kmsDirectPresenter() : nullptr;
    if (!kms) return false;
    if (!kms->hasCursorPlane() || kms->cursorRefused()) {
        kms->setCursor(false, 0, 0);
        return false;
    }
#if BRO_WITH_COMPOSITOR
    // A client's own picture, scaled to device pixels: on the plane when it
    // fits there, else drawn into the frame (drawClientCursorIntoFrame).
    compositor::WaylandCompositor::CursorPixels cur;
    if (cursorVisible_ && !lockedElement_.get() && clientCursor(drmCtx_.get(), isShellApp(), cur)) {
        const float scale = deviceScale_.render > 0.0f ? deviceScale_.render : 1.0f;
        char key[96];
        std::snprintf(key, sizeof(key), "client:%llx@%.3f", static_cast<unsigned long long>(cur.key), scale);
        if (hwCursorKey_ != key) {
            hwCursorKey_ = key;
            hwCursorFits_ = false;
            const int w = std::max(1, static_cast<int>(std::lround(cur.w * scale)));
            const int h = std::max(1, static_cast<int>(std::lround(cur.h * scale)));
            if (static_cast<uint32_t>(w) <= kms->cursorWidth() && static_cast<uint32_t>(h) <= kms->cursorHeight()) {
                if (w == cur.pixelW && h == cur.pixelH) {
                    hwCursorFits_ = kms->setCursorImage(cur.bgra->data(), static_cast<uint32_t>(w),
                                                        static_cast<uint32_t>(h), static_cast<uint32_t>(w) * 4);
                } else {
                    const SkImageInfo src =
                        SkImageInfo::Make(cur.pixelW, cur.pixelH, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
                    sk_sp<SkImage> image = SkImages::RasterFromPixmapCopy(
                        SkPixmap(src, cur.bgra->data(), static_cast<size_t>(cur.pixelW) * 4));
                    const SkImageInfo dst = SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
                    sk_sp<SkSurface> surface = SkSurfaces::Raster(dst);
                    if (image && surface) {
                        surface->getCanvas()->clear(SK_ColorTRANSPARENT);
                        surface->getCanvas()->drawImageRect(image, SkRect::MakeWH(float(w), float(h)),
                                                            SkSamplingOptions(SkFilterMode::kLinear), nullptr);
                        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
                        if (surface->readPixels(dst, px.data(), static_cast<size_t>(w) * 4, 0, 0))
                            hwCursorFits_ = kms->setCursorImage(px.data(), static_cast<uint32_t>(w),
                                                                static_cast<uint32_t>(h), static_cast<uint32_t>(w) * 4);
                    }
                }
            }
        }
        if (!hwCursorFits_) {
            kms->setCursor(false, 0, 0);
            return false;
        }
        // The hotspot moves without a new picture (wl_surface.offset).
        const float sx = static_cast<float>(frameCompositeW_) / static_cast<float>(viewportWidth_ > 0 ? viewportWidth_ : 1);
        const float sy = static_cast<float>(frameCompositeH_) / static_cast<float>(viewportHeight_ > 0 ? viewportHeight_ : 1);
        kms->setCursor(true, static_cast<int32_t>(std::lround((lastMouseX_ - cur.hotX) * sx)),
                       static_cast<int32_t>(std::lround((lastMouseY_ - cur.hotY) * sy)));
        return true;
    }
#endif
    const std::string shape = cursorVisible_ && !lockedElement_.get() ? screenCursorShape() : std::string("none");
    if (shape == "none" || shape.empty()) {
        kms->setCursor(false, 0, 0);
        return true;  // nothing to draw either way
    }

    const float scale = deviceScale_.render > 0.0f ? deviceScale_.render : 1.0f;
    char key[96];
    std::snprintf(key, sizeof(key), "%s@%.3f", shape.c_str(), scale);
    if (hwCursorKey_ != key) {
        hwCursorKey_ = key;
        hwCursorFits_ = false;
        // Drawn with its hotspot in the middle of a canvas far bigger than
        // any cursor, then cut to what it covers: the hotspot is wherever
        // that leaves it.
        constexpr int kCanvas = 512, kHot = kCanvas / 2;
        const SkImageInfo info = SkImageInfo::Make(kCanvas, kCanvas, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
        sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
        if (surface) {
            surface->getCanvas()->clear(SK_ColorTRANSPARENT);
            render::drawSoftwareCursor(surface->getCanvas(), kHot, kHot, shape, scale);
            std::vector<uint8_t> px(static_cast<size_t>(kCanvas) * kCanvas * 4);
            if (surface->readPixels(info, px.data(), kCanvas * 4, 0, 0)) {
                int left = kCanvas, top = kCanvas, right = -1, bottom = -1;
                for (int y = 0; y < kCanvas; ++y) {
                    const uint8_t* row = px.data() + static_cast<size_t>(y) * kCanvas * 4;
                    for (int x = 0; x < kCanvas; ++x) {
                        if (!row[x * 4 + 3]) continue;
                        left = std::min(left, x);
                        right = std::max(right, x);
                        top = std::min(top, y);
                        bottom = std::max(bottom, y);
                    }
                }
                if (right >= left) {
                    const uint32_t w = static_cast<uint32_t>(right - left + 1);
                    const uint32_t h = static_cast<uint32_t>(bottom - top + 1);
                    if (w <= kms->cursorWidth() && h <= kms->cursorHeight()) {
                        const uint8_t* origin = px.data() + (static_cast<size_t>(top) * kCanvas + left) * 4;
                        hwCursorFits_ = kms->setCursorImage(origin, w, h, kCanvas * 4);
                        hwCursorHotX_ = kHot - left;
                        hwCursorHotY_ = kHot - top;
                    }
                }
            }
        }
    }
    if (!hwCursorFits_) {
        kms->setCursor(false, 0, 0);
        return false;
    }
    const float sx = static_cast<float>(frameCompositeW_) / static_cast<float>(viewportWidth_ > 0 ? viewportWidth_ : 1);
    const float sy = static_cast<float>(frameCompositeH_) / static_cast<float>(viewportHeight_ > 0 ? viewportHeight_ : 1);
    kms->setCursor(true, static_cast<int32_t>(std::lround(lastMouseX_ * sx)) - hwCursorHotX_,
                   static_cast<int32_t>(std::lround(lastMouseY_ * sy)) - hwCursorHotY_);
    return true;
#else
    return false;
#endif
}

// The icon of a drag between clients, at the pointer above everything: a
// dmabuf icon composited as a layer, a shm one drawn from its pixels. It is
// part of the frame's key (its picture and where it is), so a frame with a
// drag moving is never held.
void Engine::compositeDragIcon() {
#if BRO_WITH_COMPOSITOR
    if (!drmCtx_ || !drmCtx_->compositor) return;
    const float sx = static_cast<float>(frameCompositeW_) / static_cast<float>(viewportWidth_ > 0 ? viewportWidth_ : 1);
    const float sy = static_cast<float>(frameCompositeH_) / static_cast<float>(viewportHeight_ > 0 ? viewportHeight_ : 1);
    // The shell's own drag (startShellDrag): its label at the pointer.
    if (auto& ctx = *drmCtx_; ctx.shellDrag != 0) {
        if (ctx.shellDragIcon.empty() || ctx.shellDragIconW <= 0 || ctx.shellDragIconH <= 0) return;
        SkCanvas* canvas = frameSegmentCanvas();
        if (!canvas) return;
        const SkImageInfo info =
            SkImageInfo::Make(ctx.shellDragIconW, ctx.shellDragIconH, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
        sk_sp<SkImage> image = SkImages::RasterFromPixmapCopy(
            SkPixmap(info, ctx.shellDragIcon.data(), static_cast<size_t>(ctx.shellDragIconW) * 4));
        if (!image) return;
        const float x = lastMouseX_ - static_cast<float>(ctx.shellDragHotX);
        const float y = lastMouseY_ - static_cast<float>(ctx.shellDragHotY);
        frameKeyAdd(0x5d4au ^ ctx.shellDrag);
        frameKeyAdd(static_cast<uint64_t>(std::lround(x)) << 32 ^ static_cast<uint32_t>(std::lround(y)));
        canvas->drawImageRect(image,
                              SkRect::MakeXYWH(x * sx, y * sy, static_cast<float>(ctx.shellDragIconW) * sx,
                                               static_cast<float>(ctx.shellDragIconH) * sy),
                              SkSamplingOptions(SkFilterMode::kLinear), nullptr);
        return;
    }
    std::vector<engine::UILayer> layers;
    std::vector<compositor::LeasedSurfaceFrame> leased;
    compositor::WaylandCompositor::DragIconPixels icon;
    if (!drmCtx_->compositor->acquireDragIcon(lastMouseX_, lastMouseY_, layers, leased, icon)) return;
    frameKeyAdd(0xd4a6u);
    frameKeyAdd(static_cast<uint64_t>(std::lround(lastMouseX_)) << 32 ^ static_cast<uint32_t>(std::lround(lastMouseY_)));
    if (!layers.empty()) {
        for (const auto& lf : leased) {
            frameKeyAdd(lf.surfaceId);
#if BRO_HAVE_WAYLAND_SERVER
            frameKeyAdd(lf.frame.sequence);
            frameKeyAdd(lf.frame.image_id);
#endif
        }
        compositeLayers(layers);
        drmCtx_->leasedFrames.insert(drmCtx_->leasedFrames.end(), std::make_move_iterator(leased.begin()),
                                     std::make_move_iterator(leased.end()));
        return;
    }
    if (!icon.bgra || icon.pixelW <= 0 || icon.pixelH <= 0) return;
    SkCanvas* canvas = frameSegmentCanvas();
    if (!canvas) return;
    frameKeyAdd(icon.key);
    const SkImageInfo info =
        SkImageInfo::Make(icon.pixelW, icon.pixelH, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    sk_sp<SkImage> image =
        SkImages::RasterFromPixmapCopy(SkPixmap(info, icon.bgra->data(), static_cast<size_t>(icon.pixelW) * 4));
    if (!image) return;
    canvas->drawImageRect(image, SkRect::MakeXYWH(icon.x * sx, icon.y * sy, icon.w * sx, icon.h * sy),
                          SkSamplingOptions(SkFilterMode::kLinear), nullptr);
#endif
}

// A client's cursor picture drawn into the frame, its hotspot on the
// pointer: under DRM where the cursor plane cannot take it, and in the
// headless shell host (no plane), where a screenshot shows it. Part of the
// frame's key (picture, hotspot, place).
bool Engine::drawClientCursorIntoFrame() {
#if BRO_WITH_COMPOSITOR
    if (!cursorVisible_ || lockedElement_.get()) return false;
    compositor::WaylandCompositor::CursorPixels cur;
    if (!clientCursor(drmCtx_.get(), isShellApp(), cur)) return false;
    SkCanvas* canvas = frameSegmentCanvas();
    if (!canvas) return false;
    const SkImageInfo info = SkImageInfo::Make(cur.pixelW, cur.pixelH, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    sk_sp<SkImage> image =
        SkImages::RasterFromPixmapCopy(SkPixmap(info, cur.bgra->data(), static_cast<size_t>(cur.pixelW) * 4));
    if (!image) return false;
    const float sx = static_cast<float>(frameCompositeW_) / static_cast<float>(viewportWidth_ > 0 ? viewportWidth_ : 1);
    const float sy = static_cast<float>(frameCompositeH_) / static_cast<float>(viewportHeight_ > 0 ? viewportHeight_ : 1);
    const float x = lastMouseX_ - cur.hotX, y = lastMouseY_ - cur.hotY;
    frameKeyAdd(0xc0c5u ^ cur.key);
    frameKeyAdd(static_cast<uint64_t>(std::lround(x * 16.0f)) << 32 ^ static_cast<uint32_t>(std::lround(y * 16.0f)));
    canvas->drawImageRect(image, SkRect::MakeXYWH(x * sx, y * sy, cur.w * sx, cur.h * sy),
                          SkSamplingOptions(SkFilterMode::kNearest), nullptr);
    return true;
#else
    return false;
#endif
}

}  // namespace bro::engine
