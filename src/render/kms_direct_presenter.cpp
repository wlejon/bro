#include "render/kms_direct_presenter.h"
#include "render/vulkan_context.h"
#include "render/vulkan_presenter.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <unistd.h>

namespace bro::render {

namespace {
// A scanout slot is drawn into, sampled, cleared, and copied out of (the
// frame tap: the screen recorder).
constexpr VkImageUsageFlags kScanoutUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

struct KmsDirectPresenter::SlotHolds {
    std::mutex m;
    std::condition_variable cv;
    std::array<int, 8> count{};
};

std::function<void()> KmsDirectPresenter::holdSlot(size_t slot) {
    if (!holds_) holds_ = std::make_shared<SlotHolds>();
    if (slot >= holds_->count.size()) return [] {};
    {
        std::lock_guard<std::mutex> lk(holds_->m);
        ++holds_->count[slot];
    }
    return [holds = holds_, slot, released = std::make_shared<std::atomic<bool>>(false)] {
        if (released->exchange(true)) return;
        {
            std::lock_guard<std::mutex> lk(holds->m);
            --holds->count[slot];
        }
        holds->cv.notify_all();
    };
}

bool KmsDirectPresenter::waitForSlotRelease(size_t slot) {
    if (!holds_ || slot >= holds_->count.size()) return true;
    std::unique_lock<std::mutex> lk(holds_->m);
    return holds_->cv.wait_for(lk, std::chrono::milliseconds(kHoldWaitMs),
                               [&] { return holds_->count[slot] == 0; });
}

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

    std::vector<uint64_t> modifiers = {DRM_FORMAT_MOD_LINEAR};
    auto vkMods = dmabufVkCtx_->query_format_modifiers(VK_FORMAT_B8G8R8A8_UNORM);
    for (const auto& m : vkMods) {
        if (m.tiling_features & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
            modifiers.push_back(m.modifier);
        }
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
            kScanoutUsage);
        if (!vk) continue;
        brodmabuf::KmsAtomicReq testReq;
        const auto& pipe = presenter_->pipeline();
        testReq.add_property(pipe.connector_id, pipe.connector_props.crtc_id, pipe.crtc_id);
        testReq.add_property(pipe.crtc_id, pipe.crtc_props.active, 1);
        testReq.set_plane(
            pipe.plane_props, pipe.plane_id, pipe.crtc_id, fb.value()->fb_id(),
            0, 0, width_, height_,
            0, 0, width_, height_);
        auto msTest = testReq.commit(device_->fd(), DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET);
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
            kScanoutUsage);
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

