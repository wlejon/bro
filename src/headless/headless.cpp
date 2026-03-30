#include "headless/headless.h"

#include "engine/app_loader.h"
#include "engine/system_overlay.h"
#include "render/renderer.h"
#include "render/skia_backend.h"
#include "js/runtime.h"
#include "js/console.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "js/canvas_bindings.h"
#include "js/audio_bindings.h"
#include "js/storage_bindings.h"
#include "canvas/canvas_scene.h"
#include "canvas/canvas2d.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/node.h"
#include "dom/event.h"
#include "layout/container.h"
#include "js/dom_bindings.h"
#include "js/event_dispatch.h"
#include "util/log.h"
#include "util/time.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>

#include <include/core/SkCanvas.h>
#include <include/core/SkSurface.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkRRect.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/codec/SkCodec.h>
#include <include/ports/SkTypeface_win.h>

// Skia raster renderer for headless mode — renders to a CPU surface with
// accurate text measurement and PNG screenshot support.
namespace {

class RasterRenderer final : public bro::render::Renderer {
public:
    RasterRenderer() = default;
    ~RasterRenderer() override = default;

    void clear(bro::render::Color c) override {
        if (canvas_) canvas_->clear(toSkColor(c));
    }

    void drawRect(float x, float y, float w, float h, bro::render::Color c) override {
        if (!canvas_) return;
        SkPaint paint;
        paint.setColor(toSkColor(c));
        paint.setStyle(SkPaint::kStroke_Style);
        canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
    }

    void drawRoundRect(float x, float y, float w, float h, float rx, float ry,
                       bro::render::Color c) override {
        if (!canvas_) return;
        SkPaint paint;
        paint.setColor(toSkColor(c));
        paint.setStyle(SkPaint::kStroke_Style);
        canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, w, h), rx, ry), paint);
    }

    void fillRect(float x, float y, float w, float h, bro::render::Color c) override {
        if (!canvas_) return;
        SkPaint paint;
        paint.setColor(toSkColor(c));
        paint.setStyle(SkPaint::kFill_Style);
        canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
    }

    void drawText(std::string_view text, float x, float y, uint64_t font_handle,
                  bro::render::Color c) override {
        if (!canvas_) return;
        auto it = fonts_.find(font_handle);
        if (it == fonts_.end()) return;
        SkPaint paint;
        paint.setColor(toSkColor(c));
        canvas_->drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8,
                                x, y, *it->second.font, paint);
    }

    bro::render::TextMetrics measureText(std::string_view text, uint64_t font_handle) override {
        auto it = fonts_.find(font_handle);
        if (it == fonts_.end()) return {};
        const SkFont& font = *it->second.font;
        SkRect bounds;
        float width = font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8, &bounds);
        return { width, bounds.height() };
    }

    uint64_t createFont(std::string_view family, float size, int weight, bool italic) override {
        SkFontStyle style(weight, SkFontStyle::kNormal_Width,
                          italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
        sk_sp<SkFontMgr> mgr = SkFontMgr_New_DirectWrite();
        // CSS font-family is comma-separated — try each name in order.
        sk_sp<SkTypeface> typeface;
        std::string families(family);
        std::istringstream stream(families);
        std::string name;
        while (std::getline(stream, name, ',')) {
            while (!name.empty() && (name.front() == ' ' || name.front() == '\'' || name.front() == '"')) name.erase(name.begin());
            while (!name.empty() && (name.back() == ' ' || name.back() == '\'' || name.back() == '"')) name.pop_back();
            if (name.empty()) continue;
            typeface = mgr->matchFamilyStyle(name.c_str(), style);
            if (typeface) break;
        }
        if (!typeface) typeface = mgr->matchFamilyStyle(nullptr, SkFontStyle());
        auto sk_font = std::make_unique<SkFont>(typeface, size);
        sk_font->setEdging(SkFont::Edging::kSubpixelAntiAlias);
        sk_font->setSubpixel(true);
        uint64_t handle = nextHandle_++;
        fonts_[handle] = { std::move(typeface), std::move(sk_font) };
        return handle;
    }

    void deleteFont(uint64_t h) override { fonts_.erase(h); }

    void drawLine(float x1, float y1, float x2, float y2, bro::render::Color c,
                  float thickness) override {
        if (!canvas_) return;
        SkPaint paint;
        paint.setColor(toSkColor(c));
        paint.setStrokeWidth(thickness);
        paint.setStyle(SkPaint::kStroke_Style);
        canvas_->drawLine(x1, y1, x2, y2, paint);
    }

    void drawImage(const void* data, size_t len, float x, float y, float w, float h) override {
        if (!canvas_) return;
        auto sk_data = SkData::MakeWithoutCopy(data, len);
        auto codec = SkCodec::MakeFromData(sk_data);
        if (!codec) return;
        auto [image, result] = codec->getImage();
        if (!image) return;
        canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h), SkSamplingOptions());
    }

    void setClip(float x, float y, float w, float h) override {
        if (canvas_) canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
    }

    void resetClip() override {
        if (!canvas_) return;
        canvas_->restore();
        canvas_->save();
    }

    void beginFrame(int width, int height) override {
        if (!surface_ || surface_->width() != width || surface_->height() != height) {
            surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
        }
        canvas_ = surface_->getCanvas();
        canvas_->save();
    }

    void endFrame() override {
        if (canvas_) canvas_->restore();
        canvas_ = nullptr;
    }

    SkCanvas* getCanvas() const { return canvas_; }

    bool saveScreenshot(const std::string& path) {
        if (!surface_) return false;
        sk_sp<SkImage> image = surface_->makeImageSnapshot();
        if (!image) return false;
        SkPixmap pixmap;
        if (!image->peekPixels(&pixmap)) return false;

        int w = pixmap.width(), h = pixmap.height();
        int rowBytes = ((w * 3 + 3) / 4) * 4; // BMP rows are 4-byte aligned
        int dataSize = rowBytes * h;

        // BMP file header (14 bytes) + BITMAPINFOHEADER (40 bytes)
        uint8_t header[54] = {};
        auto put16 = [&](int off, uint16_t v) { memcpy(header + off, &v, 2); };
        auto put32 = [&](int off, uint32_t v) { memcpy(header + off, &v, 4); };
        header[0] = 'B'; header[1] = 'M';
        put32(2, 54 + dataSize);   // file size
        put32(10, 54);             // pixel data offset
        put32(14, 40);             // info header size
        put32(18, w);
        put32(22, h);
        put16(26, 1);              // planes
        put16(28, 24);             // bits per pixel
        put32(34, dataSize);

        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        out.write(reinterpret_cast<char*>(header), 54);

        // BMP is bottom-to-top, BGR
        std::vector<uint8_t> row(rowBytes, 0);
        for (int y = h - 1; y >= 0; --y) {
            const uint8_t* src = reinterpret_cast<const uint8_t*>(pixmap.addr32(0, y));
            for (int x = 0; x < w; ++x) {
                // N32 on Windows = BGRA premultiplied
                row[x * 3 + 0] = src[x * 4 + 0]; // B
                row[x * 3 + 1] = src[x * 4 + 1]; // G
                row[x * 3 + 2] = src[x * 4 + 2]; // R
            }
            out.write(reinterpret_cast<char*>(row.data()), rowBytes);
        }
        return out.good();
    }

