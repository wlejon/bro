#include "platform/desktop_tray.h"
#include "platform/desktop_platform.h"

#include <atomic>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace bro::platform::desktop {

namespace {

std::mutex s_trayMutex;
TrayConfig s_recordedTray;
std::atomic<bool> s_hasTray{false};

#ifdef _WIN32
constexpr UINT kAppTrayId = 0xB8;
HWND g_trayHwnd = nullptr;
#endif

} // namespace

bool isTrayAvailable() {
    if (isHeadless()) return true;
#ifdef _WIN32
    return true;
#elif defined(__APPLE__)
    return true;
#else
    // On Linux, tray is available if running under X11 or a desktop with StatusNotifierItem
    return getenv("DISPLAY") != nullptr || getenv("WAYLAND_DISPLAY") != nullptr;
#endif
}

bool setTray(const Window* window, const TrayConfig& config) {
    {
        std::lock_guard<std::mutex> lock(s_trayMutex);
        s_recordedTray = config;
        s_hasTray.store(true, std::memory_order_relaxed);
    }

    if (isHeadless()) {
        return true;
    }

#ifdef _WIN32
    HWND hwnd = hwndOf(window);
    if (!hwnd) return false;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kAppTrayId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_APP + 101;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    copyWide(nid.szTip, ARRAYSIZE(nid.szTip), utf8ToWide(config.tooltip.empty() ? "bro" : config.tooltip));

    if (!g_trayHwnd) {
        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            g_trayHwnd = hwnd;
        }
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    return true;
#else
    (void)window;
    return true;
#endif
}

bool removeTray() {
    {
        std::lock_guard<std::mutex> lock(s_trayMutex);
        s_hasTray.store(false, std::memory_order_relaxed);
        s_recordedTray = {};
    }

#ifdef _WIN32
    if (g_trayHwnd) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_trayHwnd;
        nid.uID = kAppTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_trayHwnd = nullptr;
    }
#endif
    return true;
}

bool hasTray() {
    return s_hasTray.load(std::memory_order_relaxed);
}

const TrayConfig& getRecordedTray() {
    std::lock_guard<std::mutex> lock(s_trayMutex);
    return s_recordedTray;
}

} // namespace bro::platform::desktop
