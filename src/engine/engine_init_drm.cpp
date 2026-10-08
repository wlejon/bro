// DRM display mode setup: seat, KMS presenter, libinput and the Wayland
// compositor clients run under. The frame loop is engine_run_drm.cpp, input
// routing engine_drm_input.cpp.
#include "engine/engine.h"
#include "engine/engine_drm.h"
#include "platform/drm_seat.h"
#include "platform/drm_input.h"
#include "render/vulkan_context.h"
#include "render/vulkan_presenter.h"
#if BRO_WITH_DMABUF
#include "render/kms_direct_presenter.h"
#endif
#if BRO_WITH_COMPOSITOR
#include "compositor/wayland_compositor.h"  // DrmPlatformContext owns one
#endif
#include "util/log.h"

#include <stdexcept>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(__linux__) && BRO_WITH_DMABUF
#include <brodmabuf/gbm.h>
#endif

namespace bro::engine {

DrmPlatformContext::DrmPlatformContext() = default;
DrmPlatformContext::~DrmPlatformContext() = default;

void Engine::initDrm(const EngineConfig& config) {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
    drmCtx_ = std::make_unique<DrmPlatformContext>();
    drmCtx_->seat = std::make_unique<platform::DrmSeatPlatform>();
    if (!drmCtx_->seat->initSeat("seat0")) {
        LOG_WARN("Engine: could not initialize seat0, falling back to direct DRM device access");
    }

    std::string cardNode = brodmabuf::find_card_node();
    int cardFd = -1;
    if (drmCtx_->seat && !cardNode.empty()) {
        cardFd = drmCtx_->seat->openDevice(cardNode);
    }
    if (cardFd < 0 && !cardNode.empty()) {
        cardFd = ::open(cardNode.c_str(), O_RDWR | O_CLOEXEC);
    }
    if (cardFd < 0) {
        throw std::runtime_error("DRM initialization failed: cannot open DRM card node " + cardNode);
    }

    render::VulkanContextConfig vkCfg;
    vkCfg.headless = true;
    vulkanContext_ = std::make_unique<render::VulkanContext>(vkCfg);
    if (!vulkanContext_->init()) {
        throw std::runtime_error("DRM VulkanContext initialization failed");
    }

    vulkanPresenter_ = std::make_unique<render::VulkanPresenter>(*vulkanContext_);
    if (!vulkanPresenter_->init()) {
        throw std::runtime_error("DRM VulkanPresenter initialization failed");
    }

    if (!vulkanPresenter_->initKms(cardFd)) {
        LOG_WARN("Engine: VulkanPresenter::initKms failed on card %s", cardNode.c_str());
    }

    viewportWidth_ = vulkanPresenter_->width() ? vulkanPresenter_->width() : static_cast<uint32_t>(config.graphics.width);
    viewportHeight_ = vulkanPresenter_->height() ? vulkanPresenter_->height() : static_cast<uint32_t>(config.graphics.height);

    drmCtx_->input = std::make_unique<platform::DrmInputPlatform>();
    drmCtx_->input->init(*drmCtx_->seat, "seat0", viewportWidth_, viewportHeight_);

    // Hook VT switch callbacks
    drmCtx_->seat->setActiveChangeCallback([this](bool active) {
        if (!active) {
            LOG_INFO("Engine: VT switched away, pausing KMS presentation");
            if (vulkanPresenter_ && vulkanPresenter_->kmsDirectPresenter()) {
                vulkanPresenter_->kmsDirectPresenter()->pause();
            }
        } else {
            LOG_INFO("Engine: VT switched back, restoring KMS modeset");
            if (vulkanPresenter_ && vulkanPresenter_->kmsDirectPresenter()) {
                vulkanPresenter_->kmsDirectPresenter()->restoreModeset();
            }
        }
    });

    auto skia = std::make_unique<render::SkiaRenderer>();
    if (vulkanPresenter_) skia->setGpu(createSkiaGpu());
    renderer_ = std::move(skia);

    LOG_INFO("Engine: DRM display mode initialized (%ux%u)", viewportWidth_, viewportHeight_);
    installScreenCaptureSignal();
    LOG_INFO("Engine: screen capture: kill -USR2 %d (to %s), or write a path into %s",
             static_cast<int>(::getpid()), defaultScreenCapturePath().c_str(), screenCaptureRequestPath().c_str());

#if BRO_WITH_COMPOSITOR
    startShellCompositor(viewportWidth_, viewportHeight_, "wayland-0", /*xwayland=*/true);
#endif
#else
    (void)config;
    throw std::runtime_error("DRM display mode is only supported on Linux with seat and dmabuf enabled");
#endif
}

} // namespace bro::engine
