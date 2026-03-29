#include "engine/system_overlay.h"
#include "engine/app_loader.h"
#include "render/renderer.h"
#include "layout/container.h"
#include "dom/document.h"
#include "dom/element.h"
#include "js/console.h"
#include "js/timers.h"
#include "util/log.h"

#include <filesystem>
#include <regex>

extern "C" {
#include "quickjs.h"
}

namespace fs = std::filesystem;

namespace bro::engine {

// ---------------------------------------------------------------------------
// Minimal DOM bindings for system panels
// ---------------------------------------------------------------------------

static JSClassID s_elementProxyClassId = 0;
static JSClassID s_styleProxyClassId = 0;

struct ElementProxyData {
    dom::Element* element;
    dom::Document* document;
};

struct StyleProxyData {
    dom::Element* element;
    dom::Document* document;
};

// --- Style proxy ---

static JSValue style_proxy_set(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    auto* data = static_cast<StyleProxyData*>(
        JS_GetOpaque(this_val, s_styleProxyClassId));
    if (!data || !data->element || argc < 2) return JS_UNDEFINED;

    const char* prop = JS_ToCString(ctx, argv[0]);
    const char* val = JS_ToCString(ctx, argv[1]);
    if (prop && val) {
        // Convert camelCase to kebab-case
        std::string kebab = dom::StyleProxy::camelToKebab(prop);
        data->element->style().setProperty(kebab, val);
        data->element->syncStylesToLitehtml();
    }
    if (prop) JS_FreeCString(ctx, prop);
    if (val) JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

static JSValue style_proxy_get_prop(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* data = static_cast<StyleProxyData*>(
        JS_GetOpaque(this_val, s_styleProxyClassId));
    if (!data || !data->element || argc < 1) return JS_UNDEFINED;

    const char* prop = JS_ToCString(ctx, argv[0]);
    if (!prop) return JS_UNDEFINED;
    std::string kebab = dom::StyleProxy::camelToKebab(prop);
    std::string val = data->element->style().getProperty(kebab);
    JS_FreeCString(ctx, prop);
    return JS_NewString(ctx, val.c_str());
}

// Use a Proxy-like approach: style.width = "10px" via JS setter magic
// We'll use a JS wrapper instead: __bro_style_set(elem_proxy, "width", "10px")
// But for a nicer API, we install the style as an object with set() method
// and wrap it in JS to intercept property assignments.

static void style_proxy_finalizer(JSRuntime*, JSValue val) {
    auto* data = static_cast<StyleProxyData*>(
        JS_GetOpaque(val, s_styleProxyClassId));
    delete data;
}

static JSClassDef styleProxyClassDef = {
    "SystemStyleProxy",
    style_proxy_finalizer,
    nullptr, nullptr, nullptr
};

// --- Element proxy ---

static JSValue elem_get_textContent(JSContext* ctx, JSValueConst this_val) {
    auto* data = static_cast<ElementProxyData*>(
        JS_GetOpaque(this_val, s_elementProxyClassId));
    if (!data || !data->element) return JS_UNDEFINED;
    std::string text = data->element->textContent();
    return JS_NewString(ctx, text.c_str());
}

static JSValue elem_set_textContent(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* data = static_cast<ElementProxyData*>(
        JS_GetOpaque(this_val, s_elementProxyClassId));
    if (!data || !data->element) return JS_UNDEFINED;
    const char* str = JS_ToCString(ctx, val);
    if (str) {
        data->element->setTextContent(str);
        JS_FreeCString(ctx, str);
    }
    return JS_UNDEFINED;
}

static JSValue elem_get_className(JSContext* ctx, JSValueConst this_val) {
    auto* data = static_cast<ElementProxyData*>(
        JS_GetOpaque(this_val, s_elementProxyClassId));
    if (!data || !data->element) return JS_UNDEFINED;
    return JS_NewString(ctx, data->element->className().c_str());
}

static JSValue elem_set_className(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* data = static_cast<ElementProxyData*>(
        JS_GetOpaque(this_val, s_elementProxyClassId));
    if (!data || !data->element) return JS_UNDEFINED;
    const char* str = JS_ToCString(ctx, val);
    if (str) {
        data->element->setClassName(str);
        JS_FreeCString(ctx, str);
    }
    return JS_UNDEFINED;
}

static JSValue elem_get_innerHTML(JSContext* ctx, JSValueConst this_val) {
    auto* data = static_cast<ElementProxyData*>(
        JS_GetOpaque(this_val, s_elementProxyClassId));
    if (!data || !data->element) return JS_UNDEFINED;
    return JS_NewString(ctx, data->element->innerHTML().c_str());
}

static JSValue elem_set_innerHTML(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* data = static_cast<ElementProxyData*>(
        JS_GetOpaque(this_val, s_elementProxyClassId));
    if (!data || !data->element) return JS_UNDEFINED;
    const char* str = JS_ToCString(ctx, val);
    if (str) {
        data->element->setInnerHTML(str);
        JS_FreeCString(ctx, str);
    }
    return JS_UNDEFINED;
}

static void elem_proxy_finalizer(JSRuntime*, JSValue val) {
    auto* data = static_cast<ElementProxyData*>(
        JS_GetOpaque(val, s_elementProxyClassId));
    delete data;
}

static JSClassDef elementProxyClassDef = {
    "SystemElementProxy",
    elem_proxy_finalizer,
    nullptr, nullptr, nullptr
};

static JSValue wrap_element_proxy(JSContext* ctx, dom::Element* elem, dom::Document* doc) {
    if (!elem) return JS_NULL;

    // Create element proxy
    JSValue obj = JS_NewObjectClass(ctx, s_elementProxyClassId);
    auto* edata = new ElementProxyData{elem, doc};
    JS_SetOpaque(obj, edata);

    // Create style sub-object with set/get methods
    JSValue styleObj = JS_NewObjectClass(ctx, s_styleProxyClassId);
    auto* sdata = new StyleProxyData{elem, doc};
    JS_SetOpaque(styleObj, sdata);
    JS_SetPropertyStr(ctx, styleObj, "setProperty",
                      JS_NewCFunction(ctx, style_proxy_set, "setProperty", 2));
    JS_SetPropertyStr(ctx, styleObj, "getPropertyValue",
                      JS_NewCFunction(ctx, style_proxy_get_prop, "getPropertyValue", 1));
    JS_SetPropertyStr(ctx, obj, "style", styleObj);

    return obj;
}

// document.getElementById
static JSValue sys_getElementById(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    const char* id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_NULL;

    // The document pointer is stored as opaque data on the document object
    auto* panels = static_cast<std::vector<SystemOverlay::Panel>*>(
        JS_GetOpaque(this_val, 0));

    // Search all panels for the element
    dom::Element* found = nullptr;
    dom::Document* foundDoc = nullptr;
    if (panels) {
        for (auto& panel : *panels) {
            if (panel.document) {
                found = panel.document->getElementById(id);
                if (found) {
                    foundDoc = panel.document.get();
                    break;
                }
            }
        }
    }

    JS_FreeCString(ctx, id);
    if (!found) return JS_NULL;
    return wrap_element_proxy(ctx, found, foundDoc);
}

// ---------------------------------------------------------------------------
// SystemOverlay implementation
// ---------------------------------------------------------------------------

SystemOverlay::SystemOverlay(render::Renderer* renderer, int vpW, int vpH)
    : renderer_(renderer)
    , viewportWidth_(vpW)
    , viewportHeight_(vpH) {

    // Create isolated JS environment
    jsRt_ = JS_NewRuntime();
    JS_SetMemoryLimit(jsRt_, 32 * 1024 * 1024); // 32 MB
    JS_SetMaxStackSize(jsRt_, 512 * 1024);

    jsCtx_ = JS_NewContext(jsRt_);

    // Register class IDs (safe because these are unique statics)
    if (s_elementProxyClassId == 0) {
        JS_NewClassID(jsRt_, &s_elementProxyClassId);
    }
    if (s_styleProxyClassId == 0) {
        JS_NewClassID(jsRt_, &s_styleProxyClassId);
    }
    JS_NewClass(jsRt_, s_elementProxyClassId, &elementProxyClassDef);
    JS_NewClass(jsRt_, s_styleProxyClassId, &styleProxyClassDef);

    // Set up element proxy prototype with getters/setters
    JSValue elemProto = JS_NewObject(jsCtx_);
    JS_DefinePropertyGetSet(jsCtx_, elemProto,
        JS_NewAtom(jsCtx_, "textContent"),
        JS_NewCFunction(jsCtx_, (JSCFunction*)elem_get_textContent, "get textContent", 0),
        JS_NewCFunction(jsCtx_, (JSCFunction*)elem_set_textContent, "set textContent", 1),
        JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(jsCtx_, elemProto,
        JS_NewAtom(jsCtx_, "className"),
        JS_NewCFunction(jsCtx_, (JSCFunction*)elem_get_className, "get className", 0),
        JS_NewCFunction(jsCtx_, (JSCFunction*)elem_set_className, "set className", 1),
        JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(jsCtx_, elemProto,
        JS_NewAtom(jsCtx_, "innerHTML"),
        JS_NewCFunction(jsCtx_, (JSCFunction*)elem_get_innerHTML, "get innerHTML", 0),
        JS_NewCFunction(jsCtx_, (JSCFunction*)elem_set_innerHTML, "set innerHTML", 1),
        JS_PROP_CONFIGURABLE);
    JS_SetClassProto(jsCtx_, s_elementProxyClassId, elemProto);

    // Install console + timers
    js::Console::install(jsCtx_);
    timers_ = std::make_unique<js::Timers>();
    js::Timers::install(jsCtx_, timers_.get());

    // Install __bro object
    installBroObject();

    // Install minimal document object
    installMinimalBindings();
}

SystemOverlay::~SystemOverlay() {
    // Clear panels before JS cleanup (they hold litehtml docs)
    panels_.clear();

    if (timers_ && jsCtx_) {
        timers_->clearAll(jsCtx_);
    }
    timers_.reset();

    if (!JS_IsUndefined(broPerfObj_)) {
        JS_FreeValue(jsCtx_, broPerfObj_);
    }

    if (jsCtx_) {
        JS_FreeContext(jsCtx_);
        jsCtx_ = nullptr;
    }
    if (jsRt_) {
        JS_FreeRuntime(jsRt_);
        jsRt_ = nullptr;
    }
}

void SystemOverlay::installBroObject() {
    JSValue global = JS_GetGlobalObject(jsCtx_);

    JSValue bro = JS_NewObject(jsCtx_);
    JSValue perf = JS_NewObject(jsCtx_);

    JS_SetPropertyStr(jsCtx_, perf, "fps", JS_NewFloat64(jsCtx_, 0.0));
    JS_SetPropertyStr(jsCtx_, perf, "frameTime", JS_NewFloat64(jsCtx_, 0.0));
    JS_SetPropertyStr(jsCtx_, perf, "js", JS_NewFloat64(jsCtx_, 0.0));
    JS_SetPropertyStr(jsCtx_, perf, "layout", JS_NewFloat64(jsCtx_, 0.0));
    JS_SetPropertyStr(jsCtx_, perf, "raster", JS_NewFloat64(jsCtx_, 0.0));
    JS_SetPropertyStr(jsCtx_, perf, "gpu", JS_NewFloat64(jsCtx_, 0.0));
    JS_SetPropertyStr(jsCtx_, perf, "draw", JS_NewFloat64(jsCtx_, 0.0));

    JSValue viewport = JS_NewObject(jsCtx_);
    JS_SetPropertyStr(jsCtx_, viewport, "width", JS_NewInt32(jsCtx_, viewportWidth_));
    JS_SetPropertyStr(jsCtx_, viewport, "height", JS_NewInt32(jsCtx_, viewportHeight_));
    JS_SetPropertyStr(jsCtx_, bro, "viewport", viewport);

    JS_SetPropertyStr(jsCtx_, bro, "perf", perf);
    JS_SetPropertyStr(jsCtx_, global, "__bro", bro);

    // Keep a reference to perf for fast updates
    broPerfObj_ = JS_DupValue(jsCtx_, perf);

    JS_FreeValue(jsCtx_, global);
}

void SystemOverlay::installMinimalBindings() {
    JSValue global = JS_GetGlobalObject(jsCtx_);

    // Create document object
    JSValue doc = JS_NewObject(jsCtx_);

    // Store panels pointer as opaque data on the document object for getElementById
    // We use class 0 (no class) and set opaque directly
    JS_SetOpaque(doc, &panels_);

    JS_SetPropertyStr(jsCtx_, doc, "getElementById",
                      JS_NewCFunction(jsCtx_, sys_getElementById, "getElementById", 1));

    JS_SetPropertyStr(jsCtx_, global, "document", doc);

    // Also set window = globalThis
    JS_SetPropertyStr(jsCtx_, global, "window", JS_DupValue(jsCtx_, global));

    JS_FreeValue(jsCtx_, global);
}

void SystemOverlay::loadPanels(const std::string& systemDir) {
    std::error_code ec;
    if (!fs::is_directory(systemDir, ec)) {
        LOG_INFO("SystemOverlay: no system directory at '%s'", systemDir.c_str());
        return;
    }

    for (const auto& entry : fs::directory_iterator(systemDir, ec)) {
        if (!entry.is_directory()) continue;

        std::string panelDir = entry.path().string();
        std::string htmlPath = panelDir + "/index.html";

        if (!fs::exists(htmlPath, ec)) continue;

        std::string html = AppLoader::loadFile(htmlPath);
        if (html.empty()) continue;

        Panel panel;
        panel.name = entry.path().filename().string();

        // Create container for this panel
        panel.container = std::make_unique<layout::BroContainer>(
            renderer_, viewportWidth_, viewportHeight_);
        panel.container->set_base_url(panelDir.c_str());

        // Extract inline CSS from the HTML
        std::string userStyles;
        {
            std::regex styleRe(R"(<style[^>]*>([\s\S]*?)</style>)",
                               std::regex_constants::icase);
            auto begin = std::sregex_iterator(html.begin(), html.end(), styleRe);
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it) {
                userStyles += (*it)[1].str() + "\n";
            }
        }

        // Parse HTML with litehtml
        panel.litehtmlDoc = litehtml::document::createFromString(
            html, panel.container.get(), litehtml::master_css, userStyles);

        if (!panel.litehtmlDoc) {
            LOG_ERROR("SystemOverlay: failed to parse '%s'", htmlPath.c_str());
            continue;
        }

        // Build DOM tree
        panel.document = std::make_unique<dom::Document>();
        panel.document->buildFrom(panel.litehtmlDoc);

        // Initial layout
        panel.litehtmlDoc->render(static_cast<litehtml::pixel_t>(viewportWidth_));

        LOG_INFO("SystemOverlay: loaded panel '%s'", panel.name.c_str());

        // Extract and execute inline scripts
        {
            std::regex scriptRe(R"(<script[^>]*>([\s\S]*?)</script>)",
                                std::regex_constants::icase);
            auto begin = std::sregex_iterator(html.begin(), html.end(), scriptRe);
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it) {
                std::string code = (*it)[1].str();
                if (!code.empty()) {
                    std::string filename = "<system/" + panel.name + ">";
                    JSValue result = JS_Eval(jsCtx_, code.c_str(), code.size(),
                                             filename.c_str(), JS_EVAL_TYPE_GLOBAL);
                    if (JS_IsException(result)) {
                        JSValue ex = JS_GetException(jsCtx_);
                        const char* str = JS_ToCString(jsCtx_, ex);
                        if (str) {
                            LOG_ERROR("SystemOverlay JS error in '%s': %s",
                                      panel.name.c_str(), str);
                            JS_FreeCString(jsCtx_, str);
                        }
                        JS_FreeValue(jsCtx_, ex);
                    }
                    JS_FreeValue(jsCtx_, result);
                }
            }
        }

        panels_.push_back(std::move(panel));
    }

    LOG_INFO("SystemOverlay: loaded %zu panel(s)", panels_.size());
}

void SystemOverlay::toggle() {
    visible_ = !visible_;
    LOG_INFO("SystemOverlay: %s", visible_ ? "visible" : "hidden");
}

void SystemOverlay::updatePerf(double fps, double frameTime, double js, double layout,
                                double raster, double gpu, double draw,
                                int vpW, int vpH) {
    if (JS_IsUndefined(broPerfObj_)) return;

    JS_SetPropertyStr(jsCtx_, broPerfObj_, "fps", JS_NewFloat64(jsCtx_, fps));
    JS_SetPropertyStr(jsCtx_, broPerfObj_, "frameTime", JS_NewFloat64(jsCtx_, frameTime));
    JS_SetPropertyStr(jsCtx_, broPerfObj_, "js", JS_NewFloat64(jsCtx_, js));
    JS_SetPropertyStr(jsCtx_, broPerfObj_, "layout", JS_NewFloat64(jsCtx_, layout));
    JS_SetPropertyStr(jsCtx_, broPerfObj_, "raster", JS_NewFloat64(jsCtx_, raster));
    JS_SetPropertyStr(jsCtx_, broPerfObj_, "gpu", JS_NewFloat64(jsCtx_, gpu));
    JS_SetPropertyStr(jsCtx_, broPerfObj_, "draw", JS_NewFloat64(jsCtx_, draw));

    // Update viewport
    JSValue global = JS_GetGlobalObject(jsCtx_);
    JSValue bro = JS_GetPropertyStr(jsCtx_, global, "__bro");
    JSValue viewport = JS_GetPropertyStr(jsCtx_, bro, "viewport");
    JS_SetPropertyStr(jsCtx_, viewport, "width", JS_NewInt32(jsCtx_, vpW));
    JS_SetPropertyStr(jsCtx_, viewport, "height", JS_NewInt32(jsCtx_, vpH));
    JS_FreeValue(jsCtx_, viewport);
    JS_FreeValue(jsCtx_, bro);
    JS_FreeValue(jsCtx_, global);
}

void SystemOverlay::tick(double nowMs) {
    if (!visible_) return;

    timers_->tick(nowMs);
    timers_->fireAnimationFrames(nowMs);

    // Drain pending jobs
    JSContext* pctx = nullptr;
    while (JS_ExecutePendingJob(jsRt_, &pctx) > 0) {}
}

void SystemOverlay::render(int vpW, int vpH) {
    if (!visible_) return;

    for (auto& panel : panels_) {
        if (!panel.litehtmlDoc) continue;

        // Re-layout if dirty
        if (panel.document && panel.document->isDirty()) {
            if (panel.document->isStructureDirty()) {
                panel.litehtmlDoc->rebuild_render_tree();
                panel.document->clearStructureDirty();
            }
            panel.litehtmlDoc->render(static_cast<litehtml::pixel_t>(vpW));
            panel.document->clearDirty();
        }

        // Draw to the same renderer surface
        litehtml::position clip(0, 0,
                                static_cast<litehtml::pixel_t>(vpW),
                                static_cast<litehtml::pixel_t>(vpH));
        panel.litehtmlDoc->draw(
            reinterpret_cast<litehtml::uint_ptr>(renderer_), 0, 0, &clip);
    }
}

void SystemOverlay::onResize(int w, int h) {
    viewportWidth_ = w;
    viewportHeight_ = h;
    for (auto& panel : panels_) {
        if (panel.container) {
            panel.container->setViewport(w, h);
        }
        if (panel.litehtmlDoc) {
            panel.litehtmlDoc->render(static_cast<litehtml::pixel_t>(w));
        }
    }
}

} // namespace bro::engine
