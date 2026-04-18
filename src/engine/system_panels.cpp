// Engine system panel methods — split from engine.cpp for readability.
// These are Engine member function implementations, not a separate class.

#include "engine/engine.h"
#include "engine/default_styles.h"
#include "engine/app_loader.h"
#include "layout/box.h"
#include "layout/layout_node_adapter.h"
#include "engine/replaced_elements.h"
#include "engine/settings.h"
#include "render/cpu_raster_renderer.h"
#include "render/gl_context.h"
#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "js/event_dispatch.h"
#include "js/settings_bindings.h"
#include "platform/sdl_window.h"

#include "api/api.h"
#include "util/log.h"

#include <filesystem>
#include <regex>

extern "C" {
#include "quickjs.h"
}

namespace fs = std::filesystem;

namespace bro::engine {

// ---------------------------------------------------------------------------
// System panel lifecycle
// ---------------------------------------------------------------------------

void Engine::initSystemPanels() {
    systemRenderer_ = std::make_unique<render::CPURasterRenderer>(gl_.get());

    // Load app-specific system panels first (app dir takes priority)
    std::string appSystemDir = manifest_.basePath + "/system";
    loadSystemPanels(appSystemDir);

    // Load global system panels, skipping any already provided by the app
    loadSystemPanels("system");
}

void Engine::destroySystemPanels() {
    for (auto& doc : systemDocs_) {
        if (doc.timers && doc.jsCtx) {
            doc.timers->clearAll(doc.jsCtx);
        }
        doc.timers.reset();

        if (!JS_IsUndefined(doc.broPerfObj) && doc.jsCtx) {
            JS_FreeValue(doc.jsCtx, doc.broPerfObj);
            doc.broPerfObj = JS_UNDEFINED;
        }

        if (doc.jsCtx) {
            js::DomBindings::cleanup(doc.jsCtx);
        }

        doc.document.reset();
        doc.drawTraversal.reset();

        if (doc.jsCtx) {
            JS_FreeContext(doc.jsCtx);
            doc.jsCtx = nullptr;
        }
    }
    systemDocs_.clear();
    systemRenderer_.reset();
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
        doc.fontManager = std::make_unique<layout::FontManager>();
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
        doc.document->parse(html, authorStyles, kDefaultStyles);

        // Create a dedicated JSContext on the shared runtime
        doc.jsCtx = jsRuntime_->createContext();

        // Install standard bindings
        brokit::api::installConsole(doc.jsCtx);
        doc.timers = std::make_unique<js::Timers>();
        js::Timers::install(doc.jsCtx, doc.timers.get());

        // Set window = globalThis
        JSValue global = JS_GetGlobalObject(doc.jsCtx);
        JS_SetPropertyStr(doc.jsCtx, global, "window", JS_DupValue(doc.jsCtx, global));
        JS_FreeValue(doc.jsCtx, global);

        // Install DOM bindings
        js::DomBindings::install(doc.jsCtx, doc.document.get());

        // Install settings bindings if available
        if (settings_) {
            js::SettingsBindings::install(doc.jsCtx, settings_.get(), window_.get());
        }

        // Install __bro perf/nav object
        installBroObject(doc);

        // Initial layout
        {
            layout::SkiaTextMetrics textMetrics(systemRenderer_.get(), doc.fontManager.get());
            doc.document->resolveStyles();
            doc.document->performLayout(static_cast<float>(viewportWidth_), textMetrics);
        }

        LOG_INFO("System panels: loaded panel '%s'", doc.name.c_str());

        // Extract and execute inline scripts
        {
            std::regex scriptRe(R"(<script[^>]*>([\s\S]*?)</script>)",
                                std::regex_constants::icase);
            auto begin = std::sregex_iterator(html.begin(), html.end(), scriptRe);
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it) {
                std::string code = (*it)[1].str();
                if (!code.empty()) {
                    std::string filename = "<system/" + doc.name + ">";
                    JSValue result = JS_Eval(doc.jsCtx, code.c_str(), code.size(),
                                             filename.c_str(), JS_EVAL_TYPE_GLOBAL);
                    if (JS_IsException(result)) {
                        JSValue ex = JS_GetException(doc.jsCtx);
                        const char* str = JS_ToCString(doc.jsCtx, ex);
                        if (str) {
                            LOG_ERROR("System panel JS error in '%s': %s",
                                      doc.name.c_str(), str);
                            JS_FreeCString(doc.jsCtx, str);
                        }
                        JS_FreeValue(doc.jsCtx, ex);
                    }
                    JS_FreeValue(doc.jsCtx, result);
                }
            }
        }

        // Initialize replaced elements after scripts
        bro::engine::ensureReplacedElements(doc.document->documentElement(), systemRenderer_.get());

