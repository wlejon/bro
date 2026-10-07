#include "engine/engine.h"
#include "engine/engine_drm.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"
#include "engine/navmesh_subsystem.h"
#include "engine/replaced_elements.h"
#include "engine/key_mapping.h"
#include "platform/drm_seat.h"
#include "platform/drm_input.h"
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include "render/vulkan_context.h"
#include "render/vulkan_presenter.h"
#if BRO_WITH_DMABUF
#include "render/kms_direct_presenter.h"
#endif
#if BRO_WITH_COMPOSITOR
#include "compositor/wayland_compositor.h"
#include <brocompositor/api.h>
#endif
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "layout/box.h"
#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#if BRO_WITH_3D
#include "scene/scene_graph.h"
#endif
#include "webgl/webgl2_context.h"
#include "util/interrupt.h"
#include "util/log.h"
#include "util/platform.h"
#include "util/time.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
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
            brocompositor::api::setCommandSink([comp = drmCtx_->compositor.get()](const std::vector<brocompositor::Command>& cmds) {
                if (comp && comp->backend()) {
                    comp->backend()->execute(cmds);
                }
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

void Engine::runDrm() {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
    running_ = true;
    windowFocused_ = true;
    if (splashVisible_) splashStartMs_ = util::currentTimeMs();

    auto shellWantsKeyboard = [this](const platform::DrmInputEvent& ev) -> bool {
        if (!isShellApp()) return true;
        if (!document_) return false;

        auto isVisible = [](dom::Element* el) {
            if (!el) return false;
            std::string cls = el->className();
            if (cls.find("hidden") != std::string::npos) return false;
            auto it = el->computedStyle().find("display");
            if (it != el->computedStyle().end() && it->second == "none") return false;
            return true;
        };

        if (isVisible(document_->getElementById("lock-screen"))) return true;
        if (isVisible(document_->getElementById("launcher-modal"))) return true;
        if (isVisible(document_->getElementById("notify-drawer"))) return true;
        if (isVisible(document_->getElementById("popup-volume"))) return true;
        if (isVisible(document_->getElementById("popup-network"))) return true;
        if (isVisible(document_->getElementById("popup-power"))) return true;

        dom::Element* active = document_->activeElement();
        if (active && active != document_->body() && active != document_->documentElement()) {
            const std::string& tag = active->tagName();
            if (tag == "input" || tag == "INPUT" || tag == "textarea" || tag == "TEXTAREA") {
                return true;
            }
        }

        // Hotkeys that shell intercepts regardless of client focus:
        if (ev.rawKeycode == 125 || ev.rawKeycode == 126 ||
            ev.scancode == SDL_SCANCODE_LGUI || ev.scancode == SDL_SCANCODE_RGUI) {
            return true;
        }

        bool isCtrl = (ev.modifiers & SDL_KMOD_CTRL) != 0;
        bool isAlt  = (ev.modifiers & SDL_KMOD_ALT) != 0;
        bool isShift = (ev.modifiers & SDL_KMOD_SHIFT) != 0;
        bool isMeta  = (ev.modifiers & SDL_KMOD_GUI) != 0;

        // Ctrl+Space or Alt+Space (KEY_SPACE = 57, SDL_SCANCODE_SPACE = 44)
        bool isSpace = (ev.rawKeycode == 57 || ev.scancode == SDL_SCANCODE_SPACE);
        if (isSpace && (isCtrl || isAlt)) return true;

        // Ctrl+Alt+L or Meta+L (KEY_L = 38, SDL_SCANCODE_L = 15)
        bool isL = (ev.rawKeycode == 38 || ev.scancode == SDL_SCANCODE_L);
        if (isL && ((isCtrl && isAlt) || isMeta)) return true;

        // Super+V or Ctrl+Alt+V (KEY_V = 47, SDL_SCANCODE_V = 25)
        bool isV = (ev.rawKeycode == 47 || ev.scancode == SDL_SCANCODE_V);
        if (isV && ((isCtrl && isAlt) || isMeta)) return true;

        // Ctrl+Shift+N (KEY_N = 49, SDL_SCANCODE_N = 17)
        bool isN = (ev.rawKeycode == 49 || ev.scancode == SDL_SCANCODE_N);
        if (isN && isCtrl && isShift) return true;

        return false;
    };

    auto dispatchInput = [this, &shellWantsKeyboard](const platform::DrmInputEvent& ev) {
        if (ev.type == platform::DrmInputEvent::Type::MouseMove ||
            ev.type == platform::DrmInputEvent::Type::MouseDown ||
            ev.type == platform::DrmInputEvent::Type::MouseUp) {
            cursorVisible_ = true;
            uiDirty_ = true;
            lastMouseX_ = ev.x;
            lastMouseY_ = ev.y;
        }

#if BRO_WITH_COMPOSITOR
        if (drmCtx_ && drmCtx_->compositor && drmCtx_->compositor->isRunning()) {
            if (ev.type == platform::DrmInputEvent::Type::KeyDown) {
                if (!shellWantsKeyboard(ev) && drmCtx_->compositor->focusedWindow() != 0) {
                    uint32_t k = ev.rawKeycode ? ev.rawKeycode : static_cast<uint32_t>(ev.scancode);
                    drmCtx_->compositor->injectKey(k, true);
                    return;
                }
            } else if (ev.type == platform::DrmInputEvent::Type::KeyUp) {
                if (!shellWantsKeyboard(ev) && drmCtx_->compositor->focusedWindow() != 0) {
                    uint32_t k = ev.rawKeycode ? ev.rawKeycode : static_cast<uint32_t>(ev.scancode);
                    drmCtx_->compositor->injectKey(k, false);
                    return;
                }
            } else if (ev.type == platform::DrmInputEvent::Type::MouseMove) {
                if (drmCtx_->compositor->isDraggingWindow()) {
                    bool wasActive = drmCtx_->compositor->isDragActive();
                    bool moved = drmCtx_->compositor->updateInteractiveDrag(static_cast<double>(ev.x), static_cast<double>(ev.y));
                    if (!wasActive && drmCtx_->compositor->isDragActive()) {
                        drmCtx_->compositor->injectPointerButton(BTN_LEFT, false);
                    }
                    if (moved) uiDirty_ = true;
                    return;
                }

                bool overlay = isShellOverlayAt(ev.x, ev.y);
                if (overlay) {
                    drmCtx_->compositor->routePointer(-1.0, -1.0);
                } else {
                    drmCtx_->compositor->injectPointerWarp(static_cast<double>(ev.x), static_cast<double>(ev.y));
                    drmCtx_->compositor->routePointer(static_cast<double>(ev.x), static_cast<double>(ev.y));
                }
            } else if (ev.type == platform::DrmInputEvent::Type::MouseDown) {
                bool overlay = isShellOverlayAt(ev.x, ev.y);
                uint32_t wlButton = ev.rawButton ? ev.rawButton : BTN_LEFT;
                if (!ev.rawButton) {
                    if (ev.button == 3) wlButton = BTN_RIGHT;
                    else if (ev.button == 2) wlButton = BTN_MIDDLE;
                }

                if (!overlay) {
                    uint64_t hitWin = drmCtx_->compositor->windowAt(static_cast<double>(ev.x), static_cast<double>(ev.y));
                    bool isSuper = (ev.modifiers & SDL_KMOD_GUI) != 0;
                    bool isAlt   = (ev.modifiers & SDL_KMOD_ALT) != 0;
                    bool isLeft  = (wlButton == BTN_LEFT);
                    bool isRight = (wlButton == BTN_RIGHT);

                    if (hitWin != 0 && (isSuper || isAlt) && isLeft) {
                        drmCtx_->compositor->startInteractiveMove(hitWin, static_cast<double>(ev.x), static_cast<double>(ev.y), true);
                        if (hitWin != drmCtx_->compositor->focusedWindow()) {
                            drmCtx_->compositor->focusWindow(hitWin);
                        }
                        return;
                    }

                    if (hitWin != 0 && (isSuper || isAlt) && isRight) {
                        drmCtx_->compositor->startInteractiveResize(hitWin, static_cast<double>(ev.x), static_cast<double>(ev.y), 10, true);
                        if (hitWin != drmCtx_->compositor->focusedWindow()) {
                            drmCtx_->compositor->focusWindow(hitWin);
                        }
                        return;
                    }

                    if (hitWin != 0 && isLeft) {
                        auto snap = drmCtx_->compositor->queryWindow(hitWin);
                        if (snap) {
                            float lx = ev.x - snap->frame.x;
                            float ly = ev.y - snap->frame.y;

                            uint32_t resizeEdges = 0;
                            constexpr float kB = 6.0f;
                            if (lx >= 0.0f && lx < snap->frame.width && ly >= 0.0f && ly < snap->frame.height) {
                                if (lx < kB) resizeEdges |= 4;
                                else if (lx >= snap->frame.width - kB) resizeEdges |= 8;
                                if (ly < kB) resizeEdges |= 1;
                                else if (ly >= snap->frame.height - kB) resizeEdges |= 2;
                            }

                            if (resizeEdges != 0) {
                                drmCtx_->compositor->startInteractiveResize(hitWin, static_cast<double>(ev.x), static_cast<double>(ev.y), resizeEdges, true);
                                if (hitWin != drmCtx_->compositor->focusedWindow()) {
                                    drmCtx_->compositor->focusWindow(hitWin);
                                }
                                return;
                            }

                            if (ly >= 0.0f && ly < 38.0f) {
                                drmCtx_->compositor->startInteractiveMove(hitWin, static_cast<double>(ev.x), static_cast<double>(ev.y), false);
                                if (hitWin != drmCtx_->compositor->focusedWindow()) {
                                    drmCtx_->compositor->focusWindow(hitWin);
                                }
                                drmCtx_->compositor->routePointer(static_cast<double>(ev.x), static_cast<double>(ev.y));
                                drmCtx_->compositor->injectPointerButton(wlButton, true);
                                return;
                            }
                        }
                    }

                    bool hitClient = drmCtx_->compositor->routePointer(static_cast<double>(ev.x), static_cast<double>(ev.y));
                    if (hitClient) {
                        if (hitWin != 0 && hitWin != drmCtx_->compositor->focusedWindow()) {
                            drmCtx_->compositor->focusWindow(hitWin);
                        }
                        drmCtx_->compositor->injectPointerButton(wlButton, true);
                        return;
                    }
                } else {
                    if (drmCtx_->compositor->focusedWindow() != 0) {
                        drmCtx_->compositor->focusWindow(0);
                    }
                }
            } else if (ev.type == platform::DrmInputEvent::Type::MouseUp) {
                if (drmCtx_->compositor->isDraggingWindow()) {
                    bool wasActive = drmCtx_->compositor->isDragActive();
                    drmCtx_->compositor->endInteractiveDrag();
                    if (!wasActive && drmCtx_->compositor->focusedWindow() != 0) {
                        uint32_t wlButton = ev.rawButton ? ev.rawButton : BTN_LEFT;
                        if (!ev.rawButton) {
                            if (ev.button == 3) wlButton = BTN_RIGHT;
                            else if (ev.button == 2) wlButton = BTN_MIDDLE;
                        }
                        drmCtx_->compositor->injectPointerButton(wlButton, false);
                    }
                    uiDirty_ = true;
                    return;
                }

                bool overlay = isShellOverlayAt(ev.x, ev.y);
                uint32_t wlButton = ev.rawButton ? ev.rawButton : BTN_LEFT;
                if (!ev.rawButton) {
                    if (ev.button == 3) wlButton = BTN_RIGHT;
                    else if (ev.button == 2) wlButton = BTN_MIDDLE;
                }
                if (!overlay && drmCtx_->compositor->focusedWindow() != 0) {
                    drmCtx_->compositor->injectPointerButton(wlButton, false);
                    return;
                }
            } else if (ev.type == platform::DrmInputEvent::Type::MouseWheel) {
                bool overlay = isShellOverlayAt(ev.x, ev.y);
                if (!overlay && drmCtx_->compositor->focusedWindow() != 0) {
                    drmCtx_->compositor->injectPointerAxis(0, static_cast<double>(ev.wheelDy), 0);
                    return;
                }
            }
        }
#endif
        switch (ev.type) {
            case platform::DrmInputEvent::Type::KeyDown: {
                handleKeyDown(ev.keycode, ev.scancode, ev.modifiers, ev.repeat);
                if (!util::hasPrimaryMod(ev.modifiers)) {
                    std::string webKey = sdlKeycodeToWebKey(ev.keycode, ev.modifiers);
                    if (webKey.size() == 1) {
                        handleTextInput(webKey);
                    }
                }
                break;
            }
            case platform::DrmInputEvent::Type::KeyUp:
                handleKeyUp(ev.keycode, ev.scancode, ev.modifiers, ev.repeat);
                break;
            case platform::DrmInputEvent::Type::MouseMove:
                handleMouseMove(ev.x, ev.y, ev.dx, ev.dy);
                break;
            case platform::DrmInputEvent::Type::MouseDown:
                handleMouseDown(ev.x, ev.y, ev.button);
                break;
            case platform::DrmInputEvent::Type::MouseUp:
                handleMouseUp(ev.x, ev.y, ev.button);
                break;
            case platform::DrmInputEvent::Type::MouseWheel:
                handleWheel(ev.x, ev.y, ev.wheelDx, ev.wheelDy);
                break;
            case platform::DrmInputEvent::Type::TouchDown:
                handleTouchDown(ev.touchId, ev.x, ev.y, ev.touchPressure);
                break;
            case platform::DrmInputEvent::Type::TouchMove:
                handleTouchMove(ev.touchId, ev.x, ev.y, ev.touchPressure);
                break;
            case platform::DrmInputEvent::Type::TouchUp:
                handleTouchUp(ev.touchId, ev.x, ev.y);
                break;
            default:
                break;
        }
    };

    rasterReady_.store(false, std::memory_order_relaxed);

    framePresenter_ = std::make_unique<FramePresenter>();
    layoutPipeline_ = std::make_unique<LayoutPipeline>();

    layoutThread_ = std::thread(&Engine::layoutThreadFunc, this);
    rasterThread_ = std::thread(&Engine::rasterThreadFunc, this);
    rasterReady_.wait(false, std::memory_order_acquire);

    while (running_) {
        if (bro::util::interrupted()) {
            running_ = false;
            break;
        }
        double frameStart = util::currentTimeMs();

        double wallFrameDtMs = 0.0;
        if (lastWallTickMs_ > 0.0 && frameStart > lastWallTickMs_)
            wallFrameDtMs = frameStart - lastWallTickMs_;
        lastWallTickMs_ = frameStart;
        const double scaledFrameDtMs = wallFrameDtMs * effectiveTimeScale();
        engineNowMs_ += scaledFrameDtMs;

        if (layoutPipeline_->waitForIdle()) {
            updateDocumentHeight();
            for (auto& ev : transitionManager_.takePendingEvents()) {
                dom::TransitionEvent tevt(ev.type, true, false);
                tevt.setPropertyName(ev.name);
                tevt.setElapsedTime(ev.elapsedTime);
                tevt.setIsTrusted(true);
                dispatchEvent(ev.element, tevt);
            }
            for (auto& ev : animationManager_.takePendingEvents()) {
                dom::AnimationEvent aevt(ev.type, true, false);
                aevt.setAnimationName(ev.name);
                aevt.setElapsedTime(ev.elapsedTime);
                aevt.setIsTrusted(true);
                dispatchEvent(ev.element, aevt);
            }
        }

        pollAppWatcher(util::currentTimeMs());
        if (pendingAppReload_) {
            framePresenter_->waitUntilIdle();
            processPendingAppReload();
        }

        if (document_ && !document_->isStructureDirty()) {
            document_->drainPendingFrees();
        }

        pumpVideoEvents();
        pumpTerminals();
        pumpWebGLContextEvents();

        framePresenter_->consumeIfReady();

        if (!canvasScenesDetached_.empty() && framePresenter_->isRasterIdle()) canvasScenesDetached_.clear();

        if (drmCtx_) {
            if (drmCtx_->seat) drmCtx_->seat->pollEvents();
            if (drmCtx_->input) drmCtx_->input->pollEvents(dispatchInput);
#if BRO_WITH_COMPOSITOR
            if (drmCtx_->compositor && drmCtx_->compositor->pollEvents()) {
                uiDirty_ = true;
            }
#endif
        }

        beginGpuFrame();

#if BRO_WITH_PHYSICS
        if (physicsWorld_) {
            physicsWorld_->consumeStep();
        }
#endif

        auto isDetached = [](dom::Element* el) {
            if (!el) return false;
            auto* n = el;
            while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
            return n->tagName() != "html" && n->tagName() != "HTML";
        };
#if BRO_WITH_3D
        pruneDetachedSceneGraphs();
#endif
        webglEntries_.erase(
            std::remove_if(webglEntries_.begin(), webglEntries_.end(),
                [&](auto& e) { return isDetached(e.element); }),
            webglEntries_.end());

#if BRO_WITH_3D
        for (auto& sg : sceneGraphs_) sg.graph->syncPhysics();
#endif

        {
            double nowMs = engineNowMs_;
            float frameDt = (lastFrameTimeMs_ > 0.0)
                ? static_cast<float>((nowMs - lastFrameTimeMs_) / 1000.0)
                : 1.0f / 60.0f;
            if (frameDt < 0.0f) frameDt = 0.0f;
            if (frameDt > 0.1f) frameDt = 0.1f;
            lastFrameTimeMs_ = nowMs;
            bro::engine::pumpNavMeshObstacles(frameDt);
#if BRO_WITH_3D
            for (auto& sg : sceneGraphs_) {
                sg.graph->tickAnimations(frameDt);
            }
#endif
        }

        double now = util::currentTimeMs();
        tickSystemPanels(now);
        if (!timePaused_ && tickIframes(engineNowMs_)) uiDirty_ = true;
        if (!timePaused_ && tickWindowHosts(engineNowMs_)) uiDirty_ = true;
        if (systemDirty_) uiDirty_ = true;

        syncWebGLCanvasSizes();

        if (!timePaused_) fireFrameCallbacks(scaledFrameDtMs);

        for (auto& pump : framePumps_) pump();

#if BRO_WITH_3D
        if (auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get())) {
            for (auto& sg : sceneGraphs_) {
                if (sg.graph) sg.graph->materializeHtmlNodes(skia);
            }
        }

        for (auto& sg : sceneGraphs_) {
            if (sg.element) {
                auto& box = sg.element->layoutBox();
                int ew = static_cast<int>(box.contentRect.width);
                int eh = static_cast<int>(box.contentRect.height);
                if (ew > 0 && eh > 0 &&
                    (ew != sg.graph->canvasWidth() || eh != sg.graph->canvasHeight())) {
                    sg.graph->setCanvasSize(ew, eh);
                }
            }
            sg.graph->setDeviceScale(deviceScale_.render);
            sg.graph->render();
        }
#endif

#if BRO_WITH_PHYSICS
        if (physicsWorld_ && physicsWorld_->isIdle()) {
            if (!physicsWorld_->hasActiveBodies()) {
                lastPhysicsTimeMs_ = util::currentTimeMs();
                physicsAccumMs_ = 0.0;
            } else {
                double stepMs = physicsWorld_->timeStep() * 1000.0;
                double nowPhys = util::currentTimeMs();
                if (lastPhysicsTimeMs_ == 0.0) lastPhysicsTimeMs_ = nowPhys;
                physicsAccumMs_ += (nowPhys - lastPhysicsTimeMs_) * effectiveTimeScale();
                lastPhysicsTimeMs_ = nowPhys;
                if (physicsAccumMs_ >= stepMs) {
                    physicsAccumMs_ -= stepMs;
                    if (physicsAccumMs_ > stepMs * 3) physicsAccumMs_ = 0;
                    physicsWorld_->signalStep();
                }
            }
        }
#endif

        bool layoutIdle = layoutPipeline_->isIdle();
        bool layoutSignaled = false;
        bool animActive = layoutPipeline_->animationsActive();

        bool sceneHtmlDirty = false;
#if BRO_WITH_3D
        for (auto& sg : sceneGraphs_) {
            if (sg.graph && sg.graph->hasPendingHtmlWork()) { sceneHtmlDirty = true; break; }
        }
#endif

        bool baseWasDirty = document_ && document_->isDirty();

        if (layoutIdle && framePresenter_->isRasterIdle() && document_ &&
            (baseWasDirty || animActive || sceneHtmlDirty || !hasRenderedOnce_)) {
            if (document_->isStructureDirty()) {
                ensureReplacedElements(document_->documentElement());
                iframeSyncNeeded_ = true;
            }
            LayoutPipeline::Snapshot ls;
            ls.vpWidth          = viewportWidth_;
            ls.vpHeight         = viewportHeight_;
            ls.insetTop         = contentTop();
            ls.insetRight       = contentRight();
            ls.insetBottom      = contentBottom();
            ls.animationsActive = animActive;
            ls.hoveredElement   = hoveredElement_.get();
            ls.timeMs           = engineNowMs_;
            layoutPipeline_->signalLayout(ls);
            layoutSignaled = true;
        }

        renderAndPresentFrame(frameStart, now, wallFrameDtMs, layoutSignaled, baseWasDirty);

        if (vulkanPresenter_ && vulkanPresenter_->kmsDirectPresenter()) {
            vulkanPresenter_->kmsDirectPresenter()->handlePageFlipEvent(10);
        }
    }

    shutdown();
#endif
}

bool Engine::isShellOverlayAt(float x, float y) {
    if (!isShellApp()) return true;
    if (!document_ || !document_->documentElement()) return false;

    dom::Element* hit = hitTest(x, y);
    if (!hit) return false;
    if (hit == document_->documentElement() || hit == document_->body()) {
        return false;
    }

    for (dom::Element* cur = hit; cur; cur = cur->parentElement()) {
        if (cur == document_->body() || cur == document_->documentElement()) break;

        const std::string& id = cur->id();
        if (id == "wallpaper") return false;
        const std::string& cls = cur->className();
        if (cls.find("desktop-wallpaper") != std::string::npos) return false;

        if (id == "top-panel" || id == "launcher-modal" ||
            id == "notify-drawer" || id == "lock-screen" ||
            cls.find("quick-popup") != std::string::npos ||
            cls.find("toast") != std::string::npos) {
            return true;
        }

        const auto& style = cur->computedStyle();
        auto it = style.find("z-index");
        if (it != style.end() && !it->second.empty() && it->second != "auto") {
            try {
                int z = std::stoi(it->second);
                if (z >= 1000) return true;
            } catch (...) {}
        }
    }
    return false;
}

} // namespace bro::engine