        slot.dmabuf = std::move(attrs);
        scanoutSlots_.push_back(std::move(slot));
    }

    if (scanoutSlots_.empty()) return false;

    // Clear all scanout slots to opaque black so the monitor never shows uninitialized RAM
    auto& frames = ctx.frames();
    frames.ensureFrame();
    VkCommandBuffer initCmd = frames.beginCommands();
    if (initCmd != VK_NULL_HANDLE) {
        for (auto& s : scanoutSlots_) {
            ImageBarrier b;
            b.image = s.vkImage->handle();
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcStages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            b.dstStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
            b.dstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
            cmdImageBarrier(initCmd, b);

            VkClearColorValue black{};
            black.float32[0] = 0.0f;
            black.float32[1] = 0.0f;
            black.float32[2] = 0.0f;
            black.float32[3] = 1.0f;
            const VkImageSubresourceRange range = colorRange();
            vkCmdClearColorImage(initCmd, s.vkImage->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
            b.srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            b.dstAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            cmdImageBarrier(initCmd, b);
        }
        uint64_t t = frames.submit(initCmd, {});
        ctx.queue().wait(t);
    }

    // Attach initial modeset commit
    auto msRes = presenter_->initialize_modeset(*scanoutSlots_[0].fb);
    if (!msRes) {
        std::string err(msRes.status().message());
        LOG_WARN("KmsDirectPresenter: initialize_modeset: %s", err.c_str());
    }
    cursorShown_ = {};

    // The cursor plane's buffers. Without them the cursor is drawn into the
    // frame as before.
    if (presenter_->has_cursor_plane()) {
        const auto& pipe = presenter_->pipeline();
        for (auto& b : cursorBufs_) {
            auto r = brodmabuf::KmsDumbBuffer::create(device_->fd(), pipe.cursor_width, pipe.cursor_height);
            if (!r) {
                LOG_WARN("KmsDirectPresenter: no cursor buffer: %s", std::string(r.status().message()).c_str());
                cursorBufs_[0].reset();
                cursorBufs_[1].reset();
                break;
            }
            b = std::move(r.value());
        }
        if (cursorBufs_[0])
            LOG_INFO("KmsDirectPresenter: cursor plane %u, %ux%u", pipe.cursor_plane_id, pipe.cursor_width,
                     pipe.cursor_height);
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
    if (!active_ || paused_ || directScanoutInhibited_) return false;
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
    if (flipPending_) waitForFlip(100);  // one commit in flight at a time

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

    stageCursor();
    auto flipRes = presenter_->present(*fbRes.value(), inFenceFd, false);
    if (!flipRes) return false;
    cursorCommitted();
    // The framebuffer must outlive its time on screen: removing one that a
    // plane scans out disables the plane (and, on amdgpu, the CRTC), after
    // which every commit without a modeset is refused. Kept until a
    // composited flip has replaced it, the last few at most.
    directFbs_.push_back(std::move(fbRes.value()));
    if (directFbs_.size() > 3) directFbs_.erase(directFbs_.begin());
    directOnScreen_ = true;
    flipPending_ = true;

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
    if (!active_ || paused_ || !presenter_ || scanoutSlots_.empty()) {
        static bool s_logged = false;
        if (!s_logged) {
            LOG_WARN("KmsDirectPresenter: presentComposited inactive (active=%d paused=%d presenter=%d slots=%zu)",
                     active_, paused_, presenter_ != nullptr, scanoutSlots_.size());
            s_logged = true;
        }
        return false;
    }

    // The slot drawn into next is the one on screen until the last flip
    // lands (and the frame tap's one copy is that flip's until it does).
    lastTiming_.flipWaitMs = 0.0;
    if (flipPending_) {
        const double t = nowMs();
        waitForFlip(100);
        lastTiming_.flipWaitMs = nowMs() - t;
    }

    currentSlot_ = (currentSlot_ + 1) % scanoutSlots_.size();
    auto& slot = scanoutSlots_[currentSlot_];

    // A reader (the remote encoder) may still be copying this slot out.
    // That copy takes well under a frame; the bound is for an encoder that
    // is being set up or has stalled.
    if (!waitForSlotRelease(currentSlot_)) {
        static uint32_t s_heldLogCount = 0;
        if (s_heldLogCount++ < 5) {
            LOG_WARN("KmsDirectPresenter: scanout slot %zu still held after %d ms; drawing into it",
                     currentSlot_, kHoldWaitMs);
        }
    }

    static uint32_t s_frameLogCount = 0;
    if (s_frameLogCount++ < 5) {
        LOG_INFO("KmsDirectPresenter: presentComposited slot=%zu images=%zu below=%d clear=[%.1f,%.1f,%.1f,%.1f] dims=%ux%u",
                 currentSlot_, frame.images.size(), frame.below ? 1 : 0,
                 frame.clearColor[0], frame.clearColor[1], frame.clearColor[2], frame.clearColor[3],
                 frame.width, frame.height);
    }

    auto& frames = ctx.frames();
    frames.ensureFrame();

    VkCommandBuffer cmd = frames.beginCommands();
    if (cmd == VK_NULL_HANDLE) {
        LOG_WARN("KmsDirectPresenter: beginCommands returned null");
        return false;
    }

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
        LOG_WARN("KmsDirectPresenter: recordFrame failed");
        return false;
    }

    cmdTransitionImage(cmd, target.image, colorRange(), layout, VK_IMAGE_LAYOUT_GENERAL);
    if (frameTap_) frameTap_->record(cmd, target.image, slot.width, slot.height);

    uint64_t ticket = frames.submit(cmd, {});
    const double tWait = nowMs();
    ctx.queue().wait(ticket);
    const double tFlip = nowMs();
    lastTiming_.gpuWaitMs = tFlip - tWait;

    // The slot's GPU work is done, so its content is complete: hand it to a
    // reader (the remote encoder) now, before the flip. After the flip the
    // frame would first wait for the next vblank, most of a refresh period,
    // before it could be encoded. The reader needs no acquire fence (were
    // the CPU wait above ever replaced by a fence on the flip, the
    // render-done sync_file would have to be exported for the reader too),
    // and its hold keeps the slot from being drawn into again whatever the
    // flip does.
    if (scanoutListener_) {
        KmsScanoutFrame out;
        out.slot = currentSlot_;
        out.width = slot.width;
        out.height = slot.height;
        out.drmFormat = slot.dmabuf.drm_format;
        out.modifier = slot.dmabuf.modifier;
        out.planeCount = static_cast<uint32_t>(std::min<size_t>(slot.dmabuf.planes.size(), 4));
        for (uint32_t i = 0; i < out.planeCount; ++i) {
            out.fds[i] = slot.dmabuf.planes[i].fd.get();
            out.offsets[i] = slot.dmabuf.planes[i].offset;
            out.strides[i] = slot.dmabuf.planes[i].stride;
        }
        scanoutListener_(out);
    }

    stageCursor();
    auto flipRes = presenter_->present(*slot.fb, inFenceFd, true);
    if (!flipRes) {
        static uint32_t s_flipFailCount = 0;
        if (s_flipFailCount++ < 5) {
            LOG_WARN("KmsDirectPresenter: presenter_->present failed: %s",
                     std::string(flipRes.status().message()).c_str());
        }
        return false;
    }
    cursorCommitted();

    // The flip lands at the next vblank; the frame loop waits for it (and
    // handles input meanwhile), and the next present waits for it before it
    // draws. Its landing does the rest (readEvents): the direct scanout
    // buffers it replaced go, and the frame tap gets its frame.
    flipPending_ = true;
    compositedFlipPending_ = true;
    lastTiming_.flipWaitMs += nowMs() - tFlip;
    composited_ = true;
    directOnScreen_ = false;

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

bool KmsDirectPresenter::readLastFrame(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height,
                                       std::string* why) {
    auto fail = [&](const char* reason) {
        if (why) *why = reason;
        return false;
    };
#if defined(__linux__)
    if (!active_ || scanoutSlots_.empty()) return fail("KMS scanout is not active");
    if (!composited_) return fail("no composited frame has been presented yet");
    if (directOnScreen_)
        return fail("a client buffer is scanned out directly; there is no composited frame on screen");
    auto& slot = scanoutSlots_[currentSlot_];
    if (!slot.gbm) return fail("the scanout slot has no buffer");
    // presentComposited waited for this slot's GPU work before flipping, so
    // its contents are complete; GBM linearises a tiled buffer on map.
    auto mapping = slot.gbm->map(0, 0, slot.width, slot.height, GBM_BO_TRANSFER_READ);
    if (!mapping || !mapping->valid()) return fail("mapping the scanout buffer failed");
    width = slot.width;
    height = slot.height;
    rgba.resize(static_cast<size_t>(width) * height * 4);
    const uint8_t* src = static_cast<const uint8_t*>(mapping->data());
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * mapping->stride();
        uint8_t* out = rgba.data() + static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            out[x * 4 + 0] = row[x * 4 + 2];
            out[x * 4 + 1] = row[x * 4 + 1];
            out[x * 4 + 2] = row[x * 4 + 0];
            out[x * 4 + 3] = 255;
        }
    }
    return true;
