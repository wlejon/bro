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
#include "compositor/wayland_compositor.h"
#include <brocompositor/api.h>
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

#if BRO_WITH_COMPOSITOR
    compositor::CompositorConfig compCfg;
    compCfg.headless = true;
    compCfg.drm = false;
    compCfg.width = viewportWidth_;
    compCfg.height = viewportHeight_;
    compCfg.xwayland = true;
    compCfg.socketName = "wayland-0";

    drmCtx_->compositor = std::make_unique<compositor::WaylandCompositor>();
    std::string compErr;
    if (drmCtx_->compositor->init(compCfg, &compErr)) {
        LOG_INFO("Engine: WaylandCompositor started on socket %s",
                 drmCtx_->compositor->socketName().c_str());
        ::setenv("WAYLAND_DISPLAY", drmCtx_->compositor->socketName().c_str(), 1);
        if (!drmCtx_->compositor->xwaylandDisplay().empty()) {
            ::setenv("DISPLAY", drmCtx_->compositor->xwaylandDisplay().c_str(), 1);
        }
#if BRO_HAVE_WAYLAND_SERVER
        if (drmCtx_->compositor->windowManager()) {
            brocompositor::api::setWindowManager(drmCtx_->compositor->windowManagerShared());
            brocompositor::api::setCommandSink([comp = drmCtx_->compositor.get()](const std::vector<brocompositor::Command>& cmds) -> size_t {
                if (comp && comp->backend()) return comp->backend()->execute(cmds);
                return cmds.size();
            });
        }
#endif
    } else {
        LOG_WARN("Engine: WaylandCompositor init failed: %s", compErr.c_str());
    }
#endif
#else
    (void)config;
    throw std::runtime_error("DRM display mode is only supported on Linux with seat and dmabuf enabled");
#endif
}

} // namespace bro::engine
