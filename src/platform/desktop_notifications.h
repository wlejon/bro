#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bro::platform { class Window; }

namespace bro::platform::desktop {

/// A button on a notification: `id` comes back with the click.
struct NotificationAction {
    std::string id;
    std::string title;
};

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
    std::vector<NotificationAction> actions;
    // Opaque text the click hands back, also to a later run of the app that
    // the click started (the page's Notification as JSON).
    std::string payload;
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

/// What the user did with a notification: clicked it (or one of its actions),
/// or dismissed it.
struct NotificationActivation {
    bool close = false;    // dismissed rather than clicked
    uint32_t id = 0;       // the id showNotification returned; 0 when posted by an earlier run
    std::string action;    // the clicked action's id; "" for the notification itself
    std::string payload;   // NotificationOptions::payload as posted
    bool earlierRun = false;  // posted by an earlier run of the app (a click that started this one)
};

/// The activations the desktop reported since the last call, oldest first.
/// The platforms' callbacks queue them from their own threads (a toast's COM
/// activator, D-Bus signals, the notification center's delegate); the page
/// thread takes them once a frame.
std::vector<NotificationActivation> takeNotificationActivations();

/// Headless: what the desktop would report for a click on notification `id`
/// (`action` "" for the notification itself) or, with `close`, its
/// dismissal. False when no notification with that id was posted.
bool simulateNotificationActivation(uint32_t id, const std::string& action, bool close);

/// The text a click carries through the OS and back (a toast's launch and
/// action arguments, `bro --notification <args>`): this run's identity, the
/// notification's id, the action and the payload. decode sets
/// `out.earlierRun` when the run that posted it is not this one.
std::string encodeNotificationArgs(uint32_t id, const std::string& action, const std::string& payload);
bool decodeNotificationArgs(const std::string& args, NotificationActivation& out);

/// This run was started by a click on a notification, its activation text
/// `args` (`bro --notification <args>`): queued as that click.
void noteLaunchNotification(const std::string& args);

/// Called once at launch by the windowed bro, before the page: prepares the
/// platform to report clicks for the app `appId` (macOS: the notification
/// center's delegate, which must be set before the app finishes launching;
/// Windows: the toast activator, when the app has notified before, so a
/// click on a toast left in the Action Center reaches this run).
/// `exePath` and `appDir` are what a click on a toast starts when the app is
/// not running. `comLaunch`: this run was started by a toast (Windows,
/// `bro --notification-activated`).
void initNotificationActivation(const std::string& appId, const std::string& exePath,
                                const std::string& appDir, bool comLaunch);
/// What initNotificationActivation was given ("" before).
const std::string& notificationLaunchExe();
const std::string& notificationLaunchAppDir();

#ifdef _WIN32
/// desktop_notifications_win.cpp: a toast under `aumid`, registered for the
/// current user (HKCU\Software\Classes\AppUserModelId) when it is not yet.
/// False when toasts are unavailable.
bool showWindowsToast(const std::string& aumid, const std::string& appName, const std::string& title,
                      const std::string& body, const NotificationOptions& options, uint32_t id);
/// The toast XML (exposed for tests): title, body, an image for the icon,
/// silent audio, a long duration for timeoutMs == 0, the activation
/// arguments of the toast itself and of each action button.
std::string windowsToastXml(const std::string& title, const std::string& body,
                            const NotificationOptions& options, uint32_t id = 0);
/// The COM class a click on `aumid`'s toasts activates (a name-based GUID,
/// "{...}"), registered as the AUMID's CustomActivator.
std::string windowsToastActivatorClsid(const std::string& aumid);
/// Windows half of initNotificationActivation / the first toast.
void initWindowsToastActivation(const std::string& aumid, bool comLaunch, bool onlyIfRegistered);
/// Queues an activation from a platform callback.
void queueNotificationActivation(NotificationActivation a);
#elif defined(__APPLE__)
/// desktop_notifications_mac.mm: UNUserNotificationCenter when the process
/// is a bundle with an identifier. False otherwise.
bool showMacUserNotification(const std::string& title, const std::string& body,
                             const NotificationOptions& options, uint32_t id);
/// Sets the notification center's delegate (clicks, actions, dismissals).
void initMacNotificationDelegate();
void queueNotificationActivation(NotificationActivation a);
#else
void queueNotificationActivation(NotificationActivation a);
#endif

} // namespace bro::platform::desktop