        // Re-layout after scripts may have modified the DOM
        {
            layout::SkiaTextMetrics textMetrics(systemRenderer_.get(), doc.fontManager.get());
            doc.document->resolveStyles();
            doc.document->performLayout(static_cast<float>(viewportWidth_), textMetrics);
        }

        // Move into vector, then create DrawTraversal pointing to stable fontManager
        systemDocs_.push_back(std::move(doc));
        auto& finalDoc = systemDocs_.back();
        finalDoc.drawTraversal = std::make_unique<layout::DrawTraversal>(
            systemRenderer_.get(), finalDoc.fontManager.get());
        finalDoc.drawTraversal->setBasePath(savedBasePath);
        finalDoc.drawTraversal->setViewport(viewportWidth_, viewportHeight_);
    }
}

// ---------------------------------------------------------------------------
// __bro JS object
// ---------------------------------------------------------------------------

void Engine::installBroObject(SystemDocument& doc) {
    JSContext* ctx = doc.jsCtx;
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue bro = JS_NewObject(ctx);
    JSValue perf = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, perf, "fps", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, perf, "frameTime", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, perf, "js", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, perf, "layout", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, perf, "raster", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, perf, "gpu", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, perf, "draw", JS_NewFloat64(ctx, 0.0));

    JSValue viewport = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, viewport, "width", JS_NewInt32(ctx, viewportWidth_));
    JS_SetPropertyStr(ctx, viewport, "height", JS_NewInt32(ctx, viewportHeight_));
    JS_SetPropertyStr(ctx, bro, "viewport", viewport);
    JS_SetPropertyStr(ctx, bro, "perf", perf);

    // Stash Engine pointer for C function callbacks
    JSValue ptrVal = JS_NewInt64(ctx, static_cast<int64_t>(
        reinterpret_cast<intptr_t>(this)));

    // __bro.showPanel(name)
    JS_SetPropertyStr(ctx, bro, "showPanel",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            if (!self || argc < 1) return JS_UNDEFINED;
            const char* name = JS_ToCString(cx, argv[0]);
            if (!name) return JS_UNDEFINED;
            self->showSystemPanel(name);
            JS_FreeCString(cx, name);
            return JS_UNDEFINED;
        }, 1, 0, 1, &ptrVal));

    // __bro.getPanels()
    JS_SetPropertyStr(ctx, bro, "getPanels",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            if (!self) return JS_NewArray(cx);
            JSValue arr = JS_NewArray(cx);
            uint32_t idx = 0;
            for (const auto& d : self->systemDocs_) {
                if (!d.tabLabel.empty()) {
                    JSValue obj = JS_NewObject(cx);
                    JS_SetPropertyStr(cx, obj, "name",
                        JS_NewString(cx, d.name.c_str()));
                    JS_SetPropertyStr(cx, obj, "tabLabel",
                        JS_NewString(cx, d.tabLabel.c_str()));
                    JS_SetPropertyUint32(cx, arr, idx++, obj);
                }
            }
            return arr;
        }, 0, 0, 1, &ptrVal));

    // __bro.getActivePanel()
    JS_SetPropertyStr(ctx, bro, "getActivePanel",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            if (!self) return JS_UNDEFINED;
            return JS_NewString(cx, self->systemActivePanel_.c_str());
        }, 0, 0, 1, &ptrVal));

    // __bro.getMenu() — returns the menu tree as a parsed JS array.
    JS_SetPropertyStr(ctx, bro, "getMenu",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            if (!self) return JS_NewArray(cx);
            std::string json = self->menuBar().toJSON();
            return JS_ParseJSON(cx, json.c_str(), json.size(), "<menu>");
        }, 0, 0, 1, &ptrVal));

    // __bro.menuClick(id) — dispatch menu action.
    JS_SetPropertyStr(ctx, bro, "menuClick",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            if (!self || argc < 1) return JS_UNDEFINED;
            const char* id = JS_ToCString(cx, argv[0]);
            if (!id) return JS_UNDEFINED;
            self->triggerMenuAction(id);
            JS_FreeCString(cx, id);
            return JS_UNDEFINED;
        }, 1, 0, 1, &ptrVal));

    // __bro.toggleSettings() — open/close settings overlay.
    JS_SetPropertyStr(ctx, bro, "toggleSettings",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            if (self) self->toggleSystemSettings();
            return JS_UNDEFINED;
        }, 0, 0, 1, &ptrVal));

    // __bro.isSettingsVisible()
    JS_SetPropertyStr(ctx, bro, "isSettingsVisible",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            return JS_NewBool(cx, self && self->isSystemVisible());
        }, 0, 0, 1, &ptrVal));

    JS_SetPropertyStr(ctx, global, "__bro", bro);
    doc.broPerfObj = JS_DupValue(ctx, perf);
    JS_FreeValue(ctx, global);
}

