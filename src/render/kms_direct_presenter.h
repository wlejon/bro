#pragma once

#include "render/layer_source.h"

#include <cstdint>
#include <memory>

#if defined(__linux__)
#include <brodmabuf/kms.h>
#include <brodmabuf/buffer.h>
#endif

namespace bro::render {

class KmsDirectPresenter {
public:
    KmsDirectPresenter() = default;
    ~KmsDirectPresenter() = default;

    KmsDirectPresenter(const KmsDirectPresenter&) = delete;
    KmsDirectPresenter& operator=(const KmsDirectPresenter&) = delete;

    /// Initialize direct scanout with DRM device file descriptor
    bool init(int drmFd);

    /// Check if a client DMA-BUF buffer can be directly scanned out without GPU compositing.
    /// Conditions:
    /// 1. KMS presenter is valid and active.
    /// 2. The layer destination matches output CRTC bounds exactly (no scaling, no rotation, at (0,0)).
    /// 3. The layer is not clipped or obscured by surrounding UI.
    bool canDirectScanout(const DmabufLayerSource& src, const LayerQuad& quad,
                          uint32_t crtcWidth, uint32_t crtcHeight) const;

    /// Present a client DMA-BUF directly via KMS atomic modesetting.
    bool directScanout(const DmabufLayerSource& src, int inFenceFd = -1, int* outFenceFd = nullptr);

    bool isActive() const { return active_; }
    void close();

private:
    bool active_ = false;
    int drmFd_ = -1;
#if defined(__linux__)
    std::shared_ptr<brodmabuf::KmsDevice> device_;
    std::unique_ptr<brodmabuf::KmsPresenter> presenter_;
#endif
};

} // namespace bro::render
