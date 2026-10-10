// `__bro_native.window` — behind bro.window (docs/window-api.js): the calling
// realm's platform window (a secondary window's own, else the main one): its state, its two flags, the three state transitions, the
// position and size limits as scalar pairs, and the display list.

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"
#include "engine/app_runtime.h"
#include "bronze_host/native_window.h"
#include "natives/window/native_window_decl.h"
#include "platform/desktop_platform.h"
#include "platform/desktop_bell.h"
#include "platform/desktop_progress.h"
#include "platform/desktop_notifications.h"
#include "platform/desktop_tray.h"
#include "platform/desktop_hotkeys.h"
#include "platform/desktop_single_instance.h"
#include "platform/window.h"
#include "util/interrupt.h"
#include "util/log.h"

#include <string>

#include <vector>

namespace bro::bronze_host {

namespace {

ev::Persistent* g_focusDispatcher = nullptr;
ev::Persistent* g_hotkeyDispatcher = nullptr;
ev::Persistent* g_trayDispatcher = nullptr;
ev::Persistent* g_singleInstanceDispatcher = nullptr;

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

using namespace bro;
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

const char* bro_window_title_get(void) {
    if (auto* child = childHost()) {
        return natives::strResult(child->opts.title);
    }
    if (auto* w = getWindow()) {
        return natives::strResult(w->getTitle());
    }
    return natives::strResult("");
}

void bro_window_title_set(const char* title) {
    std::string t = title ? title : "";
    if (auto* child = childHost()) {
        child->opts.title = t;
        if (child->window) child->window->setTitle(t);
        return;
    }
    if (auto* w = getWindow()) {
        w->setTitle(t);
    }
}

double bro_window_opacity_get(void) {
    if (auto* child = childHost()) {
        if (child->window) return static_cast<double>(child->window->getOpacity());
        return 1.0;
    }
    if (auto* w = getWindow()) {
        return static_cast<double>(w->getOpacity());
    }
    return 1.0;
}

void bro_window_opacity_set(double opacity) {
    float op = static_cast<float>(opacity);
    if (auto* child = childHost()) {
        if (child->window) child->window->setOpacity(op);
        return;
    }
    if (auto* w = getWindow()) {
        w->setOpacity(op);
    }
}

bool bro_window_fullscreen_get(void) {
    if (auto* child = childHost()) {
        if (child->window) return child->window->isFullscreen();
        return false;
    }
    if (auto* w = getWindow()) {
        return w->isFullscreen();
    }
    return false;
}

void bro_window_fullscreen_set(bool fullscreen) {
    if (auto* child = childHost()) {
        if (child->window) child->window->setFullscreen(fullscreen);
        return;
    }
    if (auto* w = getWindow()) {
        w->setFullscreen(fullscreen);
    }
}

bool bro_window_focused_get(void) {
    auto* eng = hostEngine();
    if (auto* child = childHost()) {
        if (child->window && !isHeadless()) {
            return child->window->isFocused();
        }
        return child->focused;
    }
    if (auto* w = getWindow()) {
        if (!isHeadless()) {
            return w->isFocused();
        }
    }
    return eng ? eng->isWindowFocused() : true;
}

bool bro_window_flash(bool on) {
    if (auto* child = childHost()) {
        if (child->window) return child->window->flash(on);
        return true;
    }
    if (auto* w = getWindow()) {
        return w->flash(on);
    }
    return true;
}

bool bro_window_beep(void) {
    return platform::desktop::beep();
}

int32_t bro_window_getBeepCount(void) {
    return static_cast<int32_t>(platform::desktop::getHeadlessBeepCount());
}

void bro_window_resetBeepCount(void) {
    platform::desktop::resetHeadlessBeepCount();
}

bool bro_window_setProgress(int32_t state, int32_t value) {
    auto* w = getWindow();
    auto s = static_cast<platform::desktop::ProgressState>(state);
    return platform::desktop::setTaskbarProgress(w, s, value);
}

int32_t bro_window_getProgressState(void) {
    return static_cast<int32_t>(platform::desktop::getHeadlessProgressState());
}

int32_t bro_window_getProgressValue(void) {
    return platform::desktop::getHeadlessProgressValue();
}

int32_t bro_window_notify(const char* title, const char* body, const char* icon, int32_t timeoutMs, bool silent, int32_t replacesId,
                          const char* actions, const char* payload) {
    auto* w = getWindow();
    platform::desktop::NotificationOptions opts;
    opts.icon = icon ? icon : "";
    opts.timeoutMs = timeoutMs;
    opts.silent = silent;
    opts.replacesId = static_cast<uint32_t>(replacesId);
    opts.payload = payload ? payload : "";
    if (actions && *actions) {
        const std::string all = actions;
        size_t start = 0;
        while (start <= all.size()) {
            size_t end = all.find('\x1e', start);
            if (end == std::string::npos) end = all.size();
            const std::string pair = all.substr(start, end - start);
            const size_t sep = pair.find('\x1f');
            if (!pair.empty()) {
                opts.actions.push_back({pair.substr(0, sep), sep == std::string::npos ? pair : pair.substr(sep + 1)});
            }
            start = end + 1;
        }
    }
    // The app is who notifies: its id (the Windows AUMID, the Linux desktop
    // entry) and its name.
    const engine::AppRuntimeInfo& app = engine::currentApp();
    opts.appId = app.id;
    opts.appName = app.manifest.name.empty() ? app.id : app.manifest.name;
    return static_cast<int32_t>(platform::desktop::showNotification(w, title ? title : "", body ? body : "", opts));
}

int32_t bro_window_getNotificationCount(void) {
    return static_cast<int32_t>(platform::desktop::getRecordedNotifications().size());
}

const char* bro_window_getLastNotificationTitle(void) {
    auto list = platform::desktop::getRecordedNotifications();
    return natives::strResult(list.empty() ? "" : list.back().title);
}

const char* bro_window_getLastNotificationBody(void) {
    auto list = platform::desktop::getRecordedNotifications();
    return natives::strResult(list.empty() ? "" : list.back().body);
}

void bro_window_clearNotifications(void) {
    platform::desktop::clearRecordedNotifications();
}

bool bro_window_setTray(const char* icon, const char* tooltip, const char* menuJson) {
    (void)menuJson;
    auto* w = getWindow();
    platform::desktop::TrayConfig config;
    config.icon = icon ? icon : "";
    config.tooltip = tooltip ? tooltip : "";
    return platform::desktop::setTray(w, config);
}

bool bro_window_removeTray(void) {
    return platform::desktop::removeTray();
}

bool bro_window_hasTray(void) {
    return platform::desktop::hasTray();
}

bool bro_window_isTrayAvailable(void) {
    return platform::desktop::isTrayAvailable();
}

void bro_window_simulateTrayClick(const char* itemId) {
    if (g_trayDispatcher) {
        ev::Persistent arg(ev::fromUtf8(itemId ? itemId : ""));
        Value argv[1] = {arg.get()};
        ev::call(g_trayDispatcher->get(), ev::undefined(), argv);
    }
}

int32_t bro_window_registerGlobalHotkey(const char* accelerator, bool grab) {
    if (!accelerator || !*accelerator) return 0;
    auto* w = getWindow();
    std::string accel = accelerator;
    platform::desktop::HotkeyOptions opts;
    opts.grab = grab;
    uint32_t id = platform::desktop::registerGlobalHotkey(w, accel, [accel](uint32_t hid) {
        if (g_hotkeyDispatcher) {
            ev::Persistent idArg(ev::fromDouble(static_cast<double>(hid)));
            ev::Persistent accelArg(ev::fromUtf8(accel));
            Value argv[2] = {idArg.get(), accelArg.get()};
            ev::call(g_hotkeyDispatcher->get(), ev::undefined(), argv);
        }
    }, opts);
    return static_cast<int32_t>(id);
}

int32_t bro_window_simulateHotkeyKey(const char* key, const char* mods, int32_t code, bool down, bool repeat) {
    auto k = platform::desktop::hotkeyKeyFromNames(key ? key : "", mods ? mods : "",
                                                   static_cast<uint32_t>(code), down, repeat);
    auto r = platform::desktop::routeHotkeyKey(k);
    return (r.consumed ? 1 : 0) | (r.fired ? 2 : 0) | (r.grabbed ? 4 : 0);
}

void bro_window_resetHotkeyKeys(void) {
    platform::desktop::resetHotkeyKeyState();
}

const char* bro_window_displayMode_get(void) {
    auto* eng = hostEngine();
    if (!eng) return "headless";
    switch (eng->displayMode()) {
        case engine::DisplayMode::Windowed: return "windowed";
        case engine::DisplayMode::Headless: return "headless";
        case engine::DisplayMode::Server: return "server";
        case engine::DisplayMode::Drm: return "drm";
    }
    return "windowed";
}

const char* bro_window_captureScreen(const char* path) {
    static thread_local std::string s_written;
    s_written.clear();
    auto* eng = hostEngine();
    if (!eng) return "";
    std::string target = (path && *path) ? path : engine::Engine::defaultScreenCapturePath();
    std::string why;
    if (!eng->captureScreen(target, &why)) {
        LOG_WARN("bro.window.captureScreen('%s') failed: %s", target.c_str(), why.c_str());
        return "";
    }
    s_written = std::move(target);
    return s_written.c_str();
}

bool bro_window_unregisterGlobalHotkey(int32_t id) {
    return platform::desktop::unregisterGlobalHotkey(static_cast<uint32_t>(id));
}

void bro_window_unregisterAllGlobalHotkeys(void) {
    platform::desktop::unregisterAllGlobalHotkeys();
}

bool bro_window_simulateGlobalHotkey(const char* accelerator) {
    if (!accelerator) return false;
    return platform::desktop::simulateGlobalHotkey(accelerator);
}

bool bro_window_requestSingleInstance(const char* name, const char* argsJson) {
    // An app whose manifest declares `"singleInstance": true` already holds
    // its channel (engine/app_runtime.h); the hand-offs it receives reach this
    // API's onInstance as well (host_app.cpp), so there is nothing to claim.
    if (engine::currentApp().singleInstance) return true;
    std::string n = (name && *name) ? name : "bro_app";
    // The arguments cross as the one JSON array the wrapper serialised, and
    // are handed to onInstance as that same array.
    std::vector<std::string> currentArgs{(argsJson && *argsJson) ? argsJson : "[]"};
    return platform::desktop::requestSingleInstance(n, currentArgs, [](const std::vector<std::string>& args) {
        if (g_singleInstanceDispatcher) {
            const std::string json = args.empty() ? std::string("[]") : args[0];
            ev::Persistent arg(ev::fromUtf8(json));
            Value argv[1] = {arg.get()};
            ev::call(g_singleInstanceDispatcher->get(), ev::undefined(), argv);
        }
    });
}

void bro_window_shutdownSingleInstance(void) {
    platform::desktop::shutdownSingleInstance();
}

bool bro_window_simulateSingleInstance(const char* name, const char* argsJson) {
    (void)name;
    if (g_singleInstanceDispatcher) {
        ev::Persistent arg(ev::fromUtf8(argsJson ? argsJson : "[]"));
        Value argv[1] = {arg.get()};
        ev::call(g_singleInstanceDispatcher->get(), ev::undefined(), argv);
        return true;
    }
    return false;
}

void bro_window_setFocusDispatcher(uint64_t fnBits) {
    if (!g_focusDispatcher) g_focusDispatcher = new ev::Persistent();
    g_focusDispatcher->set(ev::fromBits(fnBits));
}

void bro_window_setHotkeyDispatcher(uint64_t fnBits) {
    if (!g_hotkeyDispatcher) g_hotkeyDispatcher = new ev::Persistent();
    g_hotkeyDispatcher->set(ev::fromBits(fnBits));
}

void bro_window_setTrayDispatcher(uint64_t fnBits) {
    if (!g_trayDispatcher) g_trayDispatcher = new ev::Persistent();
    g_trayDispatcher->set(ev::fromBits(fnBits));
}

void bro_window_setSingleInstanceDispatcher(uint64_t fnBits) {
    if (!g_singleInstanceDispatcher) g_singleInstanceDispatcher = new ev::Persistent();
    g_singleInstanceDispatcher->set(ev::fromBits(fnBits));
}

void bro_window_simulateFocus(bool gained) {
    auto* eng = hostEngine();
    if (eng) {
        eng->setWindowFocused(gained);
        eng->dispatchWindowFocusChange(gained);
    } else {
        dispatchWindowFocus(gained);
    }
}

}  // extern "C"

namespace bro::bronze_host {

void dispatchWindowFocus(bool gained) {
    if (g_focusDispatcher) {
        ev::Persistent arg(ev::fromBool(gained));
        Value argv[1] = {arg.get()};
        ev::call(g_focusDispatcher->get(), ev::undefined(), argv);
    }
}

bool registerNatives_window(std::string* error);

bool registerWindowNatives(std::string* error) {
    return registerNatives_window(error);
}

}  // namespace bro::bronze_host
