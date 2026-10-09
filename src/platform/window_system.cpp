#include "platform/window_system.h"

#include "platform/backends.h"
#include "platform/clipboard.h"
#include "platform/displays.h"
#include "platform/event_loop.h"
#include "platform/gamepads.h"
#include "platform/keyboard.h"
#include "platform/system_info.h"
#include "platform/window.h"
#include "util/log.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

namespace bro::platform {

namespace {
WindowSystem* g_active = nullptr;
}  // namespace

#if !BRO_HAVE_WAYLAND_BACKEND
WindowSystem* waylandWindowSystem(std::string* why) {
    if (why) *why = "this build has no Wayland window system (browl, xkbcommon)";
    return nullptr;
}
#endif

bool selectWindowSystem(WindowSystemKind kind) {
    WindowSystem* next = nullptr;
    bool ok = true;
    switch (kind) {
        case WindowSystemKind::Drm: next = &drmWindowSystem(); break;
        case WindowSystemKind::Sdl: next = &sdlWindowSystem(); break;
        case WindowSystemKind::Wayland: {
            std::string why;
            next = waylandWindowSystem(&why);
            if (!next) {
                LOG_INFO("Wayland window system unavailable (%s); using SDL", why.c_str());
                next = &sdlWindowSystem();
                ok = false;
            }
            break;
        }
    }
    if (g_active != next) LOG_INFO("Window system: %s", next->name());
    g_active = next;
    return ok;
}

void selectDesktopWindowSystem() {
    const char* force = std::getenv("BRO_WINDOW_SYSTEM");
    const std::string forced = force ? force : "";
    if (forced == "sdl") {
        selectWindowSystem(WindowSystemKind::Sdl);
        return;
    }
#if defined(__linux__)
    // An SDL video driver asked for by name (offscreen, x11, dummy) is a
    // request for SDL; only "wayland" or none leaves the choice here.
    const char* sdlDriver = std::getenv("SDL_VIDEODRIVER");
    const bool sdlNamed = sdlDriver && *sdlDriver && std::strcmp(sdlDriver, "wayland") != 0;
    const char* wl = std::getenv("WAYLAND_DISPLAY");
    if (forced == "wayland" || (forced.empty() && !sdlNamed && wl && *wl)) {
        selectWindowSystem(WindowSystemKind::Wayland);
        return;
    }
#endif
    if (!forced.empty() && forced != "wayland")
        LOG_WARN("BRO_WINDOW_SYSTEM=%s: unknown (sdl, wayland); using SDL", forced.c_str());
    selectWindowSystem(WindowSystemKind::Sdl);
}

const std::string& launchActivationToken() {
    static const std::string token = [] {
        std::string t;
        if (const char* env = std::getenv("XDG_ACTIVATION_TOKEN")) t = env;
#ifndef _WIN32
        unsetenv("XDG_ACTIVATION_TOKEN");
#endif
        return t;
    }();
    return token;
}

WindowSystem& windowSystem() {
    if (!g_active) g_active = &sdlWindowSystem();
    return *g_active;
}

void pumpEvents() { windowSystem().pumpEvents(); }

std::unique_ptr<Window> createWindow(const WindowConfig& config) {
    return windowSystem().createWindow(config);
}

Displays& displays() { return windowSystem().displays(); }
Keyboard& keyboard() { return windowSystem().keyboard(); }
Clipboard& clipboard() { return windowSystem().clipboard(); }
SystemInfo& systemInfo() { return windowSystem().systemInfo(); }
Gamepads& gamepads() { return windowSystem().gamepads(); }

bool setClipboardText(const std::string& text) { return clipboard().setText(text); }
std::string getClipboardText(bool* ok) { return clipboard().getText(ok); }

// --- Window: the parts every backend shares ---

float Window::getDevicePixelRatio() const {
#ifdef __APPLE__
    return getPixelDensity();
#else
    return getDisplayScale();
#endif
}

std::vector<DisplayInfo> Window::getDisplays() const { return displays().list(this); }

std::vector<DisplayModeInfo> Window::getDisplayModes() const {
    const uint32_t id = currentDisplay();
    if (!id) return {};
    return displays().fullscreenModes(id);
}

// --- EventLoop: the frame loop around a backend's pollEvents ---

void EventLoop::updateTiming() {
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (m_lastFrameTime == 0) {
        m_lastFrameTime = now;
        m_deltaTime = 0.0f;
        return;
    }
    m_deltaTime = static_cast<float>(static_cast<double>(now - m_lastFrameTime) / 1e9);
    m_lastFrameTime = now;
}

void EventLoop::run(std::function<void(float deltaTime)> perFrame) {
    m_quit = false;
    m_lastFrameTime = 0;

    LOG_INFO("Event loop started");

    while (!m_quit) {
        updateTiming();
        pollEvents();

        if (!m_quit && perFrame) {
            perFrame(m_deltaTime);
        }
    }

    LOG_INFO("Event loop ended");
}

}  // namespace bro::platform
