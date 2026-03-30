#include "engine/system_overlay.h"
#include "engine/app_loader.h"
#include "render/renderer.h"
#include "render/gl_context.h"
#include "layout/container.h"
#include "dom/document.h"
#include "js/runtime.h"
#include "js/console.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "util/log.h"

#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkRRect.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkFontMgr.h>
#include <include/codec/SkCodec.h>
#include <include/ports/SkTypeface_win.h>

#include <filesystem>
#include <regex>
#include <sstream>

extern "C" {
#include "quickjs.h"
}

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// SystemRenderer — CPU raster Skia renderer for overlay compositing
// ---------------------------------------------------------------------------

namespace bro::engine {

SystemRenderer::SystemRenderer(render::GLContext* gl) : gl_(gl) {}

SystemRenderer::~SystemRenderer() {
    fonts_.clear();
    surface_.reset();
    if (texture_ && gl_) gl_->deleteTexture(texture_);
}

SkColor SystemRenderer::toSkColor(render::Color c) const {
    return SkColorSetARGB(c.a, c.r, c.g, c.b);
}

void SystemRenderer::clear(render::Color color) {
    if (canvas_) canvas_->clear(toSkColor(color));
}

void SystemRenderer::drawRect(float x, float y, float w, float h, render::Color color) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStyle(SkPaint::kStroke_Style);
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
}

void SystemRenderer::drawRoundRect(float x, float y, float w, float h, float rx, float ry, render::Color color) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStyle(SkPaint::kStroke_Style);
    canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, w, h), rx, ry), paint);
}

void SystemRenderer::fillRect(float x, float y, float w, float h, render::Color color) {
    if (!canvas_) return;

    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
}

void SystemRenderer::drawText(std::string_view text, float x, float y, uint64_t font_handle, render::Color color) {
    if (!canvas_ || text.empty()) return;
    auto it = fonts_.find(font_handle);
    if (it == fonts_.end()) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    canvas_->drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8,
                            x, y, *it->second.font, paint);
}

render::TextMetrics SystemRenderer::measureText(std::string_view text, uint64_t font_handle) {
    auto it = fonts_.find(font_handle);
    if (it == fonts_.end()) return {};
    const SkFont& font = *it->second.font;
    SkRect bounds;
    float width = font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8, &bounds);
    return { width, bounds.height() };
}

uint64_t SystemRenderer::createFont(std::string_view family, float size, int weight, bool italic) {
    SkFontStyle style(weight,
                      SkFontStyle::kNormal_Width,
                      italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
    sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_DirectWrite();

    auto resolveGeneric = [](const std::string& name) -> const char* {
        if (name == "sans-serif")  return "Arial";
        if (name == "serif")       return "Times New Roman";
        if (name == "monospace")   return "Consolas";
        if (name == "cursive")     return "Comic Sans MS";
        if (name == "fantasy")     return "Impact";
        if (name == "system-ui")   return "Segoe UI";
        return nullptr;
    };

    sk_sp<SkTypeface> typeface;
    std::string families(family);
    std::istringstream stream(families);
    std::string name;
    while (std::getline(stream, name, ',')) {
        while (!name.empty() && (name.front() == ' ' || name.front() == '\'' || name.front() == '"')) name.erase(name.begin());
        while (!name.empty() && (name.back() == ' ' || name.back() == '\'' || name.back() == '"')) name.pop_back();
        if (name.empty()) continue;
        const char* resolved = resolveGeneric(name);
        if (resolved) {
            typeface = font_mgr->matchFamilyStyle(resolved, style);
            if (typeface) break;
        }
        typeface = font_mgr->matchFamilyStyle(name.c_str(), style);
        if (typeface) break;
    }
    if (!typeface) {
        typeface = font_mgr->matchFamilyStyle(nullptr, SkFontStyle());
    }

    auto sk_font = std::make_unique<SkFont>(typeface, size);
    sk_font->setEdging(SkFont::Edging::kAntiAlias);

    uint64_t handle = nextFontHandle_++;
    fonts_[handle] = FontEntry{std::move(typeface), std::move(sk_font)};
    return handle;
}

void SystemRenderer::deleteFont(uint64_t font_handle) {
    fonts_.erase(font_handle);
}

void SystemRenderer::drawLine(float x1, float y1, float x2, float y2, render::Color color, float thickness) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStrokeWidth(thickness);
    paint.setStyle(SkPaint::kStroke_Style);
    canvas_->drawLine(x1, y1, x2, y2, paint);
}

