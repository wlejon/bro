#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bro::platform { class Window; }

namespace bro::platform::desktop {

struct NotificationOptions {
    std::string icon;
    std::string sound;
    int32_t timeoutMs = -1;
    bool silent = false;
    uint32_t replacesId = 0;
};

struct NotificationRecord {
    uint32_t id = 0;
    std::string title;
    std::string body;
    NotificationOptions options;
};

/// Shows a desktop notification and returns a notification ID (> 0 on success).
uint32_t showNotification(
    const Window* window,
    const std::string& title,
    const std::string& body,
    const NotificationOptions& options = {}
);

/// Headless inspection and testing.
std::vector<NotificationRecord> getRecordedNotifications();
void clearRecordedNotifications();

} // namespace bro::platform::desktop
