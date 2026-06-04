// Engine system panel methods — split from engine.cpp for readability.
// These are Engine member function implementations, not a separate class.

#include "engine/engine.h"
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
#include "dom/event.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "js/canvas_bindings.h"
#include "js/image_bindings.h"
#include "js/imagebitmap_bindings.h"
#include "js/event_dispatch.h"
#include "js/window_bindings.h"
#include "js/settings_bindings.h"
#include "canvas/canvas_scene.h"
#include "platform/sdl_window.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkSurface.h>

#include "api/api.h"
#include "util/log.h"
#include "util/time.h"

#include <algorithm>
#include <filesystem>
#include <functional>
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

        if (doc.jsCtx) {
            JS_FreeContext(doc.jsCtx);
            doc.jsCtx = nullptr;
        }
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

    // All panels are in systemDocs_ — fire __onPanelsReady on each so scripts
    // that query the panel list (e.g. the preferences nav building tabs) see
    // the full set rather than only the panels loaded before them.
    for (auto& doc : systemDocs_) {
        if (!doc.jsCtx) continue;
        JSContext* ctx = doc.jsCtx;
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue fn = JS_GetPropertyStr(ctx, global, "__onPanelsReady");
        if (JS_IsFunction(ctx, fn)) {
            JSValue r = JS_Call(ctx, fn, global, 0, nullptr);
            if (JS_IsException(r)) {
                JSValue ex = JS_GetException(ctx);
                const char* s = JS_ToCString(ctx, ex);
                if (s) { LOG_ERROR("__onPanelsReady: %s", s); JS_FreeCString(ctx, s); }
                JS_FreeValue(ctx, ex);
            }
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, global);
    }
}

