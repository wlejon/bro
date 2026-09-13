// Engine system panel methods — split from engine.cpp for readability.
// These are Engine member function implementations, not a separate class.

#include "engine/engine.h"
#include "bronze_host/host_telemetry.h"
#include "engine/default_styles.h"
#include "engine/app_loader.h"
#include "layout/box.h"
#include "layout/layout_node_adapter.h"
#include "engine/replaced_elements.h"
#include "engine/settings.h"
#include "engine/key_mapping.h"
#include "engine/overflow.h"
#include "util/platform.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include "render/renderer.h"
#include "render/recording_renderer.h"
#include "render/skia_backend.h"
#include "render/gl_context.h"
#include <include/gpu/ganesh/GrDirectContext.h>
#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "canvas/canvas_scene.h"
#include "platform/sdl_window.h"
#if BRO_WITH_3D
#include "scene/scene_renderer.h"
#endif

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkSurface.h>

#include "util/log.h"
#include "util/time.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <regex>

namespace fs = std::filesystem;

namespace bro::engine {

// ---------------------------------------------------------------------------
// System panel lifecycle
// ---------------------------------------------------------------------------

void Engine::initSystemPanels() {
    // Load app-specific system panels first (app dir takes priority)
    std::string appSystemDir = manifest_.basePath + "/system";
    loadSystemPanels(appSystemDir);

    // Load global system panels, skipping any already provided by the app
    loadSystemPanels("system");

    // Move the splash panel to the end so it renders on top of everything
    // (menu bar included) and receives hit-tests first during startup.
    std::stable_partition(systemDocs_.begin(), systemDocs_.end(),
        [](const SystemDocument& d) { return d.group != "splash"; });
}

void Engine::destroySystemPanels() {
    for (auto& doc : systemDocs_) {
        doc.document.reset();
    }
    systemDocs_.clear();
}

// ---------------------------------------------------------------------------
// Panel loading
// ---------------------------------------------------------------------------

void Engine::loadSystemPanels(const std::string& systemDir) {
    std::error_code ec;
    if (!fs::is_directory(systemDir, ec)) {
        LOG_INFO("System panels: no system directory at '%s'", systemDir.c_str());
        return;
    }

    scanSystemPanelDir(systemDir, "");

    // Default: prefer "settings/graphics" as initial active, else first found
    for (auto& d : systemDocs_) {
        if (!d.group.empty()) {
            d.active = false;
            if (systemActivePanel_.empty() || d.name == "settings/graphics") {
                if (!systemActivePanel_.empty()) {
                    for (auto& q : systemDocs_) {
                        if (q.name == systemActivePanel_) q.active = false;
                    }
                }
                systemActivePanel_ = d.name;
                d.active = true;
            }
        }
    }

    LOG_INFO("System panels: loaded %zu panel(s)", systemDocs_.size());
}

void Engine::scanSystemPanelDir(const std::string& baseDir, const std::string& relPath) {
    std::error_code ec;
    std::string dirPath = relPath.empty() ? baseDir : baseDir + "/" + relPath;

    for (const auto& entry : fs::directory_iterator(dirPath, ec)) {
        if (entry.is_directory()) {
            // A subdirectory containing its own bro.json is a self-contained
            // bro app shipped under system/ (e.g. system/projects/, the
            // built-in project manager, and system/skeletons/<name>/), not
            // an overlay panel. Skip it so its DOM is not loaded twice.
            if (fs::exists(entry.path() / "bro.json", ec)) continue;
            std::string subName = entry.path().filename().string();
            std::string subRel = relPath.empty() ? subName : relPath + "/" + subName;
            scanSystemPanelDir(baseDir, subRel);
            continue;
        }

        if (entry.path().extension() != ".html") continue;

        std::string stem = entry.path().stem().string();
        std::string fullRel = relPath.empty() ? stem : relPath + "/" + stem;

        // Skip if a panel with this name was already loaded (app override)
        bool duplicate = false;
        for (const auto& d : systemDocs_) {
            if (d.name == fullRel) { duplicate = true; break; }
        }
        if (duplicate) continue;

        std::string htmlPath = entry.path().string();
        std::string html = AppLoader::loadFile(htmlPath);
        if (html.empty()) continue;

        SystemDocument doc;
        doc.name = fullRel;

        // Assign tab label and group based on panel path
        if (fullRel == "perf") {
            doc.tabLabel = "";
            doc.group = "perf";
        } else if (fullRel == "nav") {
            doc.tabLabel = "";
            doc.group = "nav";
        } else if (fullRel == "menu") {
            doc.tabLabel = "";
            doc.group = "menu";
        } else if (fullRel == "inspector") {
            doc.tabLabel = "";
            doc.group = "inspector";
        } else if (fullRel == "splash") {
            doc.tabLabel = "";
            doc.group = "splash";
        } else if (fullRel.rfind("settings/", 0) == 0) {
            doc.group = "settings";
            std::string leaf = stem;
            if (!leaf.empty()) leaf[0] = static_cast<char>(toupper(leaf[0]));
            doc.tabLabel = leaf;
            doc.active = false;
        } else {
            doc.tabLabel = fullRel;
            doc.group = "";
        }

        std::string savedBasePath = dirPath;

        // Extract inline CSS from <style> elements
        std::string authorStyles;
        {
            std::regex styleRe(R"(<style[^>]*>([\s\S]*?)</style>)",
                               std::regex_constants::icase);
            auto begin = std::sregex_iterator(html.begin(), html.end(), styleRe);
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it) {
                authorStyles += (*it)[1].str() + "\n";
            }
        }

        // Parse HTML — UA defaults at UserAgent origin, inline styles at Author
        doc.document = std::make_unique<dom::Document>();
        doc.document->setMediaViewport(static_cast<float>(viewportWidth_),
                                       static_cast<float>(viewportHeight_));
        doc.document->setMediaColorScheme(effectiveColorScheme());
        doc.document->parse(html, authorStyles, kDefaultStyles);

        // Initial layout — shared engine text metrics (same renderer/fontManager
        // as the app document; system panels are just additional documents).
        doc.document->resolveStyles();
        doc.document->performLayout(static_cast<float>(viewportWidth_),
                                    static_cast<float>(viewportHeight_),
                                    *textMetrics_);

        LOG_INFO("System panels: loaded panel '%s'", doc.name.c_str());

        systemDocs_.push_back(std::move(doc));
        size_t docIdx = systemDocs_.size() - 1;
        auto& liveDoc = systemDocs_[docIdx];

        // Initialize replaced elements
        bro::engine::ensureReplacedElements(liveDoc.document->documentElement(),
                                            renderer_.get(),
                                            audioEngine_.get());

        // Re-layout after replaced elements attach
        liveDoc.document->resolveStyles();
        liveDoc.document->performLayout(static_cast<float>(viewportWidth_),
                                        static_cast<float>(viewportHeight_),
                                        *textMetrics_);
        // Stash base path on the document so drawSystemPanels can forward it
        // to the shared DrawTraversal (for image URL resolution).
        liveDoc.document->setBasePath(savedBasePath);
    }
}