void SystemRenderer::drawImage(const void* data, size_t len, float x, float y, float w, float h) {
    if (!canvas_) return;
    sk_sp<SkData> sk_data = SkData::MakeWithoutCopy(data, len);
    auto codec = SkCodec::MakeFromData(sk_data);
    if (!codec) return;
    auto [image, result] = codec->getImage();
    if (!image) return;
    canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h), SkSamplingOptions());
}

void SystemRenderer::setClip(float x, float y, float w, float h) {
    if (canvas_) canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
}

void SystemRenderer::resetClip() {
    if (!canvas_) return;
    canvas_->restore();
    canvas_->save();
}

void SystemRenderer::beginFrame(int width, int height) {
    if (!surface_ || surface_->width() != width || surface_->height() != height) {
        surface_.reset();
        if (gl_) {
            if (texture_) gl_->deleteTexture(texture_);
            texture_ = gl_->createTexture2D(width, height, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE);
        }
        texWidth_ = width;
        texHeight_ = height;
    }
    // Always create a fresh surface to guarantee clean canvas state
    surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    canvas_ = surface_->getCanvas();
    canvas_->clear(SK_ColorTRANSPARENT);
    canvas_->save();
}

void SystemRenderer::endFrame() {
    if (canvas_) canvas_->restore();
    canvas_ = nullptr;
}

void SystemRenderer::uploadToGPU() {
    if (!gl_ || !surface_ || !texture_) return;
    SkPixmap pixmap;
    if (!surface_->peekPixels(&pixmap)) return;

    gl_->uploadTexture2D(texture_, pixmap.addr(),
                         static_cast<uint32_t>(pixmap.width()),
                         static_cast<uint32_t>(pixmap.height()),
                         GL_BGRA, GL_UNSIGNED_BYTE);
}

} // namespace bro::engine

namespace bro::engine {

// ---------------------------------------------------------------------------
// SystemOverlay implementation — uses shared JS runtime with per-panel contexts
// ---------------------------------------------------------------------------

SystemOverlay::SystemOverlay(js::Runtime* jsRuntime, render::GLContext* gl, int vpW, int vpH)
    : jsRuntime_(jsRuntime)
    , gl_(gl)
    , viewportWidth_(vpW)
    , viewportHeight_(vpH) {

    // Create CPU-raster renderer (no Ganesh — avoids dual-GPU-context conflicts)
    renderer_ = std::make_unique<SystemRenderer>(gl_);
}

SystemOverlay::~SystemOverlay() {
    // Tear down panels in reverse order, cleaning up per-panel JS state
    for (auto& panel : panels_) {
        if (panel.timers && panel.jsCtx) {
            panel.timers->clearAll(panel.jsCtx);
        }
        panel.timers.reset();

        if (!JS_IsUndefined(panel.broPerfObj) && panel.jsCtx) {
            JS_FreeValue(panel.jsCtx, panel.broPerfObj);
            panel.broPerfObj = JS_UNDEFINED;
        }

        // Clean up DomBindings state for this context
        if (panel.jsCtx) {
            js::DomBindings::cleanup(panel.jsCtx);
        }

        // Must release litehtml doc before container (it calls deleteFont)
        panel.litehtmlDoc.reset();
        panel.document.reset();
        panel.container.reset();

        if (panel.jsCtx) {
            JS_FreeContext(panel.jsCtx);
            panel.jsCtx = nullptr;
        }
    }
    panels_.clear();
}

void SystemOverlay::installBroObject(Panel& panel) {
    JSContext* ctx = panel.jsCtx;
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
    JS_SetPropertyStr(ctx, global, "__bro", bro);

    // Keep a reference to perf for fast updates
    panel.broPerfObj = JS_DupValue(ctx, perf);

    JS_FreeValue(ctx, global);
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
            renderer_.get(), viewportWidth_, viewportHeight_);
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

