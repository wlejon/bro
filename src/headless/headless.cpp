#include "headless/headless.h"

#include "engine/app_loader.h"
#include "render/renderer.h"
#include "render/skia_backend.h"
#include "js/runtime.h"
#include "js/console.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/node.h"
#include "dom/event.h"
#include "layout/container.h"
#include "js/dom_bindings.h"
#include "util/log.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>

// Minimal null renderer for headless mode -- does nothing, but satisfies
// the Renderer interface so BroContainer can measure text.
namespace {

class NullRenderer final : public bro::render::Renderer {
public:
    void clear(bro::render::Color) override {}
    void drawRect(float, float, float, float, bro::render::Color) override {}
    void drawRoundRect(float, float, float, float, float, float, bro::render::Color) override {}
    void fillRect(float, float, float, float, bro::render::Color) override {}
    void drawText(std::string_view, float, float, uint64_t, bro::render::Color) override {}

    bro::render::TextMetrics measureText(std::string_view text, uint64_t font_handle) override {
        float sz = 16.0f;
        auto it = fonts_.find(font_handle);
        if (it != fonts_.end()) sz = it->second;
        return { static_cast<float>(text.size()) * sz * 0.6f, sz };
    }

    uint64_t createFont(std::string_view, float size, int, bool) override {
        uint64_t h = nextHandle_++;
        fonts_[h] = size;
        return h;
    }
    void deleteFont(uint64_t h) override { fonts_.erase(h); }

    void drawLine(float, float, float, float, bro::render::Color, float) override {}
    void drawImage(const void*, size_t, float, float, float, float) override {}
    void setClip(float, float, float, float) override {}
    void resetClip() override {}
    void beginFrame(int, int) override {}
    void endFrame() override {}

private:
    std::unordered_map<uint64_t, float> fonts_;
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
{
    // 1. Null renderer (text measurement only)
    renderer_ = std::make_unique<NullRenderer>();

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

    // 7. Install JS DOM bindings
    js::DomBindings::install(jsRuntime_->getContext(), document_.get());

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
        JSValue global = JS_GetGlobalObject(ctx);
        JS_DeleteProperty(ctx, global, JS_NewAtom(ctx, "__bro_elem_map"), 0);
        JS_DeleteProperty(ctx, global, JS_NewAtom(ctx, "document"), 0);
        JS_DeleteProperty(ctx, global, JS_NewAtom(ctx, "console"), 0);
        JS_FreeValue(ctx, global);
        js::DomBindings::cleanup(ctx);
        jsRuntime_->executePendingJobs();
        JS_RunGC(jsRuntime_->getRuntime());
    }
    // 3. Release litehtml doc before document (it holds element refs)
    litehtmlDoc_.reset();
    document_.reset();
    container_.reset();
    timers_.reset();
    // Note: jsRuntime_ destruction may trigger QuickJS GC assertion if
    // class prototypes are still alive. This is a known embedding issue.
    // For headless debugging, a clean exit isn't critical.
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
    virtualTime_ += ms;
    timers_->tick(virtualTime_);
    flush();
}

void Headless::flush() {
    jsRuntime_->executePendingJobs();
    if (document_ && document_->isDirty() && litehtmlDoc_) {
        litehtmlDoc_->render(viewportWidth_);
        document_->clearDirty();
    }
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
// Event dispatch (mirrors Engine::dispatchEvent)
// ---------------------------------------------------------------------------

void Headless::dispatchClickOn(dom::Element* target) {
    if (!target || !jsRuntime_) return;

    dom::MouseEvent event("click");
    event.setTarget(target);
    JSContext* ctx = jsRuntime_->getContext();

    for (dom::Element* current = target; current != nullptr;
         current = current->parentElement()) {

        if (event.propagationStopped()) break;
        event.setCurrentTarget(current);

        auto& listeners = current->listeners();
        auto it = listeners.find("click");
        if (it == listeners.end() || it->second.empty()) continue;

        JSValue global = jsRuntime_->getGlobalObject();
        JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
        if (JS_IsUndefined(elemMap)) {
            JS_FreeValue(ctx, global);
            continue;
        }

        std::string elemKey = std::to_string(current->nodeId());
        JSValue jsElem = JS_GetPropertyStr(ctx, elemMap, elemKey.c_str());
        JS_FreeValue(ctx, elemMap);

        if (JS_IsUndefined(jsElem) || JS_IsNull(jsElem)) {
            JS_FreeValue(ctx, jsElem);
            JS_FreeValue(ctx, global);
            continue;
        }

        JSValue listenersArr = JS_GetPropertyStr(ctx, jsElem, "__bro_listeners");
        if (JS_IsUndefined(listenersArr) || !JS_IsArray(listenersArr)) {
            JS_FreeValue(ctx, listenersArr);
            JS_FreeValue(ctx, jsElem);
            JS_FreeValue(ctx, global);
            continue;
        }

        int64_t len = 0;
        JSValue lenVal = JS_GetPropertyStr(ctx, listenersArr, "length");
        JS_ToInt64(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);

        JSValue jsEvent = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, jsEvent, "type", JS_NewString(ctx, "click"));
        JS_SetPropertyStr(ctx, jsEvent, "timeStamp", JS_NewFloat64(ctx, virtualTime_));

        for (int64_t i = 0; i < len; i++) {
            JSValue entry = JS_GetPropertyInt64(ctx, listenersArr, i);
            if (JS_IsObject(entry)) {
                JSValue typeVal = JS_GetPropertyStr(ctx, entry, "type");
                const char* entryType = JS_ToCString(ctx, typeVal);
                bool match = entryType && std::strcmp(entryType, "click") == 0;
                JS_FreeCString(ctx, entryType);
                JS_FreeValue(ctx, typeVal);

                if (match) {
                    JSValue cb = JS_GetPropertyStr(ctx, entry, "cb");
                    if (JS_IsFunction(ctx, cb)) {
                        JSValue result = JS_Call(ctx, cb, jsElem, 1, &jsEvent);
                        if (JS_IsException(result)) {
                            js::Runtime::checkException(ctx, result);
                        }
                        JS_FreeValue(ctx, result);
                    }
                    JS_FreeValue(ctx, cb);
                }
            }
            JS_FreeValue(ctx, entry);
            if (event.propagationStopped()) break;
        }

        JS_FreeValue(ctx, jsEvent);
        JS_FreeValue(ctx, listenersArr);
        JS_FreeValue(ctx, jsElem);
        JS_FreeValue(ctx, global);

        if (!event.bubbles()) break;
    }
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

    if (cmd == "help") {
        std::cout << "Commands:\n"
                  << "  dump              Dump full DOM as HTML\n"
                  << "  dump <selector>   Dump a single element's outer HTML\n"
                  << "  diff              Show changes since last dump\n"
                  << "  click <selector>  Simulate a click (e.g. click #btn)\n"
                  << "  eval <js>         Evaluate JavaScript, print result\n"
                  << "  wait <ms>         Advance virtual time by N ms\n"
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