// ---------------------------------------------------------------------------
// Menu actions + re-render notification
// ---------------------------------------------------------------------------

void Engine::triggerMenuAction(const std::string& id) {
    if (id == "__system.preferences") { toggleSystemSettings(); return; }
    if (id == "__system.quit") { running_ = false; return; }
    menuBar_.triggerHandler(id);
}

void Engine::onMenuChanged() {
    systemDirty_ = true;
    // Visibility may have flipped — the app doc's usable height depends on
    // contentTop(). Re-run the resize path so innerHeight, layout, scroll
    // clamp, and the resize event all update in lockstep.
    handleResize(viewportWidth_, viewportHeight_);
    for (auto& doc : systemDocs_) {
        if (doc.group != "menu" || !doc.jsCtx) continue;
        JSContext* ctx = doc.jsCtx;
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue fn = JS_GetPropertyStr(ctx, global, "__onMenuChanged");
        if (JS_IsFunction(ctx, fn)) {
            JSValue result = JS_Call(ctx, fn, global, 0, nullptr);
            if (JS_IsException(result)) {
                JSValue ex = JS_GetException(ctx);
                const char* s = JS_ToCString(ctx, ex);
                if (s) { LOG_ERROR("__onMenuChanged: %s", s); JS_FreeCString(ctx, s); }
                JS_FreeValue(ctx, ex);
            }
            JS_FreeValue(ctx, result);
        }
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, global);
    }
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
    return false;
}

