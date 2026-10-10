// Notification clicks, action buttons and dismissals reach the page
// (docs/sys-api.js, 5a). The platforms report them from their own threads
// (platform/desktop_notifications.h: a toast's COM activator, D-Bus
// ActionInvoked / NotificationClosed, the notification center's delegate, a
// `--notification` launch, headless clickNotification); a frame pump here
// takes them on the page thread and hands each to js/notification.js, which
// fires `click` / `close` on the page's Notification or, when the page holds
// none for it (a click that started this run, or a page reloaded since),
// `notificationclick` / `notificationclose` on window. A click also brings the
// app's window forward.

#include "bronze_host/host_notification.h"

#include "bronze_host/host_runtime.h"
#include "engine/engine.h"
#include "platform/desktop_notifications.h"
#include "platform/window.h"

#include "embed/embed.h"

#include <mutex>

namespace bro::bronze_host {

namespace ev = bronze::embed;
using ev::Value;

namespace {

std::mutex g_logMutex;
std::vector<DeliveredNotificationActivation> g_log;

void deliver(engine::Engine& engine, const platform::desktop::NotificationActivation& a) {
    bool raised = false;
    if (!a.close) {
        // A click asks to see the app: forward, and out of the taskbar if it
        // was minimized. Headless has no window to show; it records the ask.
        if (engine.displayMode() == engine::DisplayMode::Headless) {
            raised = true;
        } else if (auto* w = engine.window()) {
            if (w->isMinimized()) w->restore();
            w->raise();
            raised = true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_log.push_back({a.close ? "close" : "click", a.id, a.action, a.payload, a.earlierRun, raised});
    }
    ev::GlobalValue fn = ev::globalValue("__bro_notificationActivated");
    if (!fn.found || !ev::isFunction(fn.value)) return;
    ev::Persistent f(fn.value);
    ev::Persistent type(ev::fromUtf8(a.close ? "close" : "click"));
    ev::Persistent action(ev::fromUtf8(a.action));
    ev::Persistent payload(ev::fromUtf8(a.payload));
    Value args[5] = {type.get(), ev::fromDouble(static_cast<double>(a.id)), action.get(), payload.get(),
                     ev::fromBool(a.earlierRun)};
    ev::CallResult res = ev::call(f.get(), ev::undefined(), args);
    (void)res;  // notification.js reports a throwing listener itself
}

}  // namespace

void installNotificationPump(engine::Engine& engine) {
    engine.addFramePump([&engine] {
        // Held until the page has loaded: a click that started this run is
        // delivered to a page whose listeners are in place.
        if (engine.documentReadyState() != "complete") return;
        auto list = platform::desktop::takeNotificationActivations();
        if (list.empty()) return;
        for (const auto& a : list) deliver(engine, a);
        if (ev::microtasksPending()) ev::drainMicrotasks();
    });
}

std::vector<DeliveredNotificationActivation> deliveredNotificationActivations(bool clear) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::vector<DeliveredNotificationActivation> out = g_log;
    if (clear) g_log.clear();
    return out;
}

}  // namespace bro::bronze_host