private:
    static SkColor toSkColor(bro::render::Color c) {
        return SkColorSetARGB(c.a, c.r, c.g, c.b);
    }

    struct FontEntry {
        sk_sp<SkTypeface> typeface;
        std::unique_ptr<SkFont> font;
    };

    sk_sp<SkSurface> surface_;
    SkCanvas* canvas_ = nullptr;
    std::unordered_map<uint64_t, FontEntry> fonts_;
    uint64_t nextHandle_ = 1;
};

} // anonymous namespace

namespace bro::headless {

using namespace bro::engine;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Headless::Headless(const std::string& appDir, int width, int height)
    : viewportWidth_(width), viewportHeight_(height)
    , virtualTime_(bro::util::currentTimeMs())  // start from wall-clock so timers created during init work
{
    renderer_ = std::make_unique<RasterRenderer>();

    // 2. JS runtime
    jsRuntime_ = std::make_unique<js::Runtime>();
    js::Console::install(jsRuntime_->getContext());
    timers_ = std::make_unique<js::Timers>();
    js::Timers::install(jsRuntime_->getContext(), timers_.get());

    // 3. Layout container
    container_ = std::make_unique<layout::BroContainer>(
        renderer_.get(), viewportWidth_, viewportHeight_);

    // 4. Load app
    auto manifest = AppLoader::loadApp(appDir);
    std::string html = AppLoader::loadFile(manifest.htmlPath);
    if (html.empty()) {
        throw std::runtime_error("Failed to load index.html from " + appDir);
    }

    container_->set_base_url(manifest.basePath.c_str());

    std::string userStyles;
    for (auto& cssPath : manifest.stylePaths) {
        std::string css = AppLoader::loadFile(cssPath);
        if (!css.empty()) userStyles += css + "\n";
    }

    // 5. Parse HTML (single document shared by layout + DOM)
    litehtmlDoc_ = litehtml::document::createFromString(
        html, container_.get(), litehtml::master_css, userStyles);

    // 6. Build DOM tree
    document_ = std::make_unique<dom::Document>();
    document_->buildFrom(litehtmlDoc_);

    // 7. Set up window/navigator globals BEFORE DOM bindings
    //    (DOM bindings install polyfills that reference window)
    {
        JSContext* ctx = jsRuntime_->getContext();
        JSValue global = JS_GetGlobalObject(ctx);

        JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
        JS_SetPropertyStr(ctx, global, "devicePixelRatio", JS_NewFloat64(ctx, 1.0));
        JS_SetPropertyStr(ctx, global, "innerWidth", JS_NewInt32(ctx, viewportWidth_));
        JS_SetPropertyStr(ctx, global, "innerHeight", JS_NewInt32(ctx, viewportHeight_));

        JSValue nav = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, nav, "userAgent", JS_NewString(ctx, "Bro/1.0"));
        JS_SetPropertyStr(ctx, nav, "platform", JS_NewString(ctx, "Win32"));
        JS_SetPropertyStr(ctx, nav, "language", JS_NewString(ctx, "en-US"));
        JS_SetPropertyStr(ctx, global, "navigator", nav);

        const char* windowEventPolyfill = R"JS(
(function() {
    var listeners = {};
    globalThis.__bro_win_listeners = listeners;
    globalThis.addEventListener = function(type, fn) {
        if (!listeners[type]) listeners[type] = [];
        listeners[type].push(fn);
    };
    globalThis.removeEventListener = function(type, fn) {
        var arr = listeners[type];
        if (!arr) return;
        var idx = arr.indexOf(fn);
        if (idx >= 0) arr.splice(idx, 1);
    };
    globalThis.__bro_dispatch_window_event = function(type, event) {
        var arr = listeners[type];
        if (!arr) return;
        for (var i = 0; i < arr.length; i++) {
            try { arr[i](event); } catch(e) { console.error('Event handler error:', e); }
        }
    };
})();
)JS";
        JSValue r = JS_Eval(ctx, windowEventPolyfill, strlen(windowEventPolyfill),
                            "<window-events>", JS_EVAL_TYPE_GLOBAL);
        JS_FreeValue(ctx, r);