void Engine::scanSystemPanelDir(const std::string& baseDir, const std::string& relPath) {
    std::error_code ec;
    std::string dirPath = relPath.empty() ? baseDir : baseDir + "/" + relPath;

    for (const auto& entry : fs::directory_iterator(dirPath, ec)) {
        if (entry.is_directory()) {
            // A subdirectory containing its own bro.json is a self-contained
            // bro app shipped under system/ (e.g. system/projects/, the
            // built-in project manager, and system/skeletons/<name>/), not
            // an overlay panel. Skip it so its DOM/JS aren't loaded twice.
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
        doc.document->parse(html, authorStyles, kDefaultStyles);

        // Create a dedicated JSContext on the shared runtime
        doc.jsCtx = jsRuntime_->createContext();

        // Install standard bindings
        brokit::api::installConsole(doc.jsCtx);
        doc.timers = std::make_unique<js::Timers>();
        js::Timers::install(doc.jsCtx, doc.timers.get());

        // Install window bindings: sets window = globalThis, plus the
        // addEventListener/dispatchEvent polyfill so panels can listen to
        // window-level events (keydown capture for rebind UIs, etc.).
        js::installWindowBindings(doc.jsCtx, viewportWidth_, viewportHeight_);

        // Install DOM bindings
        js::DomBindings::install(doc.jsCtx, doc.document.get());

        // Install settings bindings if available
        if (settings_) {
            js::SettingsBindings::install(doc.jsCtx, settings_.get(), window_.get());
        }

        // Install Canvas 2D bindings on this panel's context. The getContext
        // factory is registered after the doc is pushed into systemDocs_ below,
        // so the factory can own the created CanvasScenes per-panel.
        js::CanvasBindings::install(doc.jsCtx);
        js::ImageBindings::install(doc.jsCtx, savedBasePath);
        js::ImageBitmapBindings::install(doc.jsCtx);

        // Install __bro perf/nav object
        installBroObject(doc);

        // Initial layout — shared engine text metrics (same renderer/fontManager
        // as the app document; system panels are just additional documents).
        doc.document->resolveStyles();
        doc.document->performLayout(static_cast<float>(viewportWidth_),
                                    static_cast<float>(viewportHeight_),
                                    *textMetrics_);

        LOG_INFO("System panels: loaded panel '%s'", doc.name.c_str());

        // Push into the vector *before* executing scripts so the canvas
        // getContext factory can resolve this panel's SystemDocument slot by
        // index and park CanvasScenes in `doc.canvasScenes`.
        systemDocs_.push_back(std::move(doc));
        size_t docIdx = systemDocs_.size() - 1;
        auto& liveDoc = systemDocs_[docIdx];

        // Canvas getContext factory — captures `this` + the panel's index so
        // it finds the right doc even if the vector reallocates as later
        // panels load. Only 2D is wired; WebGL/scene need GL compositing and
        // are not supported in system panels.
        js::DomBindings::setGetContextFactory(liveDoc.jsCtx,
            [this, docIdx](JSContext* fctx, dom::Element* el,
                           const std::string& type) -> JSValue {
                if (type != "2d") return JS_NULL;
                if (docIdx >= systemDocs_.size()) return JS_NULL;
                auto& d = systemDocs_[docIdx];
                auto scene = std::make_unique<canvas::CanvasScene>(renderer_.get());
                scene->init(nullptr);
                if (el) {
                    scene->setLayoutCallback(
                        [](void* ud, float& ox, float& oy, float& ow, float& oh) {
                            auto* elem = static_cast<dom::Element*>(ud);
                            if (!elem->parentNode()) { ox = oy = ow = oh = 0; return; }
                            auto& box = elem->layoutBox();
                            ox = box.contentRect.x;
                            oy = box.contentRect.y;
                            for (auto* lp = elem->layoutParent(); lp; lp = lp->layoutParent()) {
                                auto& pb = lp->layoutBox();
                                ox += pb.contentRect.x;
                                oy += pb.contentRect.y;
                                oy -= lp->scrollTopValue();
                            }
                            ow = box.contentRect.width;
                            oh = box.contentRect.height;
                        }, el);
                    scene->setDetachedCallback([](void* ud) -> bool {
                        auto* n = static_cast<dom::Element*>(ud);
                        while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                        return n->tagName() != "html" && n->tagName() != "HTML";
                    }, el);
                }
                auto* ptr = scene.get();
                if (el) el->setCanvasScene(ptr);
                d.canvasScenes.push_back(std::move(scene));
                return js::CanvasBindings::wrapContext2D(fctx, ptr);
            });

        // Extract and execute inline scripts
        {
            std::regex scriptRe(R"(<script[^>]*>([\s\S]*?)</script>)",
                                std::regex_constants::icase);
            auto begin = std::sregex_iterator(html.begin(), html.end(), scriptRe);
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it) {
                std::string code = (*it)[1].str();
                if (!code.empty()) {
                    std::string filename = "<system/" + liveDoc.name + ">";
                    JSValue result = JS_Eval(liveDoc.jsCtx, code.c_str(), code.size(),
                                             filename.c_str(), JS_EVAL_TYPE_GLOBAL);
                    if (JS_IsException(result)) {
                        JSValue ex = JS_GetException(liveDoc.jsCtx);
                        const char* str = JS_ToCString(liveDoc.jsCtx, ex);
                        if (str) {
                            LOG_ERROR("System panel JS error in '%s': %s",
                                      liveDoc.name.c_str(), str);
                            JS_FreeCString(liveDoc.jsCtx, str);
                        }
                        JS_FreeValue(liveDoc.jsCtx, ex);
                    }
                    JS_FreeValue(liveDoc.jsCtx, result);
                }
            }
        }

        // Initialize replaced elements after scripts
        bro::engine::ensureReplacedElements(liveDoc.document->documentElement(),
                                            renderer_.get(),
                                            liveDoc.jsCtx,
                                            audioEngine_.get());

        // Re-layout after scripts may have modified the DOM
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

    // __bro.getSettingsPanels() — enumerate settings panels as [{name, label}].
    // Used by the preferences nav to build tabs dynamically; apps add a panel
    // by dropping HTML at apps/<name>/system/settings/<panel>.html.
    JS_SetPropertyStr(ctx, bro, "getSettingsPanels",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            JSValue arr = JS_NewArray(cx);
            if (!self) return arr;
            uint32_t idx = 0;
            for (auto& d : self->systemDocs_) {
                if (d.group != "settings") continue;
                JSValue item = JS_NewObject(cx);
                JS_SetPropertyStr(cx, item, "name", JS_NewString(cx, d.name.c_str()));
                JS_SetPropertyStr(cx, item, "label", JS_NewString(cx, d.tabLabel.c_str()));
                JS_SetPropertyUint32(cx, arr, idx++, item);
            }
            return arr;
        }, 0, 0, 1, &ptrVal));

    // __bro.getViewport() — current viewport metrics for panels to size with.
    JS_SetPropertyStr(ctx, bro, "getViewport",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            JSValue o = JS_NewObject(cx);
            if (!self) return o;
            JS_SetPropertyStr(cx, o, "width", JS_NewInt32(cx, self->viewportWidth()));
            JS_SetPropertyStr(cx, o, "height", JS_NewInt32(cx, self->viewportHeight()));
            JS_SetPropertyStr(cx, o, "contentTop", JS_NewInt32(cx, self->contentTop()));
            return o;
        }, 0, 0, 1, &ptrVal));

    // __bro.dismissSplash() — called by system/splash.html after its swirl-away
    // animation finishes. Hides the splash panel so the app canvas is revealed.
    JS_SetPropertyStr(ctx, bro, "dismissSplash",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            if (self && self->splashVisible_) {
                self->splashVisible_ = false;
                self->systemDirty_ = true;
            }
            return JS_UNDEFINED;
        }, 0, 0, 1, &ptrVal));

    // ── Inspector API ───────────────────────────────────────────────────
    // Used by system/inspector.html to read/write inspector state and to
    // walk the app document tree.

    // __bro.getInspectorLayout() — { visible, dock, width, height,
    //     viewportWidth, viewportHeight, menuTop }.
    JS_SetPropertyStr(ctx, bro, "getInspectorLayout",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            JSValue o = JS_NewObject(cx);
            if (!self) return o;
            const auto& i = self->inspector();
            JS_SetPropertyStr(cx, o, "visible", JS_NewBool(cx, i.visible));
            JS_SetPropertyStr(cx, o, "dock",
                JS_NewString(cx, i.dock == InspectorDock::Right ? "right" : "bottom"));
            JS_SetPropertyStr(cx, o, "width", JS_NewInt32(cx, i.width));
            JS_SetPropertyStr(cx, o, "height", JS_NewInt32(cx, i.height));
            JS_SetPropertyStr(cx, o, "pickerMode", JS_NewBool(cx, i.pickerMode));
            JS_SetPropertyStr(cx, o, "viewportWidth", JS_NewInt32(cx, self->viewportWidth()));
            JS_SetPropertyStr(cx, o, "viewportHeight", JS_NewInt32(cx, self->viewportHeight()));
            JS_SetPropertyStr(cx, o, "menuTop", JS_NewInt32(cx, self->contentTop()));
            return o;
        }, 0, 0, 1, &ptrVal));

    // __bro.getAppDOMTree(maxDepth?) — full tree from documentElement, or
    // up to maxDepth levels deep. Resets the per-fetch nodeId map.
    JS_SetPropertyStr(ctx, bro, "getAppDOMTree",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            if (!self || !self->document() || !self->document()->documentElement())
                return JS_NULL;
            int32_t maxDepth = -1;
            if (argc >= 1 && JS_IsNumber(argv[0])) JS_ToInt32(cx, &maxDepth, argv[0]);
            return self->inspectorBuildTreeJS(cx, maxDepth);
        }, 1, 0, 1, &ptrVal));

    // __bro.getAppDOMChildren(nodeId) — one level of children.
    JS_SetPropertyStr(ctx, bro, "getAppDOMChildren",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            if (!self || argc < 1) return JS_NewArray(cx);
            int32_t id = -1;
            JS_ToInt32(cx, &id, argv[0]);
            return self->inspectorChildrenJS(cx, id);
        }, 1, 0, 1, &ptrVal));

    // __bro.inspectorSelect(nodeId)
    JS_SetPropertyStr(ctx, bro, "inspectorSelect",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            if (!self || argc < 1) return JS_UNDEFINED;
            int32_t id = -1;
            JS_ToInt32(cx, &id, argv[0]);
            self->inspectorSelectById(id);
            return JS_UNDEFINED;
        }, 1, 0, 1, &ptrVal));

    // __bro.inspectorSetDock("right"|"bottom")
    JS_SetPropertyStr(ctx, bro, "inspectorSetDock",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            if (!self || argc < 1) return JS_UNDEFINED;
            const char* s = JS_ToCString(cx, argv[0]);
            if (!s) return JS_UNDEFINED;
            self->inspectorSetDock(std::string(s) == "bottom"
                ? InspectorDock::Bottom : InspectorDock::Right);
            JS_FreeCString(cx, s);
            return JS_UNDEFINED;
        }, 1, 0, 1, &ptrVal));

    // __bro.inspectorSetSize(px)
    JS_SetPropertyStr(ctx, bro, "inspectorSetSize",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            if (!self || argc < 1) return JS_UNDEFINED;
            int32_t v = 0;
            JS_ToInt32(cx, &v, argv[0]);
            self->inspectorSetSize(v);
            return JS_UNDEFINED;
        }, 1, 0, 1, &ptrVal));

    // __bro.inspectorSetPickerMode(bool)
    JS_SetPropertyStr(ctx, bro, "inspectorSetPickerMode",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            if (!self || argc < 1) return JS_UNDEFINED;
            self->inspectorSetPickerMode(JS_ToBool(cx, argv[0]) != 0);
            return JS_UNDEFINED;
        }, 1, 0, 1, &ptrVal));

    // __bro.inspectorGetSelected() — { id, tag } | null. The id is from the
    // most recent getAppDOMTree() / getAppDOMChildren() fetch.
    JS_SetPropertyStr(ctx, bro, "inspectorGetSelected",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            if (!self) return JS_NULL;
            return self->inspectorSelectedJS(cx);
        }, 0, 0, 1, &ptrVal));

    // __bro.toggleInspector() — same effect as the View → Inspector menu.
    JS_SetPropertyStr(ctx, bro, "toggleInspector",
        JS_NewCFunctionData(ctx, [](JSContext* cx, JSValue thisVal,
                                    int argc, JSValue* argv, int magic,
                                    JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(cx, &p, fdata[0]);
            auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
            if (self) self->toggleInspector();
            return JS_UNDEFINED;
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
    if (id == "__system.inspector") { toggleInspector(); return; }
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

    // Tell the inspector panel JS to refresh (visibility / docking changed).
    for (auto& doc : systemDocs_) {
        if (doc.name != "inspector" || !doc.jsCtx) continue;
        JSContext* ctx = doc.jsCtx;
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue fn = JS_GetPropertyStr(ctx, global, "__onInspectorChanged");
        if (JS_IsFunction(ctx, fn)) {
            JSValue r = JS_Call(ctx, fn, global, 0, nullptr);
            if (JS_IsException(r)) {
                JSValue ex = JS_GetException(ctx);
                const char* s = JS_ToCString(ctx, ex);
                if (s) { LOG_ERROR("__onInspectorChanged: %s", s); JS_FreeCString(ctx, s); }
                JS_FreeValue(ctx, ex);
            }
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, global);
    }
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
    if (doc.group == "menu") return menuBar_.visible
        && displayMode_ != DisplayMode::Headless;
    if (doc.group == "inspector") return inspector_.visible;
    if (doc.group == "splash") return splashVisible_;
    return false;
}

bool Engine::isSystemVisible() const {
    const bool menuShown = menuBar_.visible
                        && displayMode_ != DisplayMode::Headless;
    return systemPerfVisible_ || systemSettingsVisible_ || menuShown
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
    // Splash lifecycle: after a minimum display time, tell the splash panel
    // to play its swirl-away animation. The panel's JS calls __bro.dismissSplash()
    // when the animation finishes. A hard fallback force-hides the splash if
    // something goes wrong so a broken splash never wedges the app.
    if (splashVisible_) {
        constexpr double kMinDisplayMs = 1800.0;
        constexpr double kHardTimeoutMs = 4500.0;
        double elapsed = nowMs - splashStartMs_;
        if (!splashDismissTriggered_ && elapsed >= kMinDisplayMs) {
            splashDismissTriggered_ = true;
            for (auto& doc : systemDocs_) {
                if (doc.group != "splash" || !doc.jsCtx) continue;
                JSContext* ctx = doc.jsCtx;
                JSValue global = JS_GetGlobalObject(ctx);
                JSValue fn = JS_GetPropertyStr(ctx, global, "__onDismiss");
                if (JS_IsFunction(ctx, fn)) {
                    JSValue r = JS_Call(ctx, fn, global, 0, nullptr);
                    if (JS_IsException(r)) {
                        JSValue ex = JS_GetException(ctx);
                        const char* s = JS_ToCString(ctx, ex);
                        if (s) { LOG_ERROR("splash __onDismiss: %s", s); JS_FreeCString(ctx, s); }
                        JS_FreeValue(ctx, ex);
                    }
                    JS_FreeValue(ctx, r);
                }
                JS_FreeValue(ctx, fn);
                JS_FreeValue(ctx, global);
            }
        }
        if (elapsed >= kHardTimeoutMs) {
            splashVisible_ = false;
            systemDirty_ = true;
        }
    }

    if (!isSystemVisible()) return;

    // Throttle rAF firing to the engine's UI render cadence. The main loop
    // can iterate much faster than the render pipeline actually produces
    // frames, and firing rAF every iteration just wastes JS work (the extra
    // canvas commands would be staged but never displayed at a different
    // rate than the raster thread can emit frames).
    bool fireRaf = (nowMs - lastSystemRafMs_) >= uiFrameIntervalMs_;
    if (fireRaf) lastSystemRafMs_ = nowMs;

    for (auto& doc : systemDocs_) {
        if (!isSystemDocVisible(doc) || !doc.timers) continue;
        doc.timers->tick(nowMs);
        if (fireRaf) doc.timers->fireAnimationFrames(nowMs);
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
// Layout + draw helpers used by both the raster thread (windowed) and main
// thread (headless). These route system panels through the same rendering
// pipeline as the app document — there is no separate system renderer.
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
        // Pass both dims so height:100% / viewport-relative CSS resolves (the
        // preferences modal relies on a full-viewport backdrop).
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

    // System panels want 2D canvas children composited inline (the canvas
    // *is* the panel). Emit a Cmd_BlitCanvasInline so the replayer performs
    // the snapshot+drawImageRect on the raster thread's Skia context — at
    // record time we don't have a target SkCanvas to draw onto, only a
    // recording renderer that's accumulating commands. If the renderer
    // happens to be a RecordingRenderer, route through it directly; otherwise
    // (headless's direct path) fall back to inline Skia work.
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
            // Direct path (no command buffer between caller and Skia).
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

    // System-context overlay (if any) on top of panels.
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
            doc.document->resolveStyles();
            doc.document->performLayout(static_cast<float>(w),
                                        static_cast<float>(h),
                                        *textMetrics_);
        }
        // Fire __onResize on the panel's JS context so scripts can reposition
        // JS-sized elements (e.g. preferences modal card, content panels).
        if (doc.jsCtx) {
            JSContext* ctx = doc.jsCtx;
            JSValue global = JS_GetGlobalObject(ctx);
            JSValue fn = JS_GetPropertyStr(ctx, global, "__onResize");
            if (JS_IsFunction(ctx, fn)) {
                JSValue r = JS_Call(ctx, fn, global, 0, nullptr);
                if (JS_IsException(r)) {
                    JSValue ex = JS_GetException(ctx);
                    const char* s = JS_ToCString(ctx, ex);
                    if (s) { LOG_ERROR("__onResize: %s", s); JS_FreeCString(ctx, s); }
                    JS_FreeValue(ctx, ex);
                }
                JS_FreeValue(ctx, r);
            }
            JS_FreeValue(ctx, fn);
            JS_FreeValue(ctx, global);
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

    // Scrollbar hit test before DOM hit-test so a drag on the thumb wins over
    // a click on the row underneath. Panels live in screen space (no viewport
    // scroll), so the offset starts at 0. If we find a hit we stamp the drag
    // target + owning doc so handleMouseMove / handleMouseUp in input_handling
    // know this drag is system-owned.
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
            scrollbarDragTarget_ = hitElem;
            scrollbarDragSystemDoc_ = &doc;
        } else {
            // Click on track — page scroll to the clicked position.
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

    // Hit-test panels in reverse (last rendered = on top).
    // Shared dispatcher does focus + mousedown with the same semantics as the
    // app doc path (see replaced_elements.cpp).
    for (int i = static_cast<int>(systemDocs_.size()) - 1; i >= 0; i--) {
        auto& doc = systemDocs_[i];
        if (!isSystemDocVisible(doc)) continue;
        dom::Element* target = systemHitTest(doc, x, y);
        if (target) {
            ControlContext cctx{doc.document.get(), doc.jsCtx,
                               renderer_.get(), window_.get(), &systemDirty_,
                               &overlayMgr_, OverlayContext::System,
                               viewportWidth_, viewportHeight_};

            dom::MouseEvent evt("mousedown");
            evt.setClientX(static_cast<double>(x));
            evt.setClientY(static_cast<double>(y));
            evt.setButton(button);
            evt.setIsTrusted(true);
            applyMouseOffset(evt, target);

            dispatchDocMousePress(cctx, doc.mouseState, target, evt, x, y);
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

        // Dispatch mouseup + click (if target matches mousedown target) using
        // the shared per-doc helper — same DOM semantics as the app path, so
        // stale mousedowns (e.g. overlay-consumed press) no longer produce
        // phantom clicks on rows beneath a just-dismissed dropdown.
        if (target) {
            ControlContext cctx{doc.document.get(), doc.jsCtx,
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

            // End range slider dragging (system-specific control state).
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
// Keyboard input (modal panels)
// ---------------------------------------------------------------------------
//
// When the settings modal is open the app should not see keystrokes at all —
// the modal is, well, modal. We dispatch to every visible settings / nav
// panel so each panel's own JS can handle keys in its own JSContext (capture
// listeners, tab navigation, etc.). Perf / splash / menu panels are not
// modal, so they don't capture keys.

// Returns true if any panel listener called preventDefault — signalling that
// engine-level shortcuts (like Esc-to-close) should be suppressed for this
// keystroke. The return value does NOT mean "was there a visible modal"; the
// caller already checks systemSettingsVisible_ before calling.
bool Engine::systemHandleKeyDown(int keycode, int scancode, int mod, bool repeat) {
    if (!systemSettingsVisible_) return false;
    bool prevented = false;
    for (auto& doc : systemDocs_) {
        if (doc.group != "nav" && doc.group != "settings") continue;
        if (!isSystemDocVisible(doc) || !doc.document || !doc.jsCtx) continue;
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
        js::dispatchDomEvent(doc.jsCtx, body, evt);
        if (evt.defaultPrevented()) prevented = true;
    }
    return prevented;
}

bool Engine::systemHandleWheel(float x, float y, float dx, float dy) {
    if (!isSystemVisible()) return false;
    // Walk panels top-down (reverse render order) for the hit-test, then walk
    // the hit element's composed ancestors looking for a scrollable overflow
    // box. Matches browser behavior where wheel bubbles to the nearest
    // scrollable ancestor.
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
        // Even when no scrollable ancestor exists, a modal panel should still
        // swallow the wheel event — otherwise the app behind the modal scrolls.
        if (systemSettingsVisible_) return true;
    }
    return false;
}

bool Engine::systemHandleKeyUp(int keycode, int scancode, int mod, bool repeat) {
    if (!systemSettingsVisible_) return false;
    bool prevented = false;
    for (auto& doc : systemDocs_) {
        if (doc.group != "nav" && doc.group != "settings") continue;
        if (!isSystemDocVisible(doc) || !doc.document || !doc.jsCtx) continue;
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
        js::dispatchDomEvent(doc.jsCtx, body, evt);
        if (evt.defaultPrevented()) prevented = true;
    }
    return prevented;
}

// ---------------------------------------------------------------------------
// Inspector DOM tree helpers
// ---------------------------------------------------------------------------

namespace {

// Walks parents up to a known root. Used to validate that a stashed element
// pointer still belongs to the live document tree before we dereference it.
bool elementInTree(dom::Element* el, dom::Element* root) {
    while (el) {
        if (el == root) return true;
        el = el->parentElement();
    }
    return false;
}

JSValue makeNodeJS(JSContext* ctx, int id, dom::Element* el, bool hasChildren) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "id", JS_NewInt32(ctx, id));
    JS_SetPropertyStr(ctx, o, "tag", JS_NewString(ctx, el->tagName().c_str()));
    std::string idAttr = el->getAttribute("id");
    JS_SetPropertyStr(ctx, o, "idAttr", JS_NewString(ctx, idAttr.c_str()));
    std::string cls = el->getAttribute("class");
    JS_SetPropertyStr(ctx, o, "classes", JS_NewString(ctx, cls.c_str()));
    JS_SetPropertyStr(ctx, o, "hasChildren", JS_NewBool(ctx, hasChildren));
    return o;
}

} // namespace

JSValue Engine::inspectorBuildTreeJS(JSContext* ctx, int maxDepth) {
    inspectorNodeMap_.clear();
    inspectorNextId_ = 0;
    auto* root = document_ ? document_->documentElement() : nullptr;
    if (!root) return JS_NULL;

    // Recursive walk. Each visited element gets a fresh integer id mapped to
    // its pointer so later inspectorSelect() calls can resolve back to it.
    std::function<JSValue(dom::Element*, int)> walk;
    walk = [&](dom::Element* el, int depth) -> JSValue {
        int id = inspectorNextId_++;
        inspectorNodeMap_[id] = el;
        auto kids = el->children();
        bool emitChildren = (maxDepth < 0 || depth < maxDepth);
        JSValue node = makeNodeJS(ctx, id, el, !kids.empty());
        if (emitChildren && !kids.empty()) {
            JSValue arr = JS_NewArray(ctx);
            uint32_t i = 0;
            for (auto* k : kids) {
                if (!k) continue;
                JS_SetPropertyUint32(ctx, arr, i++, walk(k, depth + 1));
            }
            JS_SetPropertyStr(ctx, node, "children", arr);
        }
        return node;
    };
    return walk(root, 0);
}

JSValue Engine::inspectorChildrenJS(JSContext* ctx, int parentId) {
    auto it = inspectorNodeMap_.find(parentId);
    JSValue arr = JS_NewArray(ctx);
    if (it == inspectorNodeMap_.end() || !it->second) return arr;
    auto* root = document_ ? document_->documentElement() : nullptr;
    if (!root || !elementInTree(it->second, root)) return arr;
    auto kids = it->second->children();
    uint32_t i = 0;
    for (auto* k : kids) {
        if (!k) continue;
        int id = inspectorNextId_++;
        inspectorNodeMap_[id] = k;
        JS_SetPropertyUint32(ctx, arr, i++,
            makeNodeJS(ctx, id, k, !k->children().empty()));
    }
    return arr;
}

void Engine::inspectorSelectById(int id) {
    auto it = inspectorNodeMap_.find(id);
    if (it == inspectorNodeMap_.end()) return;
    auto* root = document_ ? document_->documentElement() : nullptr;
    if (!root || !elementInTree(it->second, root)) return;
    inspector_.selected = it->second;
    systemDirty_ = true;
}

JSValue Engine::inspectorSelectedJS(JSContext* ctx) {
    auto* el = inspector_.selected;
    auto* root = document_ ? document_->documentElement() : nullptr;
    if (!el || !root || !elementInTree(el, root)) {
        inspector_.selected = nullptr;
        return JS_NULL;
    }
    // Find the id (if any) currently mapped to this element. This lets the
    // panel UI keep its tree row highlighted across re-fetches.
    int id = -1;
    for (auto& [k, v] : inspectorNodeMap_) {
        if (v == el) { id = k; break; }
    }
    return makeNodeJS(ctx, id, el, !el->children().empty());
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
