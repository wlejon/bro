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

bool Engine::drmPlaceHardwareCursor() {
#if BRO_WITH_DMABUF
    auto* kms = vulkanPresenter_ ? vulkanPresenter_->kmsDirectPresenter() : nullptr;
    if (!kms) return false;
    if (!kms->hasCursorPlane() || kms->cursorRefused()) {
        kms->setCursor(false, 0, 0);
        return false;
    }
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
    const float sx = static_cast<float>(frameCompositeW_) / static_cast<float>(viewportWidth_ > 0 ? viewportWidth_ : 1);
    const float sy = static_cast<float>(frameCompositeH_) / static_cast<float>(viewportHeight_ > 0 ? viewportHeight_ : 1);
    canvas->drawImageRect(image, SkRect::MakeXYWH(icon.x * sx, icon.y * sy, icon.w * sx, icon.h * sy),
                          SkSamplingOptions(SkFilterMode::kLinear), nullptr);
#endif
}

}  // namespace bro::engine
