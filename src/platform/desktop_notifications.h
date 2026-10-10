#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bro::platform { class Window; }

namespace bro::platform::desktop {

struct NotificationOptions {
    std::string icon;       // an image file (absolute path); empty for the app's own
    std::string sound;
    int32_t timeoutMs = -1; // -1: the desktop's default; 0: until dismissed
    bool silent = false;
    uint32_t replacesId = 0;
    // Who is notifying: the app's id (Windows AppUserModelID, the desktop
    // entry a Linux notification server matches) and its display name.
    std::string appId;
    std::string appName;
};

struct NotificationRecord {
    uint32_t id = 0;
    std::string title;
    std::string body;
    NotificationOptions options;
    // How it was shown: "toast" / "balloon" (Windows), "usernotifications" /
    // "osascript" (macOS), "dbus" / "notify-send" (Linux), "headless" when
    // only recorded, "" when nothing could show it.
    std::string via;
};

/// Shows a desktop notification and returns a notification ID (> 0 on
/// success). Windows: a toast under the app's AppUserModelID (registered for
/// the current user on first use), else a tray balloon. macOS: the
/// UserNotifications framework in a bundle, else AppleScript's `display
/// notification`. Linux: org.freedesktop.Notifications over D-Bus, else
/// notify-send. Headless records it and shows nothing.
uint32_t showNotification(
    const Window* window,
    const std::string& title,
    const std::string& body,
    const NotificationOptions& options = {}
);

/// Headless inspection and testing.
std::vector<NotificationRecord> getRecordedNotifications();
void clearRecordedNotifications();

#ifdef _WIN32
/// desktop_notifications_win.cpp: a toast under `aumid`, registered for the
/// current user (HKCU\Software\Classes\AppUserModelId) when it is not yet.
/// False when toasts are unavailable.
bool showWindowsToast(const std::string& aumid, const std::string& appName, const std::string& title,
                      const std::string& body, const NotificationOptions& options, uint32_t id);
/// The toast XML (exposed for tests): title, body, an image for the icon,
/// silent audio, a long duration for timeoutMs == 0.
std::string windowsToastXml(const std::string& title, const std::string& body,
                            const NotificationOptions& options);
#elif defined(__APPLE__)
/// desktop_notifications_mac.mm: UNUserNotificationCenter when the process
/// is a bundle with an identifier. False otherwise.
bool showMacUserNotification(const std::string& title, const std::string& body,
                             const NotificationOptions& options, uint32_t id);
#endif

} // namespace bro::platform::desktop
