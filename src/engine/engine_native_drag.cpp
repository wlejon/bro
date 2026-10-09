// A page's drag leaving the window. When dragstart goes through, what its
// handlers put in the drag data store is offered to the window system
// (Window::startDrag) with a small label for its picture, and from then on
// the window system carries the drag: pointer events stop, and the drag is
// followed through EventLoop's onOwnDrag* reports instead — over this window
// it goes on as the page's drag (dragenter/dragover/drop in the page, with
// the page's own data), dropped on another application it ends with dragend
// and the effect that application took. Where the window system cannot
// carry it (SDL on Linux), the drag stays inside the page as before.
#include "engine/engine.h"
#include "dom/drag_data_store.h"
#include "platform/window.h"
#include "render/system_font_mgr.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMetrics.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRRect.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTypeface.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace bro::engine {

namespace {

// Cuts `s` to at most `max` bytes without splitting a UTF-8 sequence.
std::string utf8Prefix(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n) + "\xE2\x80\xA6";  // …
}

std::string firstLine(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == '\n' || s[a] == '\r' || s[a] == ' ' || s[a] == '\t')) ++a;
    size_t b = s.find_first_of("\r\n", a);
    return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

// What the drag picture says: a dragged file's name, else the text.
std::string dragLabel(const dom::DragDataStore& store) {
    if (auto it = store.data.find("text/uri-list"); it != store.data.end()) {
        std::string uri;
        size_t pos = 0;
        while (pos < it->second.size()) {
            size_t end = it->second.find_first_of("\r\n", pos);
            std::string line = it->second.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? it->second.size() : end + 1;
            if (!line.empty() && line[0] != '#') {
                uri = line;
                break;
            }
        }
        if (!uri.empty()) {
            while (!uri.empty() && uri.back() == '/') uri.pop_back();
            size_t slash = uri.find_last_of('/');
            std::string name = slash == std::string::npos ? uri : uri.substr(slash + 1);
            return utf8Prefix(name.empty() ? uri : name, 48);
        }
    }
    if (auto it = store.data.find("text/plain"); it != store.data.end())
        return utf8Prefix(firstLine(it->second), 48);
    return store.data.empty() ? std::string() : utf8Prefix(store.data.begin()->first, 48);
}

// A rounded label with `text`, premultiplied BGRA.
void renderDragIcon(const std::string& text, platform::DragSource& out) {
    sk_sp<SkTypeface> face;
    if (SkFontMgr* mgr = render::systemFontMgr()) {
        face = mgr->matchFamilyStyle("sans-serif", SkFontStyle());
        if (!face) face = mgr->legacyMakeTypeface(nullptr, SkFontStyle());
    }
    SkFont font(face, 13.0f);
    font.setEdging(SkFont::Edging::kAntiAlias);
    const std::string label = text.empty() ? std::string(" ") : text;
    const float textW = font.measureText(label.data(), label.size(), SkTextEncoding::kUTF8);
    SkFontMetrics m;
    font.getMetrics(&m);
    const int padX = 10, h = 26;
    const int w = std::clamp(static_cast<int>(std::ceil(textW)) + 2 * padX, 24, 360);
    const SkImageInfo info = SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    if (!surface) return;
    SkCanvas* c = surface->getCanvas();
    c->clear(SK_ColorTRANSPARENT);
    SkPaint bg;
    bg.setAntiAlias(true);
    bg.setColor(SkColorSetARGB(230, 32, 40, 52));
    c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeWH(static_cast<float>(w), static_cast<float>(h)), 6, 6), bg);
    SkPaint border;
    border.setAntiAlias(true);
    border.setStyle(SkPaint::kStroke_Style);
    border.setColor(SkColorSetARGB(160, 120, 160, 220));
    c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(0.5f, 0.5f, w - 1.0f, h - 1.0f), 6, 6), border);
    SkPaint fg;
    fg.setAntiAlias(true);
    fg.setColor(SK_ColorWHITE);
    const float baseline = (h - (m.fDescent - m.fAscent)) / 2.0f - m.fAscent;
    c->save();
    c->clipRect(SkRect::MakeXYWH(static_cast<float>(padX - 2), 0, static_cast<float>(w - 2 * padX + 4),
                                 static_cast<float>(h)));
    c->drawSimpleText(label.data(), label.size(), SkTextEncoding::kUTF8, static_cast<float>(padX), baseline,
                      font, fg);
    c->restore();
    out.iconBgra.resize(static_cast<size_t>(w) * h * 4);
    if (!surface->readPixels(info, out.iconBgra.data(), static_cast<size_t>(w) * 4, 0, 0)) {
        out.iconBgra.clear();
        return;
    }
    out.iconWidth = w;
    out.iconHeight = h;
    // Below and to the right of the pointer, clear of the cursor.
    out.hotX = -14;
    out.hotY = -14;
}

}  // namespace