#else
    (void)rgba;
    (void)width;
    (void)height;
    return fail("KMS scanout is Linux-only");
#endif
}

bool KmsDirectPresenter::handlePageFlipEvent(int timeoutMs) {
#if defined(__linux__)
    if (!active_ || !presenter_) return false;
    return readEvents(timeoutMs);
#else
    (void)timeoutMs;
    return false;
#endif
}

bool KmsDirectPresenter::waitForFlip(int timeoutMs) {
#if defined(__linux__)
    if (!presenter_) return false;
    const double until = nowMs() + timeoutMs;
    while (flipPending_) {
        const double left = until - nowMs();
        if (left <= 0.0) break;
        (void)readEvents(static_cast<int>(left) + 1);
    }
    return !flipPending_;
#else
    (void)timeoutMs;
    return true;
#endif
}

int KmsDirectPresenter::pollFd() const {
#if defined(__linux__)
    return device_ ? device_->fd() : -1;
#else
    return -1;
#endif
}

bool KmsDirectPresenter::readEvents(int timeoutMs) {
#if defined(__linux__)
    brodmabuf::KmsFlipEvent flip;
    bool gotFlip = false;
    const bool any = presenter_->handle_event(timeoutMs, &flip, &gotFlip);
    if (gotFlip) {
        flipPending_ = false;
        lastFlip_.vblankMs = static_cast<double>(flip.timestamp_us) / 1000.0;
        lastFlip_.sequence = flip.sequence;
        ++lastFlip_.count;
        if (compositedFlipPending_) {
            compositedFlipPending_ = false;
            directFbs_.clear();  // a composited frame is on screen: no direct buffer is
            if (frameTap_) frameTap_->completed(lastFlip_);
        }
        if (flipListener_) flipListener_(lastFlip_);
    }
    return any;
#else
    (void)timeoutMs;
    return false;
#endif
}

double KmsDirectPresenter::refreshPeriodMs() const {
#if defined(__linux__)
    return presenter_ ? presenter_->refresh_period_ms() : 0.0;
#else
    return 0.0;
#endif
}

bool KmsDirectPresenter::hasCursorPlane() const {
#if defined(__linux__)
    return active_ && presenter_ && presenter_->has_cursor_plane() && cursorBufs_[0] && cursorBufs_[1];
#else
    return false;
#endif
}

