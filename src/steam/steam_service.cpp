#include "steam/steam_service.h"
#include "util/log.h"

#ifdef BRO_WITH_STEAM
// The full Steamworks SDK header — pulls in ISteamUser / ISteamFriends /
// ISteamUtils and the SteamAPI_* lifecycle functions. Included ONLY here and
// ONLY when the SDK is present, so its superset `steam/` tree never collides
// with the GameNetworkingSockets `steam/` tree used by bro::net.
#include <steam/steam_api.h>
#include <chrono>
#endif

namespace bro::steam {

SteamService* SteamService::s_instance = nullptr;

// =============================================================================
// SteamSubscriber
// =============================================================================

void SteamSubscriber::poll() {
    while (SteamEvent* ev = events_.pop()) {
        switch (ev->type) {
            case SteamEvent::Pulse:
                if (onPulse) onPulse(ev->u64);
                break;
        }
        delete ev;
    }
}

// =============================================================================
// SteamService
// =============================================================================

SteamService::SteamService() {
    if (s_instance) {
        LOG_ERROR("[steam] SteamService: second instance created — there can be only one.");
    }
    s_instance = this;

#ifdef BRO_WITH_STEAM
    status_.store(Status::Initializing, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&SteamService::threadMain, this);
#else
    // No SDK compiled in: the service is inert. bro.steam still exists and
    // reports { available: false, reason: "built without Steam support" }.
    status_.store(Status::NoSupport, std::memory_order_release);
#endif
}

SteamService::~SteamService() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();

    // Free any command nodes that arrived after the thread drained/exited. Do
    // NOT delete subscriberPtr here — the thread's shutdown already freed the
    // subscribers it owned (double-free risk otherwise).
    SteamCommand* head = cmdHead_.exchange(nullptr, std::memory_order_acquire);
    while (head) {
        SteamCommand* next = head->next;
        delete head;
        head = next;
    }

    if (s_instance == this) s_instance = nullptr;
}

const char* SteamService::reason() const {
    switch (status_.load(std::memory_order_acquire)) {
        case Status::Initializing: return "initializing";
        case Status::Available:    return "ok";
        case Status::NoSupport:    return "built without Steam support (BRO_WITH_STEAM=OFF)";
        case Status::InitFailed:   return "SteamAPI_Init failed — is the Steam client running and logged in?";
    }
    return "unknown";
}

SteamSubscriber* SteamService::createSubscriber() {
    uint32_t id = nextSubscriberId_.fetch_add(1, std::memory_order_relaxed);
    auto* sub = new SteamSubscriber(this, id);
#ifdef BRO_WITH_STEAM
    auto* c = new SteamCommand();
    c->type = SteamCommand::Register;
    c->subscriberId = id;
    c->subscriberPtr = sub;
    postCommand(c);
#else
    // No service thread to register with — the subscriber is just an inert
    // handle whose poll() never has events. Tracked nowhere; freed directly in
    // destroySubscriber().
#endif
    return sub;
}

void SteamService::destroySubscriber(SteamSubscriber* sub) {
    if (!sub) return;
#ifdef BRO_WITH_STEAM
    auto* c = new SteamCommand();
    c->type = SteamCommand::Unregister;
    c->subscriberId = sub->id();
    c->subscriberPtr = sub;
    postCommand(c);
#else
    delete sub; // no service thread owns it
#endif
}

// ---------------------------------------------------------------------------
// Lock-free MPSC push — CAS the node onto the stack head.
// ---------------------------------------------------------------------------
void SteamService::postCommand(SteamCommand* cmd) {
    SteamCommand* old = cmdHead_.load(std::memory_order_relaxed);
    do {
        cmd->next = old;
    } while (!cmdHead_.compare_exchange_weak(
        old, cmd,
        std::memory_order_release,
        std::memory_order_relaxed));
}

void SteamService::postEventTo(uint32_t subscriberId, SteamEvent* ev) {
    // Service-thread only. subscribers_ is owned by this thread.
    auto it = subscribers_.find(subscriberId);
    if (it == subscribers_.end()) { delete ev; return; }
    if (!it->second->events_.push(ev)) {
        LOG_WARN("[steam] Event queue full for subscriber %u — dropping event", subscriberId);
        delete ev;
    }
}

