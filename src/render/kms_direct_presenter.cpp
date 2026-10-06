#include "render/kms_direct_presenter.h"
#include <unistd.h>

namespace bro::render {

bool KmsDirectPresenter::init(int drmFd) {
#if defined(__linux__)
    if (drmFd < 0) return false;
    drmFd_ = drmFd;
    auto devRes = brodmabuf::KmsDevice::wrap_fd(brodmabuf::UniqueFd(::dup(drmFd)));
    if (!devRes) {
        active_ = false;
        return false;
    }
    device_ = std::move(devRes.value());
    auto pipeRes = device_->find_default_pipeline();
    if (!pipeRes) {
        active_ = false;
        return false;
    }
    auto presRes = brodmabuf::KmsPresenter::create(device_, pipeRes.value());
    if (!presRes) {
        active_ = false;
        return false;
    }
    presenter_ = std::move(presRes.value());
    active_ = (presenter_ != nullptr);
    return active_;
#else
    (void)drmFd;
    return false;
#endif
}

bool KmsDirectPresenter::canDirectScanout(
    const DmabufLayerSource& src, const LayerQuad& quad,
    uint32_t crtcWidth, uint32_t crtcHeight) const {
    if (!active_) return false;
    if (quad.clipped()) return false;
    if (quad.x != 0.0f || quad.y != 0.0f) return false;
    if (static_cast<uint32_t>(quad.w) != crtcWidth ||
        static_cast<uint32_t>(quad.h) != crtcHeight) {
        return false;
    }
    if (src.width != crtcWidth || src.height != crtcHeight) {
        return false;
    }
    return true;
}

bool KmsDirectPresenter::directScanout(
    const DmabufLayerSource& src, int inFenceFd, int* outFenceFd) {
#if defined(__linux__)
    if (!active_ || !presenter_ || !device_) return false;

    brodmabuf::DmaBufAttributes attrs;
    attrs.width = src.width;
    attrs.height = src.height;
    attrs.drm_format = src.drmFormat;
    attrs.modifier = src.modifier;
    for (uint32_t i = 0; i < src.planeCount && i < 4; ++i) {
        if (src.fds[i] >= 0) {
            attrs.planes.emplace_back(
                brodmabuf::UniqueFd(::dup(src.fds[i])),
                src.strides[i],
                src.offsets[i]);
        }
    }

    auto fbRes = brodmabuf::KmsFramebuffer::create_from_dmabuf(device_->fd(), attrs);
    if (!fbRes) return false;

    auto flipRes = presenter_->present(*fbRes.value(), inFenceFd, false);
    if (!flipRes) return false;

    if (outFenceFd) {
        *outFenceFd = flipRes.value().release();
    }
    return true;
#else
    (void)src;
    (void)inFenceFd;
    (void)outFenceFd;
    return false;
#endif
}

void KmsDirectPresenter::close() {
#if defined(__linux__)
    presenter_.reset();
    device_.reset();
#endif
    active_ = false;
    drmFd_ = -1;
}

} // namespace bro::render
