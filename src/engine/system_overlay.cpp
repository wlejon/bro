#include "engine/system_overlay.h"
#include "engine/default_styles.h"
#include "engine/app_loader.h"
#include "render/renderer.h"
#include "render/gl_context.h"
#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include "dom/document.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/dom_bindings.h"

#include "api/api.h"
#include "util/log.h"

#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkRRect.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkBlurTypes.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkFontMetrics.h>
#include <include/codec/SkCodec.h>
#include <include/effects/SkGradient.h>
#include <include/utils/SkParsePath.h>
#ifdef _WIN32
#include <include/ports/SkTypeface_win.h>
#else
#include <include/ports/SkFontMgr_fontconfig.h>
#include <include/ports/SkFontScanner_FreeType.h>
#endif

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

static SkGradient::Colors buildGradColors(std::span<const bro::render::ColorStop> stops) {
    thread_local std::vector<SkColor4f> colors;
    thread_local std::vector<float> pos;
    colors.resize(stops.size());
    pos.resize(stops.size());
    for (size_t i = 0; i < stops.size(); i++) {
        colors[i] = SkColor4f{stops[i].color.r / 255.0f, stops[i].color.g / 255.0f,
                              stops[i].color.b / 255.0f, stops[i].color.a / 255.0f};
        pos[i] = stops[i].offset;
    }
    return SkGradient::Colors(SkSpan(colors), SkSpan(pos), SkTileMode::kClamp);
}

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
    SkFontMetrics fm;
    font.getMetrics(&fm);
    return { width, bounds.height(), -fm.fAscent, fm.fDescent };
}

uint64_t SystemRenderer::createFont(std::string_view family, float size, int weight, bool italic) {
    SkFontStyle style(weight,
                      SkFontStyle::kNormal_Width,
                      italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
#ifdef _WIN32
    sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_DirectWrite();
#else
    sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#endif

    auto resolveGeneric = [](const std::string& name) -> const char* {
#ifdef _WIN32
        if (name == "sans-serif")  return "Arial";
        if (name == "serif")       return "Times New Roman";
        if (name == "monospace")   return "Consolas";
        if (name == "cursive")     return "Comic Sans MS";
        if (name == "fantasy")     return "Impact";
        if (name == "system-ui")   return "Segoe UI";
#else
        if (name == "sans-serif")  return "Liberation Sans";
        if (name == "serif")       return "Liberation Serif";
        if (name == "monospace")   return "Liberation Mono";
        if (name == "cursive")     return "DejaVu Sans";
        if (name == "fantasy")     return "DejaVu Sans";
        if (name == "system-ui")   return "Liberation Sans";
#endif
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

void SystemRenderer::fillRoundRect(float x, float y, float w, float h, float rx, float ry, render::Color color) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, w, h), rx, ry), paint);
}

void SystemRenderer::drawCircle(float cx, float cy, float r,
                                 render::Color fill, render::Color stroke, float strokeWidth) {
    if (!canvas_) return;
    if (fill.a > 0) {
        SkPaint p; p.setColor(toSkColor(fill)); p.setStyle(SkPaint::kFill_Style); p.setAntiAlias(true);
        canvas_->drawCircle(cx, cy, r, p);
    }
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint p; p.setColor(toSkColor(stroke)); p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(strokeWidth); p.setAntiAlias(true);
        canvas_->drawCircle(cx, cy, r, p);
    }
}

void SystemRenderer::drawEllipse(float cx, float cy, float rx, float ry,
                                  render::Color fill, render::Color stroke, float strokeWidth) {
    if (!canvas_) return;
    SkRect oval = SkRect::MakeXYWH(cx - rx, cy - ry, rx * 2, ry * 2);
    if (fill.a > 0) {
        SkPaint p; p.setColor(toSkColor(fill)); p.setStyle(SkPaint::kFill_Style); p.setAntiAlias(true);
        canvas_->drawOval(oval, p);
    }
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint p; p.setColor(toSkColor(stroke)); p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(strokeWidth); p.setAntiAlias(true);
        canvas_->drawOval(oval, p);
    }
}

void SystemRenderer::drawPath(std::string_view svgPathData,
                               render::Color fill, render::Color stroke, float strokeWidth) {
    if (!canvas_ || svgPathData.empty()) return;
    auto pathOpt = SkParsePath::FromSVGString(std::string(svgPathData).c_str());
    if (!pathOpt) return;
    const SkPath& path = *pathOpt;
    if (fill.a > 0) {
        SkPaint p; p.setColor(toSkColor(fill)); p.setStyle(SkPaint::kFill_Style); p.setAntiAlias(true);
        canvas_->drawPath(path, p);
    }
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint p; p.setColor(toSkColor(stroke)); p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(strokeWidth); p.setAntiAlias(true);
        canvas_->drawPath(path, p);
    }
}

