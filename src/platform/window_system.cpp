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

namespace bro::platform {

namespace {
WindowSystem* g_active = nullptr;
}  // namespace

void selectWindowSystem(WindowSystemKind kind) {
    WindowSystem* next = kind == WindowSystemKind::Drm ? &drmWindowSystem() : &sdlWindowSystem();
    if (g_active != next) LOG_INFO("Window system: %s", next->name());
    g_active = next;
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