        // Create a dedicated JSContext for this panel on the shared runtime
        panel.jsCtx = jsRuntime_->createContext();

        // Install standard bindings on the panel's context
        js::Console::install(panel.jsCtx);
        panel.timers = std::make_unique<js::Timers>();
        js::Timers::install(panel.jsCtx, panel.timers.get());

        // Set window = globalThis
        JSValue global = JS_GetGlobalObject(panel.jsCtx);
        JS_SetPropertyStr(panel.jsCtx, global, "window", JS_DupValue(panel.jsCtx, global));
        JS_FreeValue(panel.jsCtx, global);

        // Install full DOM bindings (same code path as the app)
        js::DomBindings::install(panel.jsCtx, panel.document.get());

        // Install __bro perf object
        installBroObject(panel);

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
                    JSValue result = JS_Eval(panel.jsCtx, code.c_str(), code.size(),
                                             filename.c_str(), JS_EVAL_TYPE_GLOBAL);
                    if (JS_IsException(result)) {
                        JSValue ex = JS_GetException(panel.jsCtx);
                        const char* str = JS_ToCString(panel.jsCtx, ex);
                        if (str) {
                            LOG_ERROR("SystemOverlay JS error in '%s': %s",
                                      panel.name.c_str(), str);
                            JS_FreeCString(panel.jsCtx, str);
                        }
                        JS_FreeValue(panel.jsCtx, ex);
                    }
                    JS_FreeValue(panel.jsCtx, result);
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
    for (auto& panel : panels_) {
        if (JS_IsUndefined(panel.broPerfObj) || !panel.jsCtx) continue;
        JSContext* ctx = panel.jsCtx;

        JS_SetPropertyStr(ctx, panel.broPerfObj, "fps", JS_NewFloat64(ctx, fps));
        JS_SetPropertyStr(ctx, panel.broPerfObj, "frameTime", JS_NewFloat64(ctx, frameTime));
        JS_SetPropertyStr(ctx, panel.broPerfObj, "js", JS_NewFloat64(ctx, js));
        JS_SetPropertyStr(ctx, panel.broPerfObj, "layout", JS_NewFloat64(ctx, layout));
        JS_SetPropertyStr(ctx, panel.broPerfObj, "raster", JS_NewFloat64(ctx, raster));
        JS_SetPropertyStr(ctx, panel.broPerfObj, "gpu", JS_NewFloat64(ctx, gpu));
        JS_SetPropertyStr(ctx, panel.broPerfObj, "draw", JS_NewFloat64(ctx, draw));

        // Update viewport
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

void SystemOverlay::tick(double nowMs) {
    if (!visible_) return;

    for (auto& panel : panels_) {
        if (!panel.timers) continue;
        panel.timers->tick(nowMs);
        panel.timers->fireAnimationFrames(nowMs);
    }

    // Drain pending jobs on the shared runtime
    jsRuntime_->executePendingJobs();
}

void SystemOverlay::render(int vpW, int vpH) {
    if (!visible_ || !renderer_) return;

    // Re-layout dirty panels
    for (auto& panel : panels_) {
        if (!panel.litehtmlDoc || !panel.document) continue;
        if (panel.document->isDirty()) {
            if (panel.document->isStructureDirty()) {
                panel.litehtmlDoc->rebuild_render_tree();
                panel.document->clearStructureDirty();
            }
            panel.litehtmlDoc->render(static_cast<litehtml::pixel_t>(vpW));
            panel.document->clearDirty();
        }
    }

    // Rasterize all panels to own Skia surface
    renderer_->beginFrame(vpW, vpH);

    for (auto& panel : panels_) {
        if (!panel.litehtmlDoc) continue;
        litehtml::position clip(0, 0,
                                static_cast<litehtml::pixel_t>(vpW),
                                static_cast<litehtml::pixel_t>(vpH));
        panel.litehtmlDoc->draw(
            reinterpret_cast<litehtml::uint_ptr>(renderer_.get()), 0, 0, &clip);
    }

    renderer_->endFrame();
    renderer_->uploadToGPU();
}

GLuint SystemOverlay::getTexture() const {
    if (!renderer_) return 0;
    return renderer_->getTexture();
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
