#include "steam/steam_service.h"
#include "steam/steam_flat.h"
#include "util/log.h"

#include <chrono>
#include <cstring>

namespace bro::steam {

SteamService* SteamService::s_instance = nullptr;

// Interface accessor version strings track SDK releases; the flat *method*
// wrappers do not. We probe a list until FindOrCreateUserInterface resolves.
//
// IMPORTANT ordering hazard: the flat wrappers in steam_api64.dll are compiled
// against ONE SDK version and bake in that interface's vtable layout. Asking for
// a version NEWER than the DLL can hand back a non-null but layout-wrong object —
// early methods may appear to work while a later one jumps off the vtable and
// crashes. (Observed: requesting "SteamFriends018" against the SDK 1.57 DLL made
// GetFriendCount segfault while GetPersonaName still returned the right name.)
// So the version MATCHING the shipped redistributable must come first; these
// lists lead with SDK 1.57's versions (our reference redistributable).
namespace {
const char* kUserVersions[]    = { "SteamUser023", "SteamUser022", "SteamUser021", "SteamUser020" };
const char* kFriendsVersions[] = { "SteamFriends017", "SteamFriends018", "SteamFriends015" };
const char* kUtilsVersions[]   = { "SteamUtils010", "SteamUtils009" };

// k_EFriendFlagImmediate — "regular" friends (the people on your friends list).
constexpr int kFriendFlagImmediate = 0x04;

// Rebuild the friends snapshot at ~2 Hz (every 50 * 10 ms pump iterations).
constexpr uint64_t kFriendsRebuildEvery = 50;

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
            case SteamEvent::FriendsUpdated:
                if (ev->friends) {
                    if (onFriends) onFriends(*ev->friends);
                    delete ev->friends; // ownership transferred to us via the event
                }
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

void SteamService::setRichPresence(const std::string& key, const std::string& value) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::SetRichPresence;
    c->strA = key;
    c->strB = value;
    postCommand(c);
}

void SteamService::clearRichPresence() {
    auto* c = new SteamCommand();
    c->type = SteamCommand::ClearRichPresence;
    postCommand(c);
}

void SteamService::activateOverlay(const std::string& dialog) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::ActivateOverlay;
    c->strA = dialog;
    postCommand(c);
}

void SteamService::activateOverlayToUser(const std::string& dialog, uint64_t steamId) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::ActivateOverlayToUser;
    c->strA = dialog;
    c->u64 = steamId;
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
            // Hand the newcomer the current snapshot so it isn't blank until the
            // next change (buildAndEmitFriends only emits on a diff).
            emitFriendsTo(cmd.subscriberId);
            break;
        case SteamCommand::Unregister:
            subscribers_.erase(cmd.subscriberId);
            delete cmd.subscriberPtr;
            break;
        case SteamCommand::SetRichPresence:
            if (iFriends_ && api_.Friends_SetRichPresence)
                api_.Friends_SetRichPresence(iFriends_, cmd.strA.c_str(), cmd.strB.c_str());
            break;
        case SteamCommand::ClearRichPresence:
            if (iFriends_ && api_.Friends_ClearRichPresence)
                api_.Friends_ClearRichPresence(iFriends_);
            break;
        case SteamCommand::ActivateOverlay:
            if (iFriends_ && api_.Friends_ActivateGameOverlay)
                api_.Friends_ActivateGameOverlay(iFriends_, cmd.strA.c_str());
            break;
        case SteamCommand::ActivateOverlayToUser:
            if (iFriends_ && api_.Friends_ActivateGameOverlayToUser)
                api_.Friends_ActivateGameOverlayToUser(iFriends_, cmd.strA.c_str(), cmd.u64);
            break;
    }
}