void SystemRenderer::drawPolygon(std::span<const render::PointF> points,
                                  render::Color fill, render::Color stroke, float strokeWidth) {
    if (!canvas_ || points.size() < 2) return;
    SkPathBuilder builder;
    builder.moveTo(points[0].x, points[0].y);
    for (size_t i = 1; i < points.size(); i++) builder.lineTo(points[i].x, points[i].y);
    builder.close();
    SkPath path = builder.detach();
    if (fill.a > 0) {
        SkPaint p; p.setColor(toSkColor(fill)); p.setStyle(SkPaint::kFill_Style); p.setAntiAlias(true);
        canvas_->drawPath(path, p);
    }
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint p; p.setColor(toSkColor(stroke)); p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(strokeWidth); p.setAntiAlias(true);
        canvas_->drawPath(path, p);
    }
}

void SystemRenderer::drawPolyline(std::span<const render::PointF> points,
                                   render::Color stroke, float strokeWidth) {
    if (!canvas_ || points.size() < 2) return;
    SkPathBuilder builder;
    builder.moveTo(points[0].x, points[0].y);
    for (size_t i = 1; i < points.size(); i++) builder.lineTo(points[i].x, points[i].y);
    SkPath path = builder.detach();
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint p; p.setColor(toSkColor(stroke)); p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(strokeWidth); p.setAntiAlias(true);
        canvas_->drawPath(path, p);
    }
}

void SystemRenderer::drawBoxShadow(float x, float y, float w, float h,
                                   float rx, float ry,
                                   float offsetX, float offsetY,
                                   float blur, float spread,
                                   render::Color color, bool inset) {
    if (!canvas_) return;
    (void)inset;
    float sx2 = x + offsetX - spread;
    float sy2 = y + offsetY - spread;
    float sw = w + spread * 2;
    float sh = h + spread * 2;
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(toSkColor(color));
    if (blur > 0)
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2.0f));
    if (rx > 0 || ry > 0)
        canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(sx2, sy2, sw, sh), rx, ry), paint);
    else
        canvas_->drawRect(SkRect::MakeXYWH(sx2, sy2, sw, sh), paint);
}

void SystemRenderer::save() { if (canvas_) canvas_->save(); }
void SystemRenderer::restore() { if (canvas_) canvas_->restore(); }
void SystemRenderer::saveLayerAlpha(uint8_t alpha) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setAlphaf(alpha / 255.0f);
    canvas_->saveLayer(nullptr, &paint);
}
void SystemRenderer::translate(float dx, float dy) { if (canvas_) canvas_->translate(dx, dy); }
void SystemRenderer::scale(float sx, float sy) { if (canvas_) canvas_->scale(sx, sy); }

void SystemRenderer::setClip(float x, float y, float w, float h) {
    if (canvas_) canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
}

void SystemRenderer::resetClip() {
    if (!canvas_) return;
    canvas_->restore();
    canvas_->save();
}

void SystemRenderer::fillLinearGradient(float x, float y, float w, float h,
                                        float startX, float startY, float endX, float endY,
                                        std::span<const render::ColorStop> stops) {
    if (!canvas_ || stops.empty()) return;
    SkPoint pts[2] = { {startX, startY}, {endX, endY} };
    auto shader = SkShaders::LinearGradient(pts, SkGradient(buildGradColors(stops), {}));
    SkPaint paint;
    paint.setShader(shader);
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->save();
    canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
    canvas_->restore();
}

void SystemRenderer::fillRadialGradient(float x, float y, float w, float h,
                                        float cx, float cy, float rx, float ry,
                                        std::span<const render::ColorStop> stops) {
    if (!canvas_ || stops.empty()) return;
    float r = std::max(rx, ry);
    if (r < 0.001f) r = 0.001f;
    SkPaint paint;
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->save();
    canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
    if (std::abs(rx - ry) > 0.001f && rx > 0 && ry > 0) {
        canvas_->translate(cx, cy);
        canvas_->scale(1.0f, ry / rx);
        canvas_->translate(-cx, -cy);
        paint.setShader(SkShaders::RadialGradient({cx, cy}, rx,
            SkGradient(buildGradColors(stops), {})));
    } else {
        paint.setShader(SkShaders::RadialGradient({cx, cy}, r,
            SkGradient(buildGradColors(stops), {})));
    }
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
    canvas_->restore();
}

void SystemRenderer::fillConicGradient(float x, float y, float w, float h,
                                       float cx, float cy, float angleDeg,
                                       std::span<const render::ColorStop> stops) {
    if (!canvas_ || stops.empty()) return;
    float startAngle = angleDeg - 90.0f;
    auto shader = SkShaders::SweepGradient({cx, cy}, startAngle, startAngle + 360.0f,
        SkGradient(buildGradColors(stops), {}));
    SkPaint paint;
    paint.setShader(shader);
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->save();
    canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
    canvas_->restore();
}

