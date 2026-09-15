// `__bro_native.window` — behind bro.window (docs/window-api.js): the
// platform window's state, its two flags, the three state transitions, the
// position and size limits as scalar pairs, and the display list.

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"
#include "natives/window/native_window_decl.h"
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

struct WindowPosition { int x = 0, y = 0; };
struct WindowSize { int w = 0, h = 0; };

thread_local WindowPosition g_pos;
thread_local WindowSize g_minSize;
thread_local WindowSize g_maxSize;
thread_local std::vector<platform::DisplayInfo> g_displays;

const platform::DisplayInfo* displayAt(int32_t i) {
    if (i < 0 || static_cast<size_t>(i) >= g_displays.size()) return nullptr;
    return &g_displays[static_cast<size_t>(i)];
}

}  // namespace

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

const char* bro_window_state_get(void) {
    auto* w = getWindow();
    const char* state = "normal";
    if (w) {
        if (w->isMinimized()) state = "minimized";
        else if (w->isFullscreen()) state = "fullscreen";
        else if (w->isMaximized()) state = "maximized";
    }
    return state;
}

bool bro_window_borderless_get(void) { auto* w = getWindow(); return w && w->isBorderless(); }
void bro_window_borderless_set(bool v) { if (auto* w = getWindow()) w->setBorderless(v); }

bool bro_window_alwaysOnTop_get(void) { auto* w = getWindow(); return w && w->isAlwaysOnTop(); }
void bro_window_alwaysOnTop_set(bool v) { if (auto* w = getWindow()) w->setAlwaysOnTop(v); }

void bro_window_minimize(void) { if (!isHeadless()) if (auto* w = getWindow()) w->minimize(); }
void bro_window_maximize(void) { if (!isHeadless()) if (auto* w = getWindow()) w->maximize(); }
void bro_window_restore(void) { if (!isHeadless()) if (auto* w = getWindow()) w->restore(); }

void bro_window_getPosition(void) {
    int x = 0, y = 0;
    if (auto* w = getWindow()) w->getPosition(x, y);
    g_pos = {x, y};
}
int32_t bro_window_getPosition_x(void) { return g_pos.x; }
int32_t bro_window_getPosition_y(void) { return g_pos.y; }

void bro_window_setPosition(int32_t x, int32_t y) {
    if (isHeadless()) return;
    if (auto* w = getWindow()) w->setPosition(x, y);
}

void bro_window_getMinSize(void) {
    int w = 0, h = 0;
    if (auto* win = getWindow()) win->getMinimumSize(w, h);
    g_minSize = {w, h};
}
int32_t bro_window_getMinSize_width(void) { return g_minSize.w; }
int32_t bro_window_getMinSize_height(void) { return g_minSize.h; }

void bro_window_setMinSize(int32_t width, int32_t height) {
    if (auto* win = getWindow()) win->setMinimumSize(width, height);
}

void bro_window_getMaxSize(void) {
    int w = 0, h = 0;
    if (auto* win = getWindow()) win->getMaximumSize(w, h);
    g_maxSize = {w, h};
}
int32_t bro_window_getMaxSize_width(void) { return g_maxSize.w; }
int32_t bro_window_getMaxSize_height(void) { return g_maxSize.h; }

void bro_window_setMaxSize(int32_t width, int32_t height) {
    if (auto* win = getWindow()) win->setMaximumSize(width, height);
}

int32_t bro_window_getDisplays(void) {
    g_displays.clear();
    if (auto* w = getWindow()) g_displays = w->getDisplays();
    return static_cast<int32_t>(g_displays.size());
}

double bro_window_getDisplays_id(int32_t index) {
    auto* d = displayAt(index);
    return d ? static_cast<double>(d->id) : 0.0;
}

const char* bro_window_getDisplays_name(int32_t index) {
    auto* d = displayAt(index);
    return natives::strResult(d ? d->name : std::string());
}

int32_t bro_window_getDisplays_x(int32_t index) {
    auto* d = displayAt(index);
    return d ? d->x : 0;
}

int32_t bro_window_getDisplays_y(int32_t index) {
    auto* d = displayAt(index);
    return d ? d->y : 0;
}

int32_t bro_window_getDisplays_width(int32_t index) {
    auto* d = displayAt(index);
    return d ? d->width : 0;
}

int32_t bro_window_getDisplays_height(int32_t index) {
    auto* d = displayAt(index);
    return d ? d->height : 0;
}

int32_t bro_window_getDisplays_workX(int32_t index) {
    auto* d = displayAt(index);
    return d ? d->workX : 0;
}

int32_t bro_window_getDisplays_workY(int32_t index) {
    auto* d = displayAt(index);
    return d ? d->workY : 0;
}

int32_t bro_window_getDisplays_workWidth(int32_t index) {
    auto* d = displayAt(index);
    return d ? d->workWidth : 0;
}

int32_t bro_window_getDisplays_workHeight(int32_t index) {
    auto* d = displayAt(index);
    return d ? d->workHeight : 0;
}

double bro_window_getDisplays_refreshRate(int32_t index) {
    auto* d = displayAt(index);
    return d ? d->refreshRate : 0.0;
}

double bro_window_getDisplays_contentScale(int32_t index) {
    auto* d = displayAt(index);
    return d ? d->contentScale : 1.0;
}

bool bro_window_getDisplays_isPrimary(int32_t index) {
    auto* d = displayAt(index);
    return d && d->isPrimary;
}

bool bro_window_getDisplays_isCurrent(int32_t index) {
    auto* d = displayAt(index);
    return d && d->isCurrent;
}

bool bro_window_moveToDisplay(double id) {
    auto* w = getWindow();
    if (!w || isHeadless()) return false;
    return w->moveToDisplay(static_cast<uint32_t>(id));
}

}  // extern "C"

namespace bro::bronze_host {

bool registerNatives_window(std::string* error);

bool registerWindowNatives(std::string* error) {
    return registerNatives_window(error);
}

}  // namespace bro::bronze_host
