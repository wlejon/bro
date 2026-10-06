#include "compositor/wayland_compositor.h"
#include "engine/ui_layer.h"
#include "render/layer_source.h"

namespace bro::compositor {

WaylandCompositor::WaylandCompositor() = default;

WaylandCompositor::~WaylandCompositor() {
    shutdown();
}

bool WaylandCompositor::init(const CompositorConfig& config, std::string* error) {
#if defined(__linux__) && defined(BRO_WITH_COMPOSITOR)
    brocompositor::wl::ServerConfig sCfg;
    if (config.headless) {
        sCfg.backend = brocompositor::wl::BackendKind::Headless;
    } else if (config.drm) {
        sCfg.backend = brocompositor::wl::BackendKind::Drm;
    } else {
        sCfg.backend = brocompositor::wl::BackendKind::Auto;
    }
    sCfg.initial_outputs = 1;
    sCfg.initial_output_size = {static_cast<int32_t>(config.width), static_cast<int32_t>(config.height)};
    sCfg.socket_name = config.socketName;
    sCfg.render_node = config.renderNode;

    backend_ = brocompositor::wl::ServerBackend::create(sCfg, error);
    if (!backend_) return false;

    wm_ = std::make_unique<brocompositor::WindowManager>();
    socketName_ = backend_->socket_name();
    running_ = true;
    return true;
#else
    if (error) *error = "brocompositor is not available on this platform or build";
    return false;
#endif
}

void WaylandCompositor::shutdown() {
#if defined(__linux__) && defined(BRO_WITH_COMPOSITOR)
    wm_.reset();
    backend_.reset();
#endif
    running_ = false;
    socketName_.clear();
}

bool WaylandCompositor::isRunning() const {
    return running_;
}

std::string WaylandCompositor::socketName() const {
    return socketName_;
}

void WaylandCompositor::pollEvents() {
#if defined(__linux__) && defined(BRO_WITH_COMPOSITOR)
    if (!backend_) return;

    auto events = backend_->events().drain();
    for (auto& ev : events) {
        if (wm_) {
            auto cmd = wm_->handle(ev);
            backend_->execute(cmd);
        }
    }

    auto sevents = backend_->server_events().drain();
    (void)sevents;
#endif
}

std::vector<LeasedSurfaceFrame> WaylandCompositor::acquireClientLayers(std::vector<engine::UILayer>& outLayers) {
#if defined(__linux__) && defined(BRO_WITH_COMPOSITOR)
    if (!backend_) return {};

    std::vector<LeasedSurfaceFrame> leasedFrames;
    auto windows = backend_->windows();
    for (auto winId : windows) {
        if (!backend_->visible(winId)) continue;
        auto optSnap = backend_->query(winId);
        auto surfaceNodes = backend_->window_surfaces(winId);
        for (const auto& node : surfaceNodes) {
            auto surface = backend_->surface(node.surface);
            if (!surface) continue;

            auto frameOpt = surface->acquire();
            if (!frameOpt) continue;

            auto imgOpt = surface->image(frameOpt->image_id);
            if (!imgOpt) {
                surface->release(*frameOpt);
                continue;
            }

            if (imgOpt->type == brocompositor::ImageHandleType::DmaBuf) {
                render::DmabufLayerSource dmabufSrc;
                dmabufSrc.bufferId = imgOpt->id;
                dmabufSrc.width = imgOpt->width;
                dmabufSrc.height = imgOpt->height;
                dmabufSrc.drmFormat = imgOpt->drm_format;
                dmabufSrc.modifier = imgOpt->drm_modifier;
                dmabufSrc.planeCount = static_cast<uint32_t>(imgOpt->planes.size());
                for (size_t p = 0; p < imgOpt->planes.size() && p < 4; ++p) {
                    dmabufSrc.fds[p] = brocompositor::wl::fd_of(imgOpt->planes[p].handle);
                    dmabufSrc.strides[p] = imgOpt->planes[p].stride;
                    dmabufSrc.offsets[p] = imgOpt->planes[p].offset;
                }
                dmabufSrc.syncFd = brocompositor::wl::fd_of(frameOpt->sync_fd);

                engine::UILayer layer;
                float wx = optSnap ? static_cast<float>(optSnap->frame.x) : 0.0f;
                float wy = optSnap ? static_cast<float>(optSnap->frame.y) : 0.0f;
                layer.quad.x = wx + static_cast<float>(node.offset.x);
                layer.quad.y = wy + static_cast<float>(node.offset.y);
                layer.quad.w = static_cast<float>(imgOpt->width);
                layer.quad.h = static_cast<float>(imgOpt->height);
                layer.quad.clipW = -1.0f;
                layer.content = dmabufSrc;

                outLayers.push_back(layer);

                LeasedSurfaceFrame leased;
                leased.surfaceId = static_cast<uint32_t>(node.surface);
                leased.surface = surface;
                leased.frame = *frameOpt;
                leasedFrames.push_back(std::move(leased));
            } else {
                surface->release(*frameOpt);
            }
        }
    }
    return leasedFrames;
#else
    (void)outLayers;
    return {};
#endif
}

void WaylandCompositor::releaseClientLayers(const std::vector<LeasedSurfaceFrame>& frames) {
#if defined(__linux__) && defined(BRO_WITH_COMPOSITOR)
    for (const auto& lf : frames) {
        if (lf.surface) {
            lf.surface->presented_on(brocompositor::kNoMonitor, 0);
            lf.surface->release(lf.frame);
        }
    }
#else
    (void)frames;
#endif
}

} // namespace bro::compositor
