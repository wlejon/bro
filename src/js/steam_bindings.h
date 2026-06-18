#pragma once

#include <qjsbind/qjsbind.h>

namespace bro::steam { class SteamService; }

namespace bro::js {

/// Per-JSContext bindings for bro.steam.*.
///
/// Always-present runtime probe (mirrors bro.gpu): even in a stub build
/// (BRO_WITH_STEAM=OFF) bro.steam exists and reports { available: false,
/// reason }, so apps written against it load identically in a non-Steam build.
/// When the service is live, each context gets its own SteamSubscriber attached
/// to the shared SteamService.
class SteamBindings {
public:
    static void install(JSContext* ctx, steam::SteamService* service);
    static void cleanup(JSContext* ctx);

    /// Drain queued Steam events for this context and fire JS callbacks.
    /// Call once per frame from the owning thread's main loop.
    static void poll(JSContext* ctx);
};

} // namespace bro::js