        JS_FreeValue(ctx, global);
    }

    // 7a. Install JS DOM bindings (after window setup so polyfills work)
    js::DomBindings::install(jsRuntime_->getContext(), document_.get());

    // 7b. Install Canvas 2D bindings with headless factory
    js::CanvasBindings::install(jsRuntime_->getContext());
    js::DomBindings::setGetContextFactory(
        [this](JSContext* ctx, dom::Element*, const std::string&) -> JSValue {
            canvasScene_ = std::make_unique<canvas::CanvasScene>(renderer_.get());
            canvasScenePtr_ = canvasScene_.get();
            canvasScene_->onInit(nullptr, viewportWidth_, viewportHeight_);
            return js::CanvasBindings::wrapContext2D(ctx, canvasScenePtr_);
        });

    // 7c. Audio bindings (no-op in headless — AudioContext constructor will throw,
    //     JS code catches and falls back gracefully)
    js::AudioBindings::install(jsRuntime_->getContext(), nullptr);

    // 7d. localStorage
    js::StorageBindings::install(jsRuntime_->getContext(), manifest.basePath + "/.storage.json");

    // 8. Execute scripts
    for (auto& scriptPath : manifest.scriptPaths) {
        std::string code = AppLoader::loadFile(scriptPath);
        if (!code.empty()) {
            if (!jsRuntime_->eval(code, scriptPath)) {
                LOG_ERROR("Failed to execute script: %s", scriptPath.c_str());
            }
        }
    }

    // 9. Initial layout
    if (litehtmlDoc_) {
        litehtmlDoc_->render(viewportWidth_);
    }

    // 10. System overlay (shares JS runtime, no GL in headless)
    systemOverlay_ = std::make_unique<engine::SystemOverlay>(
        jsRuntime_.get(), nullptr, viewportWidth_, viewportHeight_);
    systemOverlay_->loadPanels("system");

    flush();
}