// ---------------------------------------------------------------------------
// Menu actions + re-render notification
// ---------------------------------------------------------------------------

void Engine::triggerMenuAction(const std::string& id) {
    if (id == "__system.preferences") { toggleSystemSettings(); return; }
    if (id == "__system.quit") { running_ = false; return; }
    if (id == "__system.inspector") { toggleInspector(); return; }
    if (id == "__system.togglePerf") {
        toggleSystemPerf();
        // Debug → Perf HUD item shows a checkmark when the overlay is open.
        if (auto* item = menuBar_.find("__system.togglePerf")) {
            item->checked = systemPerfVisible_;
            menuBar_.dirty = true;
            onMenuChanged();
        }
        return;
    }
    menuBar_.triggerHandler(id);
}

// ---------------------------------------------------------------------------
// Inspector
// ---------------------------------------------------------------------------

void Engine::toggleInspector() {
    inspector_.visible = !inspector_.visible;
    if (!inspector_.visible) {
        inspector_.pickerMode = false;
        inspector_.pickerHover = nullptr;
    }

    // View → Inspector item shows a checkmark when the panel is open.
    if (auto* item = menuBar_.find("__system.inspector")) {
        item->checked = inspector_.visible;
        menuBar_.dirty = true;
        onMenuChanged();
    }

    // Re-layout the app document into the new content area.
    handleResize(viewportWidth_, viewportHeight_);
    systemDirty_ = true;
}