bool Engine::isSystemVisible() const {
    return systemPerfVisible_ || systemSettingsVisible_ || menuBar_.visible;
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
    if (!isSystemVisible()) return;

    for (auto& doc : systemDocs_) {
        if (!isSystemDocVisible(doc) || !doc.timers) continue;
        doc.timers->tick(nowMs);
        doc.timers->fireAnimationFrames(nowMs);
    }

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
    for (auto& doc : systemDocs_) {
        if (JS_IsUndefined(doc.broPerfObj) || !doc.jsCtx) continue;
        JSContext* ctx = doc.jsCtx;

        JS_SetPropertyStr(ctx, doc.broPerfObj, "fps", JS_NewFloat64(ctx, fps));
        JS_SetPropertyStr(ctx, doc.broPerfObj, "frameTime", JS_NewFloat64(ctx, frameTime));
        JS_SetPropertyStr(ctx, doc.broPerfObj, "js", JS_NewFloat64(ctx, js));
        JS_SetPropertyStr(ctx, doc.broPerfObj, "layout", JS_NewFloat64(ctx, layout));
        JS_SetPropertyStr(ctx, doc.broPerfObj, "raster", JS_NewFloat64(ctx, raster));
        JS_SetPropertyStr(ctx, doc.broPerfObj, "gpu", JS_NewFloat64(ctx, gpu));
        JS_SetPropertyStr(ctx, doc.broPerfObj, "draw", JS_NewFloat64(ctx, draw));

        JSValue global = JS_GetGlobalObject(ctx);
        JSValue bro = JS_GetPropertyStr(ctx, global, "__bro");
        JSValue viewport = JS_GetPropertyStr(ctx, bro, "viewport");
        JS_SetPropertyStr(ctx, viewport, "width", JS_NewInt32(ctx, vpW));
        JS_SetPropertyStr(ctx, viewport, "height", JS_NewInt32(ctx, vpH));
        JS_FreeValue(ctx, viewport);
        JS_FreeValue(ctx, bro);
        JS_FreeValue(ctx, global);
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void Engine::renderSystemPanels() {
    if (!isSystemVisible() || !systemRenderer_ || !systemDirty_) return;

    // Re-layout dirty visible panels
    for (auto& doc : systemDocs_) {
        if (!isSystemDocVisible(doc) || !doc.document) continue;
        if (doc.document->isDirty()) {
            layout::SkiaTextMetrics textMetrics(systemRenderer_.get(), doc.fontManager.get());
            doc.document->resolveStyles();
            // performLayout() rebuilds the persistent layout tree when
            // structureDirty_ is set and clears the flag itself.
            doc.document->performLayout(static_cast<float>(viewportWidth_), textMetrics);
            doc.document->clearDirty();
        }
    }

    systemRenderer_->beginFrame(viewportWidth_, viewportHeight_);

    for (auto& doc : systemDocs_) {
        if (!isSystemDocVisible(doc) || !doc.document || !doc.drawTraversal) continue;
        doc.drawTraversal->draw(doc.document->documentElement(), 0, 0,
                                viewportWidth_, viewportHeight_);
    }

    // Draw the active system-context overlay (if any) on top of panels.
    overlayMgr_.drawIfContext(OverlayContext::System, systemRenderer_.get());

    systemRenderer_->endFrame();
    systemRenderer_->uploadToGPU();
    systemDirty_ = false;
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

void Engine::resizeSystemPanels(int w, int h) {
    systemDirty_ = true;
    for (auto& doc : systemDocs_) {
        if (doc.drawTraversal) {
            doc.drawTraversal->setViewport(w, h);
        }
        if (doc.document) {
            layout::SkiaTextMetrics textMetrics(systemRenderer_.get(), doc.fontManager.get());
            doc.document->resolveStyles();
            doc.document->performLayout(static_cast<float>(w), textMetrics);
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

    // Check if click is inside an open dropdown in any panel
    systemMouseConsumed_ = false;
    for (auto& doc : systemDocs_) {
        if (!isSystemDocVisible(doc) || !doc.document) continue;
        ControlContext cctx{doc.document.get(), doc.jsCtx,
                           systemRenderer_.get(), window_.get(), &systemDirty_,
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

    // Hit-test panels in reverse (last rendered = on top)
    for (int i = static_cast<int>(systemDocs_.size()) - 1; i >= 0; i--) {
        auto& doc = systemDocs_[i];
        if (!isSystemDocVisible(doc)) continue;
        dom::Element* target = systemHitTest(doc, x, y);
        if (target) {
            ControlContext cctx{doc.document.get(), doc.jsCtx,
                               systemRenderer_.get(), window_.get(), &systemDirty_,
                               &overlayMgr_, OverlayContext::System,
                               viewportWidth_, viewportHeight_};

            auto* prevActive = doc.document->activeElement();
            doc.document->setActiveElement(target);
            if (target != prevActive) {
                bro::engine::dispatchFocusEvents(cctx, prevActive, target);
            }

            focusNewControl(cctx, target, x, y);

            dom::MouseEvent evt("mousedown");
            evt.setClientX(static_cast<double>(x));
            evt.setClientY(static_cast<double>(y));
            evt.setButton(button);
            js::dispatchDomEvent(doc.jsCtx, target, evt);
            systemDirty_ = true;
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
            dom::MouseEvent upEvt("mouseup");
            upEvt.setClientX(static_cast<double>(x));
            upEvt.setClientY(static_cast<double>(y));
            upEvt.setButton(button);
            js::dispatchDomEvent(doc.jsCtx, target, upEvt);

            dom::MouseEvent clickEvt("click");
            clickEvt.setClientX(static_cast<double>(x));
            clickEvt.setClientY(static_cast<double>(y));
            clickEvt.setButton(button);
            js::dispatchDomEvent(doc.jsCtx, target, clickEvt);

            // End range slider dragging
            if (doc.document) {
                auto* activeEl = doc.document->activeElement();
                auto* input = getElInput(activeEl);
                if (input && input->isDragging()) {
                    input->setDragging(false);
                    ControlContext cctx{doc.document.get(), doc.jsCtx,
                                       systemRenderer_.get(), window_.get(), &systemDirty_,
                                       &overlayMgr_, OverlayContext::System,
                                       viewportWidth_, viewportHeight_};
                    dom::Event changeEvt("change");
                    dispatchControlEvent(cctx, activeEl, changeEvt);
                }
            }

            systemDirty_ = true;
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

    if (newTarget != systemHoverTarget_) {
        if (systemHoverTarget_ && systemHoverDoc_) {
            dom::MouseEvent leaveEvt("mouseleave");
            leaveEvt.setClientX(static_cast<double>(x));
            leaveEvt.setClientY(static_cast<double>(y));
            js::dispatchDomEvent(systemHoverDoc_->jsCtx, systemHoverTarget_, leaveEvt);
        }
        if (newTarget && newDoc) {
            dom::MouseEvent enterEvt("mouseenter");
            enterEvt.setClientX(static_cast<double>(x));
            enterEvt.setClientY(static_cast<double>(y));
            js::dispatchDomEvent(newDoc->jsCtx, newTarget, enterEvt);
        }
        systemHoverTarget_ = newTarget;
        systemHoverDoc_ = newDoc;
        systemDirty_ = true;
    }

    if (newTarget && newDoc) {
        dom::MouseEvent moveEvt("mousemove");
        moveEvt.setClientX(static_cast<double>(x));
        moveEvt.setClientY(static_cast<double>(y));
        js::dispatchDomEvent(newDoc->jsCtx, newTarget, moveEvt);
        return true;
    }
    return false;
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