Headless::~Headless() {
    // Must tear down in careful order:
    // 1. Clear timers (they hold JS callbacks)
    if (timers_ && jsRuntime_) {
        timers_->clearAll(jsRuntime_->getContext());
    }
    // 2. Clear the JS elem map and prototypes (prevent leaked references)
    if (jsRuntime_) {
        JSContext* ctx = jsRuntime_->getContext();
        js::AudioBindings::cleanup(ctx);
        js::StorageBindings::cleanup(ctx);
        JSValue global = JS_GetGlobalObject(ctx);
        JS_DeleteProperty(ctx, global, JS_NewAtom(ctx, "__bro_elem_map"), 0);
        JS_DeleteProperty(ctx, global, JS_NewAtom(ctx, "document"), 0);
        JS_DeleteProperty(ctx, global, JS_NewAtom(ctx, "console"), 0);
        JS_FreeValue(ctx, global);
        js::DomBindings::cleanup(ctx);
        jsRuntime_->executePendingJobs();
        JS_RunGC(jsRuntime_->getRuntime());
    }
    systemOverlay_.reset();
    canvasScene_.reset();
    // 3. Release litehtml doc before document (it holds element refs)
    litehtmlDoc_.reset();
    document_.reset();
    container_.reset();
    timers_.reset();
    // Clean up per-runtime DomBindings state before the runtime is freed.
    if (jsRuntime_) {
        js::DomBindings::cleanupRuntime(jsRuntime_->getRuntime());
    }
    jsRuntime_.reset();
    renderer_.reset();
}

// ---------------------------------------------------------------------------
// DOM output
// ---------------------------------------------------------------------------

std::string Headless::dumpHTML() const {
    if (!document_ || !document_->documentElement()) return "";
    return document_->documentElement()->innerHTML();
}