void SteamService::handleCommand(SteamCommand& cmd) {
    switch (cmd.type) {
        case SteamCommand::Register:
            subscribers_[cmd.subscriberId] = cmd.subscriberPtr;
            break;
        case SteamCommand::Unregister:
            subscribers_.erase(cmd.subscriberId);
            delete cmd.subscriberPtr;
            break;
    }
}

#ifdef BRO_WITH_STEAM
// ---------------------------------------------------------------------------
// Drain the MPSC stack and reverse LIFO → FIFO for ordered processing.
// ---------------------------------------------------------------------------
static SteamCommand* drainAndReverse(std::atomic<SteamCommand*>& head) {
    SteamCommand* lifo = head.exchange(nullptr, std::memory_order_acquire);
    SteamCommand* fifo = nullptr;
    while (lifo) {
        SteamCommand* next = lifo->next;
        lifo->next = fifo;
        fifo = lifo;
        lifo = next;
    }
    return fifo;
}
#endif

// ---------------------------------------------------------------------------
// Service thread main — owns SteamAPI_Init, the RunCallbacks pump, and (as the
// surface grows) every CCallback/CCallResult registration. Compiled only with
// BRO_WITH_STEAM; in a stub build the thread is never started.
// ---------------------------------------------------------------------------
void SteamService::threadMain() {
#ifdef BRO_WITH_STEAM
    // steam_appid.txt (containing 480 for dev against Spacewar) must sit next to
    // the executable, or own the app id, for this to succeed.
    if (!SteamAPI_Init()) {
        status_.store(Status::InitFailed, std::memory_order_release);
        LOG_ERROR("[steam] SteamAPI_Init failed — is the Steam client running and logged in?");
        return;
    }

    // Identity snapshot. Written before the Status::Available release store, so
    // JS-thread reads that observe Available see complete, immutable values.
    if (ISteamUser* user = SteamUser()) {
        localSteamId_.store(user->GetSteamID().ConvertToUint64(), std::memory_order_relaxed);
    }
    if (ISteamFriends* friends = SteamFriends()) {
        const char* name = friends->GetPersonaName();
        personaName_ = name ? name : "";
    }
    if (ISteamUtils* utils = SteamUtils()) {
        appId_.store(utils->GetAppID(), std::memory_order_relaxed);
    }

    status_.store(Status::Available, std::memory_order_release);
    LOG_INFO("[steam] SteamService started (steamId=%llu persona='%s' appId=%u)",
             static_cast<unsigned long long>(localSteamId_.load(std::memory_order_relaxed)),
             personaName_.c_str(),
             appId_.load(std::memory_order_relaxed));

    uint64_t iter = 0;
    while (running_.load(std::memory_order_acquire)) {
        // 1. Drain and process commands (FIFO).
        SteamCommand* cmd = drainAndReverse(cmdHead_);
        while (cmd) {
            SteamCommand* next = cmd->next;
            handleCommand(*cmd);
            delete cmd;
            cmd = next;
        }

        // 2. Pump Steam callbacks — fires registered CCallback handlers (none
        //    yet beyond lifecycle; friends/lobby/voice land here).
        SteamAPI_RunCallbacks();

        // 3. Heartbeat: ~1 Hz pulse to every subscriber so the lab can confirm
        //    the pump is alive (M1). Cheap; removed/repurposed once real
        //    callbacks drive the event stream.
        if (++iter % 100 == 0) {
            for (auto& [sid, sub] : subscribers_) {
                auto* ev = new SteamEvent();
                ev->type = SteamEvent::Pulse;
                ev->u64 = iter / 100;
                postEventTo(sid, ev);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Drain late commands (e.g. Unregister posted between the last loop and the
    // running_=false observation) so subscribers don't leak.
    {
        SteamCommand* cmd = drainAndReverse(cmdHead_);
        while (cmd) {
            SteamCommand* next = cmd->next;
            handleCommand(*cmd);
            delete cmd;
            cmd = next;
        }
    }

    for (auto& [sid, sub] : subscribers_) delete sub;
    subscribers_.clear();

    SteamAPI_Shutdown();
    LOG_INFO("[steam] SteamService stopped");
#endif // BRO_WITH_STEAM
}

} // namespace bro::steam
