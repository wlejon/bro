#pragma once

#include <qjsbind/qjsbind.h>

namespace bro::net { class NetService; }

namespace bro::js {

/// Per-JSContext bindings for bro.net.*.
///
/// Each JS context gets its own NetSubscriber attached to the shared
/// NetService. install() must be called once per context; cleanup() releases
/// the subscriber and detaches callbacks.
class NetBindings {
public:
    static void install(JSContext* ctx, net::NetService* service);
    static void cleanup(JSContext* ctx);

    /// Drain queued network events for this context and fire JS callbacks.
    /// Call once per frame from the owning thread's main loop.
    static void poll(JSContext* ctx);

    /// True if this context's subscriber can still receive events without
    /// further JS action: hosting (or host result pending), an outgoing
    /// connect in flight, live connections, or events already queued.
    /// Worker event loops keep polling at tick cadence while this holds
    /// instead of blocking. Call from the context's own thread.
    static bool hasActivity(JSContext* ctx);
};

} // namespace bro::js