void Engine::inspectorSetDock(InspectorDock dock) {
    if (inspector_.dock == dock) return;
    inspector_.dock = dock;
    if (inspector_.visible) handleResize(viewportWidth_, viewportHeight_);
    systemDirty_ = true;
}

void Engine::inspectorSetSize(int sizePx) {
    if (sizePx < 120) sizePx = 120;
    if (inspector_.dock == InspectorDock::Right) inspector_.width = sizePx;
    else inspector_.height = sizePx;
    if (inspector_.visible) handleResize(viewportWidth_, viewportHeight_);
    systemDirty_ = true;
}

void Engine::inspectorSetPickerMode(bool on) {
    inspector_.pickerMode = on;
    if (!on) inspector_.pickerHover = nullptr;
    systemDirty_ = true;
}

void Engine::inspectorPickElement(dom::Element* el) {
    inspector_.selected = el;
    systemDirty_ = true;
}

void Engine::onMenuChanged() {
    systemDirty_ = true;
    // Visibility may have flipped — the app doc's usable height depends on
    // contentTop(). Re-run the resize path so innerHeight, layout, scroll
    // clamp, and the resize event all update in lockstep.
    handleResize(viewportWidth_, viewportHeight_);
    menuBar_.dirty = false;
}

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

bool Engine::isSystemDocVisible(const SystemDocument& doc) const {
    if (doc.group == "perf") return systemPerfVisible_;
    if (doc.group == "nav") return systemSettingsVisible_;
    if (doc.group == "settings") return systemSettingsVisible_ && doc.active;
    if (doc.group == "menu") return menuBar_.visible;
    if (doc.group == "inspector") return inspector_.visible;
    if (doc.group == "splash") return splashVisible_;
    return false;
}

bool Engine::isSystemVisible() const {
    return systemPerfVisible_ || systemSettingsVisible_ || menuBar_.visible
        || inspector_.visible || splashVisible_;
}

void Engine::toggleSystemPerf() {
    systemPerfVisible_ = !systemPerfVisible_;
    systemDirty_ = true;
    LOG_INFO("System perf %s", systemPerfVisible_ ? "visible" : "hidden");
}

void Engine::toggleSystemSettings() {
    systemSettingsVisible_ = !systemSettingsVisible_;
    systemDirty_ = true;
    LOG_INFO("System settings %s", systemSettingsVisible_ ? "visible" : "hidden");
}