void Engine::beginNativeDrag() {
    nativeDrag_ = false;
    // A shell host (DRM, or its headless stand-in) is the window system:
    // its compositor carries the drag to the client windows.
    const bool toClients = !shellCompositorSocket().empty();
    if (!toClients && (displayMode_ != DisplayMode::Windowed || !window_)) return;
    const dom::DragDataStore& store = dom::dragDataStore();
    if (store.data.empty()) return;  // nothing another application could take

    std::string allowed = store.effectAllowed;
    for (char& ch : allowed) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    platform::DragSource drag;
    if (allowed == "none") return;
    drag.allowCopy = allowed != "move" && allowed != "linkmove";
    drag.allowMove = allowed == "move" || allowed == "linkmove" || allowed == "copymove" || allowed == "all" ||
                     allowed == "uninitialized";

    // Files (a URI list) first, then text under every name it goes by, then
    // whatever else the page set, as it set it.
    if (auto it = store.data.find("text/uri-list"); it != store.data.end())
        drag.data.emplace_back("text/uri-list", it->second);
    if (auto it = store.data.find("text/plain"); it != store.data.end()) {
        for (const char* name : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING"})
            drag.data.emplace_back(name, it->second);
    }
    std::vector<std::string> rest;
    for (const auto& [fmt, value] : store.data)
        if (fmt != "text/uri-list" && fmt != "text/plain") rest.push_back(fmt);
    std::sort(rest.begin(), rest.end());
    for (const auto& fmt : rest) drag.data.emplace_back(fmt, store.data.at(fmt));

    renderDragIcon(dragLabel(store), drag);
    nativeDrag_ = toClients ? startShellDrag(drag) : window_->startDrag(drag);
}

void Engine::handleOwnDragMotion(float x, float y) {
    if (!nativeDrag_ || !document_) return;
    lastMouseX_ = x;
    lastMouseY_ = y;
    const float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;
    dom::Element* target = hitTest(docX, docY);
    dragDrop_.update(target, x, y, pressedButtons_ | 1);
}

void Engine::handleOwnDragLeave() {
    if (!nativeDrag_) return;
    dragDrop_.leaveWindow(lastMouseX_, lastMouseY_);
}

void Engine::handleOwnDragDrop(float x, float y) {
    if (!nativeDrag_) return;
    handleOwnDragMotion(x, y);
    // The button came up over the page: the page's own drop, then dragend,
    // as for a drag that never left (handleMouseUp ends the page's drag).
    handleMouseUp(x, y, 1);
}

void Engine::handleOwnDragEnd(const std::string& action) {
    if (!nativeDrag_) return;
    nativeDrag_ = false;
    if (!dragDrop_.dragging()) return;  // dropped on this window: already over
    // Over somewhere else: dragend says what that application did with it.
    dragDrop_.leaveWindow(lastMouseX_, lastMouseY_);
    dom::dragDataStore().dropEffect = action;
    handleMouseUp(lastMouseX_, lastMouseY_, 1);
}

}  // namespace bro::engine
