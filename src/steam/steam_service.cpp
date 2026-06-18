#include "steam/steam_service.h"
#include "steam/steam_flat.h"
#include "util/log.h"

#include <chrono>
#include <cstring>

namespace bro::steam {

SteamService* SteamService::s_instance = nullptr;

// Interface accessor version strings track SDK releases; the flat *method*
// wrappers do not. Probe newest-first until FindOrCreateUserInterface resolves.
namespace {
const char* kUserVersions[]    = { "SteamUser023", "SteamUser022", "SteamUser021", "SteamUser020" };
const char* kFriendsVersions[] = { "SteamFriends018", "SteamFriends017", "SteamFriends015" };
const char* kUtilsVersions[]   = { "SteamUtils010", "SteamUtils009" };

void* findInterface(const SteamFlatApi& api, HSteamUser user,
                    const char* const* versions, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        void* iface = api.FindOrCreateUserInterface(user, versions[i]);
        if (iface) return iface;
    }
    return nullptr;
}
} // namespace

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
    status_.store(Status::Initializing, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&SteamService::threadMain, this);
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
        case Status::Initializing:    return "initializing";
        case Status::Available:       return "ok";
        case Status::LibraryNotFound: return "Steam library not found — place the redistributable "
                                             "(steam_api64.dll / libsteam_api.*) next to the executable "
                                             "with the Steam client running";
        case Status::InitFailed:      return "SteamAPI init failed — is the Steam client running and "
                                             "logged in? (steam_appid.txt next to the executable for dev)";
    }
    return "unknown";
}

SteamSubscriber* SteamService::createSubscriber() {
    uint32_t id = nextSubscriberId_.fetch_add(1, std::memory_order_relaxed);
    auto* sub = new SteamSubscriber(this, id);
    auto* c = new SteamCommand();
    c->type = SteamCommand::Register;
    c->subscriberId = id;
    c->subscriberPtr = sub;
    postCommand(c);
    return sub;
}

void SteamService::destroySubscriber(SteamSubscriber* sub) {
    if (!sub) return;
    auto* c = new SteamCommand();
    c->type = SteamCommand::Unregister;
    c->subscriberId = sub->id();
    c->subscriberPtr = sub;
    postCommand(c);
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

// ---------------------------------------------------------------------------
// Service thread main — loads the Steam redistributable, owns SteamAPI init,
// the RunCallbacks pump, and (as the surface grows) every callback handler. ALL
// SteamAPI calls happen on this thread.
//
// The command loop runs whether or not Steam is available, so subscriber
// register/unregister is always handled correctly — when Steam is down the loop
// simply skips the pump.
// ---------------------------------------------------------------------------
void SteamService::threadMain() {
    SteamFlatApi api;
    bool steamUp = false;

    if (!loadSteamFlat(api)) {
        status_.store(Status::LibraryNotFound, std::memory_order_release);
        LOG_INFO("[steam] Steam redistributable not found — bro.steam.available = false");
    } else {
        // steam_appid.txt (containing 480 for dev against Spacewar) must sit
        // next to the executable, or the process must own the app id.
        bool ok = false;
        if (api.Init) {
            ok = api.Init();
        } else if (api.InitFlat) {
            char err[1024] = {0};
            ok = (api.InitFlat(err) == 0); // ESteamAPIInitResult: 0 == OK
            if (!ok && err[0]) LOG_ERROR("[steam] SteamAPI_InitFlat: %s", err);
        }

        if (!ok) {
            status_.store(Status::InitFailed, std::memory_order_release);
            LOG_ERROR("[steam] SteamAPI init failed — is the Steam client running and logged in?");
            unloadSteamFlat(api);
        } else {
            // Identity snapshot. Written before the Status::Available release
            // store, so JS-thread reads that observe Available see complete,
            // immutable values (lock-free publication).
            HSteamUser hUser = api.GetHSteamUser();
            if (void* user = findInterface(api, hUser, kUserVersions,
                                           sizeof(kUserVersions)/sizeof(*kUserVersions))) {
                if (api.User_GetSteamID)
                    localSteamId_.store(api.User_GetSteamID(user), std::memory_order_relaxed);
            }
            if (void* friends = findInterface(api, hUser, kFriendsVersions,
                                              sizeof(kFriendsVersions)/sizeof(*kFriendsVersions))) {
                if (api.Friends_GetPersonaName) {
                    const char* name = api.Friends_GetPersonaName(friends);
                    personaName_ = name ? name : "";
                }
            }
            if (void* utils = findInterface(api, hUser, kUtilsVersions,
                                            sizeof(kUtilsVersions)/sizeof(*kUtilsVersions))) {
                if (api.Utils_GetAppID)
                    appId_.store(api.Utils_GetAppID(utils), std::memory_order_relaxed);
            }

            status_.store(Status::Available, std::memory_order_release);
            steamUp = true;
            LOG_INFO("[steam] SteamService started (steamId=%llu persona='%s' appId=%u)",
                     static_cast<unsigned long long>(localSteamId_.load(std::memory_order_relaxed)),
                     personaName_.c_str(),
                     appId_.load(std::memory_order_relaxed));
        }
    }

    uint64_t iter = 0;
    while (running_.load(std::memory_order_acquire)) {
        // 1. Drain and process commands (FIFO) — always, Steam up or not.
        SteamCommand* cmd = drainAndReverse(cmdHead_);
        while (cmd) {
            SteamCommand* next = cmd->next;
            handleCommand(*cmd);
            delete cmd;
            cmd = next;
        }

        if (steamUp) {
            // 2. Pump Steam callbacks — fires registered handlers (none yet
            //    beyond lifecycle; friends/lobby/voice land here).
            api.RunCallbacks();

            // 3. Heartbeat: ~1 Hz pulse to every subscriber so the lab can
            //    confirm the pump is alive (M1). Repurposed once real callbacks
            //    drive the event stream.
            if (++iter % 100 == 0) {
                for (auto& [sid, sub] : subscribers_) {
                    auto* ev = new SteamEvent();
                    ev->type = SteamEvent::Pulse;
                    ev->u64 = iter / 100;
                    postEventTo(sid, ev);
                }
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

    if (steamUp) {
        api.Shutdown();
        unloadSteamFlat(api);
    }
    LOG_INFO("[steam] SteamService stopped");
}

} // namespace bro::steam
