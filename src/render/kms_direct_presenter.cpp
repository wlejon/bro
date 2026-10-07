#include "render/kms_direct_presenter.h"
#include "render/vulkan_context.h"
#include "render/vulkan_presenter.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <unistd.h>

namespace bro::render {

KmsDirectPresenter::~KmsDirectPresenter() {
    close();
}

bool KmsDirectPresenter::init(int drmFd) {
#if defined(__linux__)
    if (drmFd < 0) return false;
    close();
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
    width_ = presenter_->pipeline().mode.hdisplay;
    height_ = presenter_->pipeline().mode.vdisplay;
    active_ = (presenter_ != nullptr);
    return active_;
#else
    (void)drmFd;
    return false;
#endif
}

bool KmsDirectPresenter::initScanoutBuffers(VulkanContext& ctx, uint32_t count) {
#if defined(__linux__)
    if (!active_ || !device_ || !presenter_ || drmFd_ < 0) return false;
    if (width_ == 0 || height_ == 0) return false;
    if (count == 0) count = 2;

    vkDevice_ = ctx.device();
    auto gbmRes = brodmabuf::GbmDevice::wrap_fd(brodmabuf::UniqueFd(::dup(drmFd_)));
    if (!gbmRes) {
        LOG_WARN("KmsDirectPresenter: GbmDevice::wrap_fd failed on DRM fd %d", drmFd_);
        return false;
    }
    gbmDevice_ = std::move(gbmRes.value());

    dmabufVkCtx_ = brodmabuf::VulkanContext::wrap(
        ctx.instance(), ctx.physicalDevice(), ctx.device(),
        ctx.graphicsQueue(), static_cast<uint32_t>(ctx.queueFamilies().graphicsFamily));
    if (!dmabufVkCtx_) {
        LOG_WARN("KmsDirectPresenter: brodmabuf::VulkanContext::wrap failed");
        return false;
    }

    std::vector<uint64_t> modifiers;
    auto vkMods = dmabufVkCtx_->query_format_modifiers(VK_FORMAT_B8G8R8A8_UNORM);
    for (const auto& m : vkMods) {
        if (m.tiling_features & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
            modifiers.push_back(m.modifier);
        }
    }
    if (std::find(modifiers.begin(), modifiers.end(), DRM_FORMAT_MOD_LINEAR) == modifiers.end()) {
        modifiers.push_back(DRM_FORMAT_MOD_LINEAR);
    }

    uint64_t workingModifier = DRM_FORMAT_MOD_INVALID;
    for (uint64_t m : modifiers) {
        std::vector<uint64_t> singleMod = {m};
        auto testBo = gbmDevice_->create_buffer_with_modifiers(width_, height_, DRM_FORMAT_XRGB8888, singleMod);
        if (!testBo) continue;
        auto attrs = testBo.value()->export_dmabuf();
        if (!attrs) continue;
        auto fb = brodmabuf::KmsFramebuffer::create_from_dmabuf(device_->fd(), attrs.value());
        if (!fb) continue;
        auto vk = dmabufVkCtx_->import_dmabuf(attrs.value(),
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (!vk) continue;
        auto msTest = presenter_->initialize_modeset(*fb.value());
        if (!msTest) continue;
        workingModifier = m;
        LOG_INFO("KmsDirectPresenter: negotiated scanout modifier 0x%lx", static_cast<unsigned long>(m));
        break;
    }

    scanoutSlots_.clear();
    scanoutSlots_.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        KmsScanoutSlot slot;
        slot.width = width_;
        slot.height = height_;

        brodmabuf::Result<std::unique_ptr<brodmabuf::GbmBuffer>> boRes =
            brodmabuf::Status::unsupported("not tried");
        if (workingModifier != DRM_FORMAT_MOD_INVALID) {
            std::vector<uint64_t> singleMod = {workingModifier};
            boRes = gbmDevice_->create_buffer_with_modifiers(width_, height_, DRM_FORMAT_XRGB8888, singleMod);
        }
        if (!boRes) {
            boRes = gbmDevice_->create_buffer(width_, height_, DRM_FORMAT_XRGB8888,
                                               GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
        }
        if (!boRes) {
            boRes = gbmDevice_->create_buffer(width_, height_, DRM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
        }
        if (!boRes) {
            LOG_WARN("KmsDirectPresenter: gbm create_buffer failed (%ux%u)", width_, height_);
            close();
            return false;
        }
        slot.gbm = std::move(boRes.value());

        auto attrsRes = slot.gbm->export_dmabuf();
        if (!attrsRes) {
            LOG_WARN("KmsDirectPresenter: gbm export_dmabuf failed");
            slot.gbm.reset();
            close();
            return false;
        }
        brodmabuf::DmaBufAttributes attrs = std::move(attrsRes.value());
        if (attrs.modifier == DRM_FORMAT_MOD_INVALID) {
            attrs.modifier = DRM_FORMAT_MOD_LINEAR;
        }

        auto fbRes = brodmabuf::KmsFramebuffer::create_from_dmabuf(device_->fd(), attrs);
        if (!fbRes) {
            LOG_WARN("KmsDirectPresenter: KmsFramebuffer::create_from_dmabuf failed: %s",
                     std::string(fbRes.status().message()).c_str());
            slot.gbm.reset();
            close();
            return false;
        }
        slot.fb = std::move(fbRes.value());

        auto vkImgRes = dmabufVkCtx_->import_dmabuf(
            attrs,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (!vkImgRes) {
            std::string err(vkImgRes.status().message());
            LOG_WARN("KmsDirectPresenter: dmabuf import into Vulkan failed: %s (modifier=0x%lx)",
                     err.c_str(), static_cast<unsigned long>(attrs.modifier));
            slot.vkImage.reset();
            slot.fb.reset();
            slot.gbm.reset();
            close();
            return false;
        }
        slot.vkImage = std::move(vkImgRes.value());

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = slot.vkImage->handle();
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        viewInfo.subresourceRange = colorRange();

        if (vkCreateImageView(vkDevice_, &viewInfo, nullptr, &slot.vkView) != VK_SUCCESS) {
            LOG_WARN("KmsDirectPresenter: vkCreateImageView failed for scanout slot %u", i);
            slot.vkImage.reset();
            slot.fb.reset();
            slot.gbm.reset();
            close();
            return false;
        }

        scanoutSlots_.push_back(std::move(slot));
    }

    if (scanoutSlots_.empty()) return false;

    // Attach initial modeset commit
    auto msRes = presenter_->initialize_modeset(*scanoutSlots_[0].fb);
    if (!msRes) {
        std::string err(msRes.status().message());
        LOG_WARN("KmsDirectPresenter: initialize_modeset: %s", err.c_str());
    }

    currentSlot_ = 0;
    return true;
#else
    (void)ctx;
    (void)count;
    return false;
#endif
}

bool KmsDirectPresenter::canDirectScanout(
    const DmabufLayerSource& src, const LayerQuad& quad,
    uint32_t crtcWidth, uint32_t crtcHeight) const {
    if (!active_ || paused_) return false;
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
    if (!active_ || paused_ || !presenter_ || !device_) return false;

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

bool KmsDirectPresenter::presentComposited(
    VulkanContext& ctx, VulkanPresenter& presenter,
    const PresentFrame& frame, int inFenceFd, int* outFenceFd) {
#if defined(__linux__)
    if (!active_ || paused_ || !presenter_ || scanoutSlots_.empty()) return false;

    currentSlot_ = (currentSlot_ + 1) % scanoutSlots_.size();
    auto& slot = scanoutSlots_[currentSlot_];

    auto& frames = ctx.frames();
    frames.ensureFrame();

    VkCommandBuffer cmd = frames.beginCommands();
    if (cmd == VK_NULL_HANDLE) return false;

    VulkanPresenter::Target target;
    target.image = slot.vkImage->handle();
    target.view = slot.vkView;
    target.format = VK_FORMAT_B8G8R8A8_UNORM;
    target.width = slot.width;
    target.height = slot.height;

    constexpr VkPipelineStageFlags kAcquireStages =
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (!presenter.recordFrame(cmd, frame, target, kAcquireStages, layout)) {
        return false;
    }

    cmdTransitionImage(cmd, target.image, colorRange(), layout, VK_IMAGE_LAYOUT_GENERAL);

    uint64_t ticket = frames.submit(cmd, {});
    ctx.queue().wait(ticket);

    auto flipRes = presenter_->present(*slot.fb, inFenceFd, false);
    if (!flipRes) return false;

    if (outFenceFd) {
        *outFenceFd = flipRes.value().release();
    }
    return true;
#else
    (void)ctx;
    (void)presenter;
    (void)frame;
    (void)inFenceFd;
    (void)outFenceFd;
    return false;
#endif
}

bool KmsDirectPresenter::handlePageFlipEvent(int timeoutMs) {
#if defined(__linux__)
    if (!active_ || !presenter_) return false;
    return presenter_->handle_event(timeoutMs);
#else
    (void)timeoutMs;
    return false;
#endif
}

bool KmsDirectPresenter::restoreModeset() {
#if defined(__linux__)
    if (!active_ || !presenter_ || scanoutSlots_.empty()) return false;
    paused_ = false;
    auto msRes = presenter_->initialize_modeset(*scanoutSlots_[currentSlot_].fb);
    return msRes.ok();
#else
    return false;
#endif
}

void KmsDirectPresenter::pause() {
    paused_ = true;
}

void KmsDirectPresenter::close() {
#if defined(__linux__)
    for (auto& s : scanoutSlots_) {
        if (s.vkView != VK_NULL_HANDLE && vkDevice_ != VK_NULL_HANDLE) {
            vkDestroyImageView(vkDevice_, s.vkView, nullptr);
            s.vkView = VK_NULL_HANDLE;
        }
        s.vkImage.reset();
        s.fb.reset();
        s.gbm.reset();
    }
    scanoutSlots_.clear();
    dmabufVkCtx_.reset();
    gbmDevice_.reset();
    presenter_.reset();
    device_.reset();
    vkDevice_ = VK_NULL_HANDLE;
#endif
    active_ = false;
    paused_ = false;
    drmFd_ = -1;
    width_ = 0;
    height_ = 0;
}

} // namespace bro::render
