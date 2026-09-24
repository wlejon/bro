// `__bro_native.window` — behind bro.window (docs/window-api.js): the calling
// realm's platform window (a secondary window's own, else the main one): its state, its two flags, the three state transitions, the
// position and size limits as scalar pairs, and the display list.

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"
#include "natives/window/native_window_decl.h"
#include "platform/sdl_window.h"
#include "util/interrupt.h"

#include <vector>

namespace bro::bronze_host {

namespace {

bool isHeadless() {
    auto* eng = hostEngine();
    return !eng || eng->displayMode() == engine::DisplayMode::Headless;
}

// The secondary window (bro.window.open) whose realm is calling, or null for
// the main window's realm. Each realm's bro.window drives its own window.
engine::WindowHost* childHost() {
    auto* eng = hostEngine();
    dom::Document* doc = currentHostDocument();
    return eng && doc ? eng->windowHostForDocument(doc) : nullptr;
}

platform::Window* getWindow() {
    auto* eng = hostEngine();
    if (!eng) return nullptr;
    if (auto* h = childHost()) return h->window.get();
    return eng->window();
}

struct WindowPosition { int x = 0, y = 0; };
struct WindowSize { int w = 0, h = 0; };

thread_local WindowPosition g_pos;
thread_local WindowSize g_minSize;
thread_local WindowSize g_maxSize;
thread_local WindowSize g_size;
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

// Windowed, the size is the live client area (SDL points), so a read right
// after setSize answers the new size before the resize event arrives.
// Headless has no real window size; the virtual viewport is the size.
void bro_window_getSize(void) {
    int w = 0, h = 0;
    auto* eng = hostEngine();
    if (auto* child = childHost()) {
        // A secondary window is a real (hidden, headless) OS window in both
        // modes; its client size is tracked on the host.
        g_size = {child->width, child->height};
        return;
    }
    if (!isHeadless()) {
        if (auto* win = getWindow()) win->getSize(w, h);
    } else if (eng) {
        w = eng->viewportWidth();
        h = eng->viewportHeight();
    }
    g_size = {w, h};
}
int32_t bro_window_getSize_width(void) { return g_size.w; }
int32_t bro_window_getSize_height(void) { return g_size.h; }

// Windowed: resize the OS window; SDL's resize event then runs the engine's
// relayout + 'resize' dispatch. Headless: resize the virtual viewport
// directly (the same path as the headless resize() helper), so the relayout
// and the 'resize' event happen here instead.
void bro_window_setSize(int32_t width, int32_t height) {
    if (width < 1 || height < 1) return;
    auto* eng = hostEngine();
    if (!eng) return;
    if (auto* child = childHost()) {
        // Same as the parent's handle.setSize: the host size updates now, the
        // document's viewport and 'resize' follow at the next record.
        child->opts.width = child->width = width;
        child->opts.height = child->height = height;
        if (child->window) child->window->setSize(width, height);
        return;
    }
    if (isHeadless()) {
        eng->handleResize(width, height);
        return;
    }
    if (auto* w = getWindow()) {
        w->setWindowSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }
}

// The main window's close path: the run loop stops at the top of its next
// frame. Headless is script-driven (the script's end is the exit), so this is
// a no-op there, which keeps an app's "Quit" button from ending a test.
void bro_window_quit(void) {
    if (isHeadless()) return;
    if (!bro::util::interrupted()) bro::util::requestInterrupt();
}

}  // extern "C"

namespace bro::bronze_host {

bool registerNatives_window(std::string* error);

bool registerWindowNatives(std::string* error) {
    return registerNatives_window(error);
}

}  // namespace bro::bronze_host