uint32_t KmsDirectPresenter::cursorWidth() const {
#if defined(__linux__)
    return cursorBufs_[0] ? cursorBufs_[0]->width() : 0;
#else
    return 0;
#endif
}

uint32_t KmsDirectPresenter::cursorHeight() const {
#if defined(__linux__)
    return cursorBufs_[0] ? cursorBufs_[0]->height() : 0;
#else
    return 0;
#endif
}

bool KmsDirectPresenter::setCursorImage(const uint8_t* bgra, uint32_t w, uint32_t h, size_t stride) {
#if defined(__linux__)
    if (!hasCursorPlane() || !bgra || w > cursorWidth() || h > cursorHeight()) return false;
    // Into the buffer not on screen; the next commit swaps them.
    const int next = cursorBufs_[0]->fb_id() == cursorShown_.fb_id ? 1 : 0;
    brodmabuf::KmsDumbBuffer& buf = *cursorBufs_[next];
    std::memset(buf.pixels(), 0, static_cast<size_t>(buf.stride()) * buf.height());
    for (uint32_t y = 0; y < h; ++y) std::memcpy(buf.pixels() + static_cast<size_t>(y) * buf.stride(), bgra + y * stride, w * 4);
    cursorBuf_ = next;
    cursorRefused_ = false;
    if (cursorWant_.fb_id != 0) cursorWant_.fb_id = buf.fb_id();
    return true;
#else
    (void)bgra;
    (void)w;
    (void)h;
    (void)stride;
    return false;
#endif
}

void KmsDirectPresenter::setCursor(bool visible, int32_t x, int32_t y) {
#if defined(__linux__)
    if (!visible || cursorBuf_ < 0 || !hasCursorPlane()) {
        cursorWant_ = {};
        return;
    }
    const auto& b = *cursorBufs_[cursorBuf_];
    cursorWant_ = brodmabuf::KmsCursor{b.fb_id(), x, y, b.width(), b.height()};
#else
    (void)visible;
    (void)x;
    (void)y;
#endif
}

bool KmsDirectPresenter::cursorChanged() const {
#if defined(__linux__)
    return !(cursorWant_ == cursorShown_);
#else
    return false;
#endif
}

bool KmsDirectPresenter::cursorRefused() const { return cursorRefused_; }

void KmsDirectPresenter::stageCursor() {
#if defined(__linux__)
    if (presenter_) presenter_->set_cursor(cursorRefused_ ? brodmabuf::KmsCursor{} : cursorWant_);
#endif
}

void KmsDirectPresenter::cursorCommitted() {
#if defined(__linux__)
    if (presenter_ && presenter_->cursor_refused() && cursorWant_.fb_id != 0) {
        if (!cursorRefused_) LOG_INFO("KmsDirectPresenter: the driver refused the cursor plane; drawing the cursor");
        cursorRefused_ = true;
    }
    cursorShown_ = cursorRefused_ ? brodmabuf::KmsCursor{} : cursorWant_;
    cursorOnlyFlip_ = false;
#endif
}

bool KmsDirectPresenter::presentCursorOnly() {
#if defined(__linux__)
    if (!active_ || paused_ || !hasCursorPlane()) return false;
    if (flipPending_) waitForFlip(100);
    if (flipPending_) return false;
    stageCursor();
    auto r = presenter_->commit_cursor();
    if (!r) {
        static uint32_t s_failCount = 0;
        if (s_failCount++ < 5)
            LOG_WARN("KmsDirectPresenter: cursor commit failed: %s", std::string(r.status().message()).c_str());
        return false;
    }
    cursorCommitted();
    cursorOnlyFlip_ = true;
    flipPending_ = true;
    return true;
#else
    return false;
#endif
}

bool KmsDirectPresenter::restoreModeset() {
#if defined(__linux__)
    if (!active_ || !presenter_ || scanoutSlots_.empty()) return false;
    paused_ = false;
    auto msRes = presenter_->initialize_modeset(*scanoutSlots_[currentSlot_].fb);
    cursorShown_ = {};  // the modeset turned the cursor plane off
    return msRes.ok();
#else
    return false;
#endif
}

void KmsDirectPresenter::pause() {
    paused_ = true;
    flipPending_ = false;  // a VT switch can swallow the event
    compositedFlipPending_ = false;
}

void KmsDirectPresenter::close() {
#if defined(__linux__)
    for (size_t i = 0; i < scanoutSlots_.size(); ++i) {
        if (!waitForSlotRelease(i))
            LOG_WARN("KmsDirectPresenter: closing with scanout slot %zu still held", i);
    }
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
    cursorBufs_[0].reset();
    cursorBufs_[1].reset();
    cursorBuf_ = -1;
    cursorWant_ = {};
    cursorShown_ = {};
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
