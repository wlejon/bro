// `__bro_native.window` — behind bro.window (docs/window-api.js): the
// platform window's state, its two flags, the three state transitions, the
// position and size limits as scalar pairs, and the display list.
//
// THE DISPLAY LIST crosses as scalar pieces over a SNAPSHOT: displaySnapshot()
// reads Window::getDisplays once and answers the count, and the per-index
// natives read the snapshot rather than asking SDL fourteen times per
// display. bro_core.js's getDisplays() takes the snapshot and assembles the
// documented DisplayInfo objects from it.
//
// Headless has a hidden SDL window, so the reads answer what it reports; the
// operations that would move or restyle a window a user cannot see are no-ops
// there, as they were before.

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"
#include "platform/sdl_window.h"

#include <vector>

namespace bro::bronze_host {

namespace {

bool isHeadless() {
    auto* eng = hostEngine();
    return !eng || eng->displayMode() == engine::DisplayMode::Headless;
}

platform::Window* getWindow() {
    auto* eng = hostEngine();
    return eng ? eng->window() : nullptr;
}

const char* stateGet() {
    auto* w = getWindow();
    const char* state = "normal";
    if (w) {
        if (w->isMinimized()) state = "minimized";
        else if (w->isFullscreen()) state = "fullscreen";
        else if (w->isMaximized()) state = "maximized";
    }
    return state;
}

bool borderlessGet() { auto* w = getWindow(); return w && w->isBorderless(); }
void borderlessSet(bool v) { if (auto* w = getWindow()) w->setBorderless(v); }
bool alwaysOnTopGet() { auto* w = getWindow(); return w && w->isAlwaysOnTop(); }
void alwaysOnTopSet(bool v) { if (auto* w = getWindow()) w->setAlwaysOnTop(v); }

void minimize() { if (!isHeadless()) if (auto* w = getWindow()) w->minimize(); }
void maximize() { if (!isHeadless()) if (auto* w = getWindow()) w->maximize(); }
void restore() { if (!isHeadless()) if (auto* w = getWindow()) w->restore(); }

double positionX() { int x = 0, y = 0; if (auto* w = getWindow()) w->getPosition(x, y); return x; }
double positionY() { int x = 0, y = 0; if (auto* w = getWindow()) w->getPosition(x, y); return y; }
void setPosition(double x, double y) {
    if (isHeadless()) return;
    if (auto* w = getWindow()) w->setPosition(static_cast<int>(x), static_cast<int>(y));
}

double minWidth() { int w = 0, h = 0; if (auto* win = getWindow()) win->getMinimumSize(w, h); return w; }
double minHeight() { int w = 0, h = 0; if (auto* win = getWindow()) win->getMinimumSize(w, h); return h; }
void setMinSize(double w, double h) {
    if (auto* win = getWindow()) win->setMinimumSize(static_cast<int>(w), static_cast<int>(h));
}
double maxWidth() { int w = 0, h = 0; if (auto* win = getWindow()) win->getMaximumSize(w, h); return w; }
double maxHeight() { int w = 0, h = 0; if (auto* win = getWindow()) win->getMaximumSize(w, h); return h; }
void setMaxSize(double w, double h) {
    if (auto* win = getWindow()) win->setMaximumSize(static_cast<int>(w), static_cast<int>(h));
}

// ---- displays: a snapshot, then scalar reads by index ----------------------

thread_local std::vector<platform::DisplayInfo> g_displays;

int32_t displaySnapshot() {
    g_displays.clear();
    if (auto* w = getWindow()) g_displays = w->getDisplays();
    return static_cast<int32_t>(g_displays.size());
}

const platform::DisplayInfo* displayAt(int32_t i) {
    if (i < 0 || static_cast<size_t>(i) >= g_displays.size()) return nullptr;
    return &g_displays[static_cast<size_t>(i)];
}

double displayId(int32_t i) { auto* d = displayAt(i); return d ? d->id : 0; }
const char* displayName(int32_t i) {
    auto* d = displayAt(i);
    return natives::strResult(d ? d->name : std::string());
}
double displayX(int32_t i) { auto* d = displayAt(i); return d ? d->x : 0; }
double displayY(int32_t i) { auto* d = displayAt(i); return d ? d->y : 0; }
double displayWidth(int32_t i) { auto* d = displayAt(i); return d ? d->width : 0; }
double displayHeight(int32_t i) { auto* d = displayAt(i); return d ? d->height : 0; }
double displayWorkX(int32_t i) { auto* d = displayAt(i); return d ? d->workX : 0; }
double displayWorkY(int32_t i) { auto* d = displayAt(i); return d ? d->workY : 0; }
double displayWorkWidth(int32_t i) { auto* d = displayAt(i); return d ? d->workWidth : 0; }
double displayWorkHeight(int32_t i) { auto* d = displayAt(i); return d ? d->workHeight : 0; }
double displayRefreshRate(int32_t i) { auto* d = displayAt(i); return d ? d->refreshRate : 0; }
double displayContentScale(int32_t i) { auto* d = displayAt(i); return d ? d->contentScale : 1; }
bool displayIsPrimary(int32_t i) { auto* d = displayAt(i); return d && d->isPrimary; }
bool displayIsCurrent(int32_t i) { auto* d = displayAt(i); return d && d->isCurrent; }

bool moveToDisplay(double id) {
    auto* w = getWindow();
    if (!w || isHeadless()) return false;
    return w->moveToDisplay(static_cast<uint32_t>(id));
}

}  // namespace

bool registerWindowNatives(std::string* error) {
    using namespace natives;
    auto p = [](auto f) { return reinterpret_cast<void*>(f); };
    return getter("__bro_native.window.state", p(&stateGet), "str", error) &&
           getter("__bro_native.window.borderless", p(&borderlessGet), "bool", error) &&
           setter("__bro_native.window.borderless", p(&borderlessSet), "bool", error) &&
           getter("__bro_native.window.alwaysOnTop", p(&alwaysOnTopGet), "bool", error) &&
           setter("__bro_native.window.alwaysOnTop", p(&alwaysOnTopSet), "bool", error) &&
           fn("__bro_native.window.minimize", p(&minimize), "void", {}, error) &&
           fn("__bro_native.window.maximize", p(&maximize), "void", {}, error) &&
           fn("__bro_native.window.restore", p(&restore), "void", {}, error) &&
           fn("__bro_native.window.positionX", p(&positionX), "f64", {}, error) &&
           fn("__bro_native.window.positionY", p(&positionY), "f64", {}, error) &&
           fn("__bro_native.window.setPosition", p(&setPosition), "void", {"f64", "f64"}, error) &&
           fn("__bro_native.window.minWidth", p(&minWidth), "f64", {}, error) &&
           fn("__bro_native.window.minHeight", p(&minHeight), "f64", {}, error) &&
           fn("__bro_native.window.setMinSize", p(&setMinSize), "void", {"f64", "f64"}, error) &&
           fn("__bro_native.window.maxWidth", p(&maxWidth), "f64", {}, error) &&
           fn("__bro_native.window.maxHeight", p(&maxHeight), "f64", {}, error) &&
           fn("__bro_native.window.setMaxSize", p(&setMaxSize), "void", {"f64", "f64"}, error) &&
           fn("__bro_native.window.displaySnapshot", p(&displaySnapshot), "i32", {}, error) &&
           fn("__bro_native.window.displayId", p(&displayId), "f64", {"i32"}, error) &&
           fn("__bro_native.window.displayName", p(&displayName), "str", {"i32"}, error) &&
           fn("__bro_native.window.displayX", p(&displayX), "f64", {"i32"}, error) &&
           fn("__bro_native.window.displayY", p(&displayY), "f64", {"i32"}, error) &&
           fn("__bro_native.window.displayWidth", p(&displayWidth), "f64", {"i32"}, error) &&
           fn("__bro_native.window.displayHeight", p(&displayHeight), "f64", {"i32"}, error) &&
           fn("__bro_native.window.displayWorkX", p(&displayWorkX), "f64", {"i32"}, error) &&
           fn("__bro_native.window.displayWorkY", p(&displayWorkY), "f64", {"i32"}, error) &&
           fn("__bro_native.window.displayWorkWidth", p(&displayWorkWidth), "f64", {"i32"}, error) &&
           fn("__bro_native.window.displayWorkHeight", p(&displayWorkHeight), "f64", {"i32"}, error) &&
           fn("__bro_native.window.displayRefreshRate", p(&displayRefreshRate), "f64", {"i32"}, error) &&
           fn("__bro_native.window.displayContentScale", p(&displayContentScale), "f64", {"i32"}, error) &&
           fn("__bro_native.window.displayIsPrimary", p(&displayIsPrimary), "bool", {"i32"}, error) &&
           fn("__bro_native.window.displayIsCurrent", p(&displayIsCurrent), "bool", {"i32"}, error) &&
           fn("__bro_native.window.moveToDisplay", p(&moveToDisplay), "bool", {"f64"}, error);
}

}  // namespace bro::bronze_host