void Engine::showSystemPanel(const std::string& name) {
    std::string targetGroup;
    for (auto& d : systemDocs_) {
        if (d.name == name) {
            targetGroup = d.group;
            break;
        }
    }
    if (targetGroup.empty()) return;

    for (auto& d : systemDocs_) {
        if (d.group == targetGroup) {
            d.active = (d.name == name);
            if (d.active && d.document) {
                d.document->markDirty();
            }
        }
    }
    systemActivePanel_ = name;
    systemDirty_ = true;
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void Engine::tickSystemPanels(double nowMs) {
    if (splashVisible_) {
        constexpr double kMinDisplayMs = 1800.0;
        double elapsed = nowMs - splashStartMs_;
        if (!splashDismissTriggered_ && elapsed >= kMinDisplayMs) {
            splashDismissTriggered_ = true;
            splashVisible_ = false;
            systemDirty_ = true;
        }
    }

    if (!isSystemVisible()) return;

    for (auto& doc : systemDocs_) {
        if (isSystemDocVisible(doc) && doc.document && doc.document->isDirty()) {
            systemDirty_ = true;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Perf data update
// ---------------------------------------------------------------------------

void Engine::updateSystemPerf(double fps, double frameTime, double js, double layout,
                              double raster, double gpu, double draw,
                              int vpW, int vpH) {
    auto tel = bronze_host::getHostTelemetry();
    for (auto& doc : systemDocs_) {
        if (doc.group == "perf" && doc.document) {
            bronze_host::updatePerfDocument(doc.document.get(), tel, fps, frameTime, js, layout,
                                            raster, gpu, draw, vpW, vpH);
        }
    }
}

// ---------------------------------------------------------------------------
// Layout + draw helpers
// ---------------------------------------------------------------------------

void Engine::stageSystemPanelCanvases() {
    if (!isSystemVisible()) return;
    for (auto& doc : systemDocs_) {
        if (!isSystemDocVisible(doc)) continue;
        for (auto& scene : doc.canvasScenes) {
            if (scene) scene->stageCommandsForRaster();
        }
    }
}

void Engine::layoutSystemPanels(layout::SkiaTextMetrics& metrics) {
    for (auto& doc : systemDocs_) {
        if (!isSystemDocVisible(doc) || !doc.document) continue;
        if (!doc.document->isDirty()) continue;
        doc.document->resolveStyles();
        doc.document->performLayout(static_cast<float>(viewportWidth_),
                                    static_cast<float>(viewportHeight_),
                                    metrics);
        doc.document->clearDirty();
    }
}

void Engine::drawSystemPanelDoc(render::Renderer* renderer,
                                layout::DrawTraversal& traversal,
                                SystemDocument& doc,
                                int vpW, int vpH) {
    if (!renderer || !doc.document) return;

    auto* recorder = dynamic_cast<render::RecordingRenderer*>(renderer);
    auto* skiaRenderer = dynamic_cast<render::SkiaRenderer*>(renderer);
    GrDirectContext* panelGr = skiaRenderer ? skiaRenderer->grContext() : nullptr;

    traversal.setLayerBreakCallback(
        [renderer, recorder, panelGr](canvas::CanvasScene* scene,
                                       unsigned int /*tex*/,
                                       float x, float y, float w, float h,
                                       float /*clipX*/, float /*clipY*/,
                                       float /*clipW*/, float /*clipH*/) {
            if (!scene || !renderer) return;
            if (w <= 0 || h <= 0) return;
            if (recorder) {
                recorder->recordBlitCanvasInline(scene, x, y, w, h);
                return;
            }
            if (panelGr) scene->setGrContext(panelGr);
            scene->flushStaged();
            auto* src = scene->surface();
            if (!src) return;
            auto img = src->makeImageSnapshot();
            if (!img) return;
            auto* c = renderer->getCanvas();
            if (!c) return;
            SkRect dst = SkRect::MakeXYWH(x, y, w, h);
            c->drawImageRect(img, dst, SkSamplingOptions(SkFilterMode::kLinear));
            scene->clearDirty();
        });

    traversal.setBasePath(doc.document->basePath());
    traversal.draw(doc.document->documentElement(), 0, 0,
                   static_cast<float>(vpW), static_cast<float>(vpH));

    drawElementScrollbars(renderer, doc.document->documentElement(),
                          0.0f, 0.0f);

    traversal.setLayerBreakCallback(nullptr);
}

void Engine::drawSystemPanels(render::Renderer* renderer,
                              layout::DrawTraversal& traversal) {
    if (!renderer || !isSystemVisible()) return;

    for (auto& doc : systemDocs_) {
        if (!isSystemDocVisible(doc) || !doc.document) continue;
        drawSystemPanelDoc(renderer, traversal, doc,
                           viewportWidth_, viewportHeight_);
    }

    overlayMgr_.drawIfContext(OverlayContext::System, renderer);
    systemDirty_ = false;
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

void Engine::resizeSystemPanels(int w, int h) {
    systemDirty_ = true;
    for (auto& doc : systemDocs_) {
        if (doc.document) {
            doc.document->setMediaViewport(static_cast<float>(w),
                                           static_cast<float>(h));
            doc.document->resolveStyles();
            doc.document->performLayout(static_cast<float>(w),
                                        static_cast<float>(h),
                                        *textMetrics_);
        }
    }
}

// ---------------------------------------------------------------------------
// Mouse input
// ---------------------------------------------------------------------------

dom::Element* Engine::systemHitTest(SystemDocument& doc, float x, float y) {
    if (!doc.document || !doc.document->documentElement()) return nullptr;
    auto* root = doc.document->layoutRoot();
    if (!root) return nullptr;
    auto* node = htmlayout::layout::hitTest(root, x, y);
    auto* hit = layout::LayoutNodeAdapter::elementFor(node);
    if (!hit) return nullptr;
    if (hit == doc.document->documentElement()) return nullptr;
    auto& tag = hit->tagName();
    if (tag == "BODY" && hit->layoutBox().fullHeight() < 1.0f) return nullptr;
    return hit;
}

bool Engine::systemHandleMouseDown(float x, float y, int button) {
    if (!isSystemVisible()) return false;

    systemMouseConsumed_ = false;
    for (auto& doc : systemDocs_) {
        if (!isSystemDocVisible(doc) || !doc.document) continue;
        ControlContext cctx{doc.document.get(),
                           renderer_.get(), window_.get(), &systemDirty_,
                           &overlayMgr_, OverlayContext::System,
                           viewportWidth_, viewportHeight_};
        auto* prevActive = doc.document->activeElement();
        auto disp = unfocusPreviousControl(cctx, prevActive);
        if (disp == ClickDisposition::Consumed) {
            systemMouseConsumed_ = true;
            systemDirty_ = true;
            return true;
        }
    }

    for (int i = static_cast<int>(systemDocs_.size()) - 1; i >= 0; i--) {
        auto& doc = systemDocs_[i];
        if (!isSystemDocVisible(doc) || !doc.document) continue;
        ScrollbarMetrics em;
        dom::Element* hitElem = findElementScrollbarHit(
            doc.document->documentElement(), x, y,
            0.0f, 0.0f, elementScrollbar_, em);
        if (!hitElem) continue;
        if (elementScrollbar_.thumbHitTest(x, y, em)) {
            elementScrollbar_.beginDrag(y, em);
            scrollbarDragTarget_.assign(doc.document.get(), hitElem);
            scrollbarDragSystemDoc_ = &doc;
        } else {
            float viewH = hitElem->layoutBox().contentRect.height;
            float maxST = maxScrollTop(hitElem);
            float contentH = viewH + maxST;
            float newScroll = elementScrollbar_.scrollToPosition(y,
                contentH, viewH, em);
            float prev = hitElem->scrollTopValue();
            float clamped = std::clamp(newScroll, 0.0f, maxST);
            hitElem->setScrollTopValue(clamped);
            if (clamped != prev) {
                systemDirty_ = true;
                doc.document->markDirty();
            }
        }
        systemDirty_ = true;
        return true;
    }

    for (int i = static_cast<int>(systemDocs_.size()) - 1; i >= 0; i--) {
        auto& doc = systemDocs_[i];
        if (!isSystemDocVisible(doc)) continue;
        dom::Element* target = systemHitTest(doc, x, y);
        if (target) {
            ControlContext cctx{doc.document.get(),
                               renderer_.get(), window_.get(), &systemDirty_,
                               &overlayMgr_, OverlayContext::System,
                               viewportWidth_, viewportHeight_};

            dom::MouseEvent evt("mousedown");
            evt.setClientX(static_cast<double>(x));
            evt.setClientY(static_cast<double>(y));
            evt.setButton(button);
            evt.setIsTrusted(true);
            applyMouseOffset(evt, target);

            PressIntent intent;
            intent.ordinal = pressOrdinal(doc.mouseState, target, x, y,
                                          util::currentTimeMs(),
                                          inputConfig_.doubleClickThresholdMs,
                                          inputConfig_.doubleClickDistancePx);
            intent.extend = (currentModState() & SDL_KMOD_SHIFT) != 0;

            controlDragElement_.reset();
            if (button == 0 &&
                (getElTextarea(target) ||
                 (getElInput(target) && getElInput(target)->isTextType(target)))) {
                controlDragElement_.assign(doc.document.get(), target);
                controlDragIsPanel_ = true;
            }

            dispatchDocMousePress(cctx, doc.mouseState, target, evt, x, y, intent);
            return true;
        }
    }
    return false;
}

bool Engine::systemHandleMouseUp(float x, float y, int button) {
    if (!isSystemVisible()) return false;

    if (systemMouseConsumed_) {
        systemMouseConsumed_ = false;
        return true;
    }

    for (int i = static_cast<int>(systemDocs_.size()) - 1; i >= 0; i--) {
        auto& doc = systemDocs_[i];
        if (!isSystemDocVisible(doc)) continue;
        dom::Element* target = systemHitTest(doc, x, y);

        if (target) {
            ControlContext cctx{doc.document.get(),
                               renderer_.get(), window_.get(), &systemDirty_,
                               &overlayMgr_, OverlayContext::System,
                               viewportWidth_, viewportHeight_};

            dom::MouseEvent upEvt("mouseup");
            upEvt.setClientX(static_cast<double>(x));
            upEvt.setClientY(static_cast<double>(y));
            upEvt.setButton(button);
            upEvt.setIsTrusted(true);
            applyMouseOffset(upEvt, target);

            dispatchDocMouseRelease(cctx, doc.mouseState, target, upEvt,
                                    x, y, button, 0, 0, 0, 0, x, y,
                                    util::currentTimeMs(),
                                    inputConfig_.doubleClickThresholdMs,
                                    inputConfig_.doubleClickDistancePx);

            if (doc.document) {
                auto* activeEl = doc.document->activeElement();
                auto* input = getElInput(activeEl);
                if (input && input->isDragging()) {
                    input->setDragging(false);
                    dom::Event changeEvt("change");
                    dispatchControlEvent(cctx, activeEl, changeEvt);
                }
            }
            return true;
        }
    }
    return false;
}

bool Engine::systemHandleMouseMove(float x, float y) {
    if (!isSystemVisible()) return false;

    dom::Element* newTarget = nullptr;
    SystemDocument* newDoc = nullptr;

    for (int i = static_cast<int>(systemDocs_.size()) - 1; i >= 0; i--) {
        auto& doc = systemDocs_[i];
        if (!isSystemDocVisible(doc)) continue;
        dom::Element* target = systemHitTest(doc, x, y);
        if (target) {
            newTarget = target;
            newDoc = &doc;
            break;
        }
    }

    dom::Element* prevSysHover = systemHoverTarget_.get();
    if (newTarget != prevSysHover) {
        if (prevSysHover && systemHoverDoc_) {
            dom::MouseEvent leaveEvt("mouseleave");
            leaveEvt.setClientX(static_cast<double>(x));
            leaveEvt.setClientY(static_cast<double>(y));
            dom::dispatchDomEvent(prevSysHover, leaveEvt);
        }
        if (newTarget && newDoc) {
            dom::MouseEvent enterEvt("mouseenter");
            enterEvt.setClientX(static_cast<double>(x));
            enterEvt.setClientY(static_cast<double>(y));
            dom::dispatchDomEvent(newTarget, enterEvt);
        }
        systemHoverTarget_.assign(newDoc ? newDoc->document.get() : nullptr, newTarget);
        systemHoverDoc_ = newDoc;
        systemDirty_ = true;
    }

    if (newTarget && newDoc) {
        dom::MouseEvent moveEvt("mousemove");
        moveEvt.setClientX(static_cast<double>(x));
        moveEvt.setClientY(static_cast<double>(y));
        dom::dispatchDomEvent(newTarget, moveEvt);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Keyboard input (modal panels)
// ---------------------------------------------------------------------------

bool Engine::systemHandleKeyDown(int keycode, int scancode, int mod, bool repeat) {
    if (!systemSettingsVisible_) return false;
    bool prevented = false;
    for (auto& doc : systemDocs_) {
        if (doc.group != "nav" && doc.group != "settings") continue;
        if (!isSystemDocVisible(doc) || !doc.document) continue;
        auto* body = doc.document->body();
        if (!body) continue;
        dom::KeyboardEvent evt("keydown");
        evt.setKey(sdlKeycodeToWebKey(keycode, mod));
        evt.setCode(sdlScancodeToWebCode(scancode));
        evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
        evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
        evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
        evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
        evt.setRepeat(repeat);
        evt.setIsTrusted(true);
        dom::dispatchDomEvent(body, evt);
        if (evt.defaultPrevented()) prevented = true;
    }
    return prevented;
}

bool Engine::systemHandleWheel(float x, float y, float dx, float dy) {
    if (!isSystemVisible()) return false;
    for (int i = static_cast<int>(systemDocs_.size()) - 1; i >= 0; i--) {
        auto& doc = systemDocs_[i];
        if (!isSystemDocVisible(doc) || !doc.document) continue;
        dom::Element* target = systemHitTest(doc, x, y);
        if (!target) continue;

        auto* el = target;
        while (el) {
            std::string ov = getOverflowY(el->computedStyle());
            if (overflowScrollable(ov)) {
                float maxST = maxScrollTop(el);
                if (maxST > 0) {
                    float scrollPx = -util::wheelDeltaToPixels(
                        util::verticalWheelDelta(dx, dy), inputConfig_.scrollSpeed);
                    float prev = el->scrollTopValue();
                    float next = std::clamp(prev + scrollPx, 0.0f, maxST);
                    if (next != prev) {
                        el->setScrollTopValue(next);
                        systemDirty_ = true;
                        if (doc.document) doc.document->markDirty();
                    }
                    return true;
                }
            }
            el = el->parentElement();
        }
        if (systemSettingsVisible_) return true;
    }
    return false;
}

bool Engine::systemHandleKeyUp(int keycode, int scancode, int mod, bool repeat) {
    if (!systemSettingsVisible_) return false;
    bool prevented = false;
    for (auto& doc : systemDocs_) {
        if (doc.group != "nav" && doc.group != "settings") continue;
        if (!isSystemDocVisible(doc) || !doc.document) continue;
        auto* body = doc.document->body();
        if (!body) continue;
        dom::KeyboardEvent evt("keyup");
        evt.setKey(sdlKeycodeToWebKey(keycode, mod));
        evt.setCode(sdlScancodeToWebCode(scancode));
        evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
        evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
        evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
        evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
        evt.setRepeat(repeat);
        evt.setIsTrusted(true);
        dom::dispatchDomEvent(body, evt);
        if (evt.defaultPrevented()) prevented = true;
    }
    return prevented;
}

// ---------------------------------------------------------------------------
// Inspector DOM tree helpers
// ---------------------------------------------------------------------------

namespace {

bool elementInTree(dom::Element* el, dom::Element* root) {
    while (el) {
        if (el == root) return true;
        el = el->parentElement();
    }
    return false;
}

} // namespace

void Engine::inspectorSelectById(int id) {
    auto it = inspectorNodeMap_.find(id);
    if (it == inspectorNodeMap_.end()) return;
    auto* root = document_ ? document_->documentElement() : nullptr;
    if (!root || !elementInTree(it->second, root)) return;
    inspector_.selected = it->second;
    systemDirty_ = true;
}

// ---------------------------------------------------------------------------
// Headless DOM inspection
// ---------------------------------------------------------------------------

dom::Element* Engine::overlayQuerySelector(const std::string& panelName,
                                           const std::string& selector) const {
    for (auto& doc : systemDocs_) {
        if (doc.name == panelName && doc.document) {
            return doc.document->querySelector(selector);
        }
    }
    return nullptr;
}

std::vector<std::string> Engine::overlayPanelNames() const {
    std::vector<std::string> names;
    for (const auto& doc : systemDocs_) {
        names.push_back(doc.name);
    }
    return names;
}

} // namespace bro::engine