// Build a fresh friends snapshot from the Steam API (service thread only). Emits
// a FriendsUpdated event to every subscriber only when the list actually changed,
// so a steady state produces no traffic.
void SteamService::buildAndEmitFriends() {
    if (!iFriends_ || !api_.Friends_GetFriendCount || !api_.Friends_GetFriendByIndex)
        return;

    std::vector<FriendInfo> list;
    int n = api_.Friends_GetFriendCount(iFriends_, kFriendFlagImmediate);
    if (n > 0) list.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        uint64_t fid = api_.Friends_GetFriendByIndex(iFriends_, i, kFriendFlagImmediate);
        FriendInfo fi;
        fi.steamId = fid;
        if (api_.Friends_GetFriendPersonaName) {
            const char* nm = api_.Friends_GetFriendPersonaName(iFriends_, fid);
            fi.name = nm ? nm : "";
        }
        if (api_.Friends_GetFriendPersonaState)
            fi.personaState = api_.Friends_GetFriendPersonaState(iFriends_, fid);
        if (api_.Friends_GetFriendRelationship)
            fi.relationship = api_.Friends_GetFriendRelationship(iFriends_, fid);
        list.push_back(std::move(fi));
    }

    if (list == lastFriends_) return; // no change → no event
    lastFriends_ = std::move(list);
    for (auto& [sid, sub] : subscribers_) emitFriendsTo(sid);
}

void SteamService::emitFriendsTo(uint32_t subscriberId) {
    auto* ev = new SteamEvent();
    ev->type = SteamEvent::FriendsUpdated;
    ev->friends = new std::vector<FriendInfo>(lastFriends_); // per-subscriber copy
    postEventTo(subscriberId, ev);
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
    bool steamUp = false;

    if (!loadSteamFlat(api_)) {
        status_.store(Status::LibraryNotFound, std::memory_order_release);
        LOG_INFO("[steam] Steam redistributable not found — bro.steam.available = false");
    } else {
        // steam_appid.txt (containing 480 for dev against Spacewar) must sit
        // next to the executable, or the process must own the app id.
        bool ok = false;
        if (api_.Init) {
            ok = api_.Init();
        } else if (api_.InitFlat) {
            char err[1024] = {0};
            ok = (api_.InitFlat(err) == 0); // ESteamAPIInitResult: 0 == OK
            if (!ok && err[0]) LOG_ERROR("[steam] SteamAPI_InitFlat: %s", err);
        }

        if (!ok) {
            status_.store(Status::InitFailed, std::memory_order_release);
            LOG_ERROR("[steam] SteamAPI init failed — is the Steam client running and logged in?");
            unloadSteamFlat(api_);
        } else {
            // Resolve the interface pointers once, here on the service thread;
            // they stay valid for the process and are touched only by this thread.
            HSteamUser hUser = api_.GetHSteamUser();
            iUser_    = findInterface(api_, hUser, kUserVersions,
                                      sizeof(kUserVersions)/sizeof(*kUserVersions));
            iFriends_ = findInterface(api_, hUser, kFriendsVersions,
                                      sizeof(kFriendsVersions)/sizeof(*kFriendsVersions));
            iUtils_   = findInterface(api_, hUser, kUtilsVersions,
                                      sizeof(kUtilsVersions)/sizeof(*kUtilsVersions));

            // Identity snapshot. Written before the Status::Available release
            // store, so JS-thread reads that observe Available see complete,
            // immutable values (lock-free publication).
            if (iUser_ && api_.User_GetSteamID)
                localSteamId_.store(api_.User_GetSteamID(iUser_), std::memory_order_relaxed);
            if (iFriends_ && api_.Friends_GetPersonaName) {
                const char* name = api_.Friends_GetPersonaName(iFriends_);
                personaName_ = name ? name : "";
            }
            if (iUtils_ && api_.Utils_GetAppID)
                appId_.store(api_.Utils_GetAppID(iUtils_), std::memory_order_relaxed);

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
            //    beyond lifecycle; lobby/voice land here via ManualDispatch).
            api_.RunCallbacks();

            ++iter;

            // 3. Friends snapshot (~2 Hz). Emits only on a diff, so a steady
            //    list produces no events.
            if (iter % kFriendsRebuildEvery == 0) buildAndEmitFriends();

            // 4. Heartbeat: ~1 Hz pulse to every subscriber so the lab can
            //    confirm the pump is alive (M1).
            if (iter % 100 == 0) {
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
        api_.Shutdown();
        unloadSteamFlat(api_);
    }
    LOG_INFO("[steam] SteamService stopped");
}

} // namespace bro::steam
