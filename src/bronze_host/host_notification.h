#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

/// The frame pump that hands notification clicks and dismissals to the page
/// (host_notification.cpp). Installed once, with the Notification module.
void installNotificationPump(engine::Engine& engine);

/// What was handed to the page, oldest first (headless
/// notificationActivations()): "click" or "close", the notification's id (0
/// for one an earlier run posted), the action, the payload, whether an
/// earlier run posted it, and whether the window was brought forward.
struct DeliveredNotificationActivation {
    std::string type;
    uint32_t id = 0;
    std::string action;
    std::string payload;
    bool earlierRun = false;
    bool raised = false;
};
std::vector<DeliveredNotificationActivation> deliveredNotificationActivations(bool clear);

}  // namespace bro::bronze_host