void SystemRenderer::beginFrame(int width, int height) {
    if (!surface_ || surface_->width() != width || surface_->height() != height) {
        surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
        if (gl_) {
            if (texture_) gl_->deleteTexture(texture_);
            texture_ = gl_->createTexture2D(width, height, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE);
        }
        texWidth_ = width;
        texHeight_ = height;
    }
    canvas_ = surface_->getCanvas();
    canvas_->restoreToCount(0);
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

        panel.document.reset();
        panel.drawTraversal.reset();

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
        std::string savedBasePath = panelDir;

        // Extract inline CSS from the HTML, prepended with UA defaults
        std::string userStyles = kDefaultStyles;
        userStyles += "\n";
        {
            std::regex styleRe(R"(<style[^>]*>([\s\S]*?)</style>)",
                               std::regex_constants::icase);
            auto begin = std::sregex_iterator(html.begin(), html.end(), styleRe);
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it) {
                userStyles += (*it)[1].str() + "\n";
            }
        }

        // Parse HTML with htmlayout
        panel.document = std::make_unique<dom::Document>();
        panel.document->parse(html, userStyles);

        // Create a dedicated JSContext for this panel on the shared runtime
        panel.jsCtx = jsRuntime_->createContext();

        // Install standard bindings on the panel's context
        brokit::api::installConsole(panel.jsCtx);
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

        // Initial layout (uses local fontManager — data moves with panel)
        {
            layout::SkiaTextMetrics textMetrics(renderer_.get(), &panel.fontManager);
            panel.document->resolveStyles();
            panel.document->performLayout(static_cast<float>(viewportWidth_), textMetrics);
        }

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

        // Move panel into vector FIRST, then create DrawTraversal with
        // the stable fontManager address. DrawTraversal stores a raw pointer
        // to fontManager, so it must point to the final location.
        panels_.push_back(std::move(panel));
        auto& finalPanel = panels_.back();
        finalPanel.drawTraversal = std::make_unique<layout::DrawTraversal>(
            renderer_.get(), &finalPanel.fontManager);
        finalPanel.drawTraversal->setBasePath(savedBasePath);
        finalPanel.drawTraversal->setViewport(viewportWidth_, viewportHeight_);
    }

    LOG_INFO("SystemOverlay: loaded %zu panel(s)", panels_.size());
}

void SystemOverlay::toggle() {
    visible_ = !visible_;
    renderDirty_ = true;
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

    // Note: pending jobs are drained by the engine's main loop
    // (jsRuntime_->executePendingJobs()) — not here, to avoid
    // draining app jobs before the WebGL FBO is bound.

    // Check if any panel became dirty from timer callbacks
    for (auto& panel : panels_) {
        if (panel.document && panel.document->isDirty()) {
            renderDirty_ = true;
            break;
        }
    }
}

void SystemOverlay::render(int vpW, int vpH) {
    if (!visible_ || !renderer_ || !renderDirty_) return;

    // Re-layout dirty panels
    for (auto& panel : panels_) {
        if (!panel.document) continue;
        if (panel.document->isDirty()) {
            panel.document->clearStructureDirty();
            layout::SkiaTextMetrics textMetrics(renderer_.get(), &panel.fontManager);
            panel.document->resolveStyles();
            panel.document->performLayout(static_cast<float>(vpW), textMetrics);
            panel.document->clearDirty();
        }
    }

    // Rasterize all panels to own Skia surface
    renderer_->beginFrame(vpW, vpH);

    for (auto& panel : panels_) {
        if (!panel.document || !panel.drawTraversal) continue;
        panel.drawTraversal->draw(panel.document->documentElement(), 0, 0, vpW, vpH);
    }

    renderer_->endFrame();
    renderer_->uploadToGPU();
    renderDirty_ = false;
}

GLuint SystemOverlay::getTexture() const {
    if (!renderer_) return 0;
    return renderer_->getTexture();
}

void SystemOverlay::onResize(int w, int h) {
    viewportWidth_ = w;
    viewportHeight_ = h;
    renderDirty_ = true;
    for (auto& panel : panels_) {
        if (panel.drawTraversal) {
            panel.drawTraversal->setViewport(w, h);
        }
        if (panel.document) {
            layout::SkiaTextMetrics textMetrics(renderer_.get(), &panel.fontManager);
            panel.document->resolveStyles();
            panel.document->performLayout(static_cast<float>(w), textMetrics);
        }
    }
}

} // namespace bro::engine