std::string Headless::dumpElement(const std::string& selector) const {
    auto* el = querySelector(selector);
    if (!el) return "(not found: " + selector + ")";
    // Outer HTML: <tag attrs>innerHTML</tag>
    std::string tag = el->tagName();
    std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
    std::string result = "<" + tag;
    std::string id = el->id();
    if (!id.empty()) result += " id=\"" + id + "\"";
    result += ">" + el->innerHTML() + "</" + tag + ">";
    return result;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

bool Headless::click(const std::string& selector) {
    auto* el = querySelector(selector);
    if (!el) {
        std::cout << "[headless] click: element not found: " << selector << "\n";
        return false;
    }
    dispatchClickOn(el);
    flush();
    return true;
}

bool Headless::setValue(const std::string& selector, const std::string& value) {
    auto* el = querySelector(selector);
    if (!el) return false;
    el->setAttribute("value", value);
    document_->markDirty();
    flush();
    return true;
}

std::string Headless::eval(const std::string& code) {
    JSContext* ctx = jsRuntime_->getContext();
    JSValue result = JS_Eval(ctx, code.c_str(), code.size(), "<headless>",
                              JS_EVAL_TYPE_GLOBAL);
    std::string output;
    if (JS_IsException(result)) {
        js::Runtime::checkException(ctx, result);
        output = "[exception]";
    } else {
        const char* str = JS_ToCString(ctx, result);
        if (str) {
            output = str;
            JS_FreeCString(ctx, str);
        } else {
            output = "[null]";
        }
    }
    JS_FreeValue(ctx, result);
    flush();
    return output;
}

void Headless::advanceTime(double ms) {
    // Advance in 16ms steps to mirror real-time frame cadence
    double remaining = ms;
    static constexpr double kGCIntervalMs = 1000.0;
    while (remaining > 0) {
        double step = std::min(remaining, 16.0);
        virtualTime_ += step;
        remaining -= step;
        timers_->tick(virtualTime_);
        timers_->fireAnimationFrames(virtualTime_);
        flush();

        // Periodic GC + orphan sweep (every ~1s of virtual time)
        static double lastGCTime = 0;
        if (virtualTime_ - lastGCTime >= kGCIntervalMs) {
            bro::js::DomBindings::sweepOrphanedWrappers(jsRuntime_->getContext());
            JS_RunGC(jsRuntime_->getRuntime());
            lastGCTime = virtualTime_;
        }
    }
}

void Headless::flush() {
    jsRuntime_->executePendingJobs();
    if (document_ && document_->isDirty() && litehtmlDoc_) {
        if (document_->isStructureDirty()) {
            litehtmlDoc_->rebuild_render_tree();
            document_->clearStructureDirty();
        }
        litehtmlDoc_->render(viewportWidth_);
        document_->clearDirty();
    }
}

bool Headless::screenshot(const std::string& path) {
    if (!litehtmlDoc_) return false;

    // Fire any pending rAF callbacks so canvas commands are up to date
    timers_->fireAnimationFrames(virtualTime_);
    jsRuntime_->executePendingJobs();

    // Render a frame to the raster surface
    renderer_->beginFrame(viewportWidth_, viewportHeight_);
    renderer_->clear({0, 0, 0, 255});

    // Render canvas scene first (behind HTML)
    if (canvasScenePtr_) {
        auto& cmds = canvasScenePtr_->canvas().commands();
        uint8_t fillR = 0, fillG = 0, fillB = 0, fillA = 255;
        float globalAlpha = 1.0f;

        for (auto& cmd : cmds) {
            using CT = canvas::CmdType;
            switch (cmd.type) {
            case CT::SetFillStyle:
                fillR = cmd.r; fillG = cmd.g; fillB = cmd.b; fillA = cmd.a;
                break;
            case CT::SetGlobalAlpha:
                globalAlpha = cmd.f;
                break;
            case CT::FillRect: {
                uint8_t a = static_cast<uint8_t>(fillA * globalAlpha);
                renderer_->fillRect(cmd.x, cmd.y, cmd.w, cmd.h,
                                    {fillR, fillG, fillB, a});
                break;
            }
            case CT::ClearRect:
                renderer_->fillRect(cmd.x, cmd.y, cmd.w, cmd.h, {0, 0, 0, 255});
                break;
            case CT::StrokeRect: break; // skip in headless for now
            default: break;
            }
        }
    }

    // Render HTML/CSS overlay on top
    litehtml::position clip(0, 0, viewportWidth_, viewportHeight_);
    litehtmlDoc_->draw(
        reinterpret_cast<litehtml::uint_ptr>(renderer_.get()), 0, 0, &clip);

    // Render system overlay on top of everything
    if (systemOverlay_ && systemOverlay_->isVisible()) {
        systemOverlay_->tick(virtualTime_);
        systemOverlay_->render(viewportWidth_, viewportHeight_);

        // Composite system overlay surface onto app surface
        auto* sysRenderer = systemOverlay_->getRenderer();
        if (sysRenderer && sysRenderer->surface()) {
            auto* appCanvas = static_cast<RasterRenderer*>(renderer_.get())->getCanvas();
            if (appCanvas) {
                sk_sp<SkImage> sysImage = sysRenderer->surface()->makeImageSnapshot();
                if (sysImage) {
                    SkPaint paint;
                    paint.setBlendMode(SkBlendMode::kSrcOver);
                    appCanvas->drawImage(sysImage, 0, 0, SkSamplingOptions(), &paint);
                }
            }
        }
    }

    renderer_->endFrame();

    // Save the surface as BMP
    auto* raster = static_cast<RasterRenderer*>(renderer_.get());
    return raster->saveScreenshot(path);
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

dom::Element* Headless::querySelector(const std::string& selector) const {
    if (!document_) return nullptr;

    // Handle #id shorthand
    if (!selector.empty() && selector[0] == '#') {
        return document_->getElementById(selector.substr(1));
    }

    return document_->querySelector(selector);
}

// ---------------------------------------------------------------------------
// Event dispatch (delegates to shared implementation)
// ---------------------------------------------------------------------------

void Headless::dispatchClickOn(dom::Element* target) {
    if (!target || !jsRuntime_) return;
    dom::MouseEvent event("click");
    js::dispatchDomEvent(jsRuntime_->getContext(), target, event);
}

// ---------------------------------------------------------------------------
// Command processing
// ---------------------------------------------------------------------------

bool Headless::processCommand(const std::string& line) {
    // Trim
    std::string cmd = line;
    while (!cmd.empty() && (cmd.front() == ' ' || cmd.front() == '\t')) cmd.erase(cmd.begin());
    while (!cmd.empty() && (cmd.back() == ' ' || cmd.back() == '\t' ||
                             cmd.back() == '\r' || cmd.back() == '\n')) cmd.pop_back();

    if (cmd.empty() || cmd[0] == '#') return true; // comment or blank

    if (cmd == "quit" || cmd == "exit") return false;

    if (cmd == "dump") {
        std::string html = dumpHTML();
        std::cout << html << "\n";
        lastDump_ = html;
        return true;
    }

    if (cmd.substr(0, 5) == "dump ") {
        std::string selector = cmd.substr(5);
        std::cout << dumpElement(selector) << "\n";
        return true;
    }

    if (cmd == "diff") {
        std::string current = dumpHTML();
        if (lastDump_.empty()) {
            std::cout << "(no previous dump to diff against — showing full HTML)\n";
            std::cout << current << "\n";
        } else if (lastDump_ == current) {
            std::cout << "(no changes)\n";
        } else {
            // Simple line-by-line diff
            auto splitLines = [](const std::string& s) -> std::vector<std::string> {
                std::vector<std::string> lines;
                std::istringstream iss(s);
                std::string l;
                while (std::getline(iss, l)) lines.push_back(l);
                return lines;
            };
            auto oldLines = splitLines(lastDump_);
            auto newLines = splitLines(current);
            size_t maxLines = std::max(oldLines.size(), newLines.size());
            for (size_t i = 0; i < maxLines; i++) {
                std::string oldL = (i < oldLines.size()) ? oldLines[i] : "";
                std::string newL = (i < newLines.size()) ? newLines[i] : "";
                if (oldL != newL) {
                    if (!oldL.empty()) std::cout << "- " << oldL << "\n";
                    if (!newL.empty()) std::cout << "+ " << newL << "\n";
                }
            }
        }
        lastDump_ = current;
        return true;
    }

    if (cmd.substr(0, 6) == "click ") {
        std::string selector = cmd.substr(6);
        std::string before = dumpHTML();
        click(selector);
        std::string after = dumpHTML();
        if (before != after) {
            std::cout << "[changed]\n";
        }
        return true;
    }

    if (cmd.substr(0, 5) == "eval ") {
        std::string code = cmd.substr(5);
        std::string result = eval(code);
        std::cout << result << "\n";
        return true;
    }

    if (cmd.substr(0, 5) == "wait ") {
        double ms = std::stod(cmd.substr(5));
        advanceTime(ms);
        return true;
    }

    if (cmd.substr(0, 11) == "screenshot ") {
        std::string path = cmd.substr(11);
        if (screenshot(path)) {
            std::cout << "[headless] saved screenshot to " << path << "\n";
        } else {
            std::cout << "[headless] screenshot failed\n";
        }
        return true;
    }

    if (cmd == "system" || cmd == "system toggle") {
        if (systemOverlay_) {
            systemOverlay_->toggle();
            std::cout << "[headless] system overlay "
                      << (systemOverlay_->isVisible() ? "visible" : "hidden") << "\n";
        }
        return true;
    }

    if (cmd.substr(0, 12) == "system perf ") {
        // Push perf data: system perf <fps> <frameTime> <js> <layout> <raster> <gpu> <draw>
        if (systemOverlay_) {
            std::istringstream iss(cmd.substr(12));
            double fps, ft, js, layout, raster, gpu, draw;
            if (iss >> fps >> ft >> js >> layout >> raster >> gpu >> draw) {
                systemOverlay_->updatePerf(fps, ft, js, layout, raster, gpu, draw,
                                           viewportWidth_, viewportHeight_);
                systemOverlay_->tick(virtualTime_);
            }
        }
        return true;
    }

    if (cmd == "help") {
        std::cout << "Commands:\n"
                  << "  dump              Dump full DOM as HTML\n"
                  << "  dump <selector>   Dump a single element's outer HTML\n"
                  << "  diff              Show changes since last dump\n"
                  << "  click <selector>  Simulate a click (e.g. click #btn)\n"
                  << "  eval <js>         Evaluate JavaScript, print result\n"
                  << "  wait <ms>         Advance virtual time by N ms\n"
                  << "  screenshot <path> Render to PNG file\n"
                  << "  quit              Exit\n"
                  << "  # comment         Ignored\n";
        return true;
    }

    std::cout << "[headless] unknown command: " << cmd << "\n";
    return true;
}

void Headless::runInteractive() {
    std::cout << "bro headless> " << std::flush;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!processCommand(line)) break;
        std::cout << "bro headless> " << std::flush;
    }
}

void Headless::runScript(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        std::cerr << "Cannot open script: " << path << "\n";
        return;
    }
    std::string line;
    while (std::getline(ifs, line)) {
        std::cout << "> " << line << "\n";
        if (!processCommand(line)) break;
    }
}

} // namespace bro::headless
