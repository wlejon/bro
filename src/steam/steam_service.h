#pragma once

#include "steam/steam_flat.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// SteamService owns the Steamworks API on a dedicated thread and marshals
// to/from JS contexts lock-free — a direct mirror of bro::net::NetService.
//
// bro depends on NO Steamworks SDK at build time. The service talks to the
// Steam redistributable (steam_api64.dll / libsteam_api.{so,dylib}) through the
// stable *flat C API*, resolved at runtime (see steam_flat.h). So there are no
// proprietary headers or import libs in the build, and the binding is always
// real — it just probes at runtime, exactly like bro.gpu / bro.tensor:
//   - redistributable present + Steam client running  -> available() == true
//   - library absent, or SteamAPI init fails           -> available() == false
// The JS surface (bro.steam) is always present and reports { available, reason }
// either way, so apps load identically with or without Steam.

namespace bro::steam {

// ---------------------------------------------------------------------------
// SPSC ring buffer (service thread → subscriber thread). Same lock-free shape
// as net's; kept local so the steam module stays self-contained.
// ---------------------------------------------------------------------------
template <typename T, size_t Capacity = 1024>
class Spsc {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of two");
public:
    bool push(T* item) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next = (tail + 1) & (Capacity - 1);
        if (next == head_.load(std::memory_order_acquire)) return false;
        slots_[tail] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }
    T* pop() {
        size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return nullptr;
        T* item = slots_[head];
        slots_[head] = nullptr;
        head_.store((head + 1) & (Capacity - 1), std::memory_order_release);
        return item;
    }
    void clear() { while (T* m = pop()) delete m; }
private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    T* slots_[Capacity]{};
};

// ---------------------------------------------------------------------------
// A friend, as snapshotted on the service thread (M2). Ownership of a snapshot
// vector transfers service-thread → subscriber-thread via a FriendsUpdated
// event, so the JS thread never reads Steam state concurrently (no locks).
// ---------------------------------------------------------------------------
struct FriendInfo {
    uint64_t steamId       = 0;
    std::string name;
    int      personaState  = 0; // EPersonaState (0 offline … 7 invisible)
    int      relationship  = 0; // EFriendRelationship (3 == friend)

    bool operator==(const FriendInfo& o) const {
        return steamId == o.steamId && name == o.name &&
               personaState == o.personaState && relationship == o.relationship;
    }
    bool operator!=(const FriendInfo& o) const { return !(*this == o); }
};

// Decoded avatar pixels (RGBA), returned to a getAvatar() request. Empty rgba
// (w==h==0) means "no avatar / not loaded yet" — re-request after onfriends.
struct AvatarData {
    uint32_t reqId = 0;
    int      w = 0;
    int      h = 0;
    std::vector<uint8_t> rgba;
};

// ---------------------------------------------------------------------------
// Lobby snapshot (M3). Built on the service thread whenever a lobby I'm in
// changes, then handed to subscribers via a LobbyUpdated event — the JS thread
// keeps a cache and reads getLobbyMembers/getLobbyOwner/getLobbyData from it
// synchronously, never touching the Steam API itself (mirrors FriendInfo).
// ---------------------------------------------------------------------------
struct LobbyMember {
    uint64_t    steamId = 0;
    std::string name; // persona name (best-effort; may be empty until loaded)
};
struct LobbyState {
    uint64_t    lobbyId     = 0;
    uint64_t    owner       = 0;
    int         memberLimit = 0;
    std::vector<LobbyMember> members;
    std::vector<std::pair<std::string, std::string>> data; // lobby data key/value
};

// ---------------------------------------------------------------------------
// Command (subscriber thread → service thread). Intrusive node for the
// lock-free MPSC atomic stack.
// ---------------------------------------------------------------------------
struct SteamCommand {
    enum Type : uint8_t {
        Register,            // new subscriber joined (subscriberPtr set)
        Unregister,          // subscriber leaving (subscriberPtr set)
        SetRichPresence,     // strA=key, strB=value
        ClearRichPresence,   // (no args)
        ActivateOverlay,     // strA=dialog ("friends", "settings", ...)
        ActivateOverlayToUser, // strA=dialog ("steamid", "chat", ...), u64=target
        RequestAvatar,       // u64=friend steamId, i32=size (0/1/2), reqId, subscriberId
        // --- lobbies (M3) ---
        CreateLobby,         // i32=ELobbyType, i32b=maxMembers, reqId, subscriberId
        JoinLobby,           // u64=lobbyId, reqId, subscriberId
        LeaveLobby,          // u64=lobbyId
        SetLobbyData,        // u64=lobbyId, strA=key, strB=value
        SetLobbyMemberData,  // u64=lobbyId, strA=key, strB=value
        SetLobbyJoinable,    // u64=lobbyId, i32=joinable(0/1)
        SetLobbyType,        // u64=lobbyId, i32=ELobbyType
        SetLobbyMemberLimit, // u64=lobbyId, i32=maxMembers
    };
    Type type;
    uint32_t subscriberId = 0;
    class SteamSubscriber* subscriberPtr = nullptr; // Register/Unregister
    std::string strA;                               // RichPresence/Overlay/Lobby key
    std::string strB;                               // value
    uint64_t u64 = 0;                               // overlay/avatar/lobby target id
    uint32_t reqId = 0;                             // async-call correlation id
    int32_t  i32 = 0;                               // avatar size / lobby type / limit / flag
    int32_t  i32b = 0;                              // CreateLobby maxMembers
    SteamCommand* next = nullptr;                   // MPSC stack link
};

// ---------------------------------------------------------------------------
// Event (service thread → subscriber thread).
// ---------------------------------------------------------------------------
struct SteamEvent {
    enum Type : uint8_t {
        Pulse,           // RunCallbacks heartbeat — proves the service pump is alive
        FriendsUpdated,  // friends snapshot changed; `friends` owned by this event
        OverlayActivated,// Steam overlay opened/closed; u64 = active (1/0)
        JoinRequested,   // friend invited us via overlay/rich-presence;
                         //   u64 = friend steamId, str = the connect string
        AvatarData_,     // getAvatar() result; `avatar` owned by this event
        // --- lobbies (M3) ---
        LobbyCreated,    // createLobby() result; reqId, u64=lobbyId, i32=success(1/0)
        LobbyEntered,    // entered a lobby; reqId(0 if spontaneous), u64=lobbyId, i32=response
        LobbyUpdated,    // lobby membership/data changed; `lobby` owned by this event
        LobbyLeft,       // left / removed from a lobby; u64=lobbyId
    };
    Type type;
    uint64_t u64 = 0;
    uint64_t u64b = 0;                          // secondary id (reserved)
    int32_t  i32 = 0;                           // success / enter-response
    uint32_t reqId = 0;                         // async-call correlation id
    std::string str;                            // JoinRequested connect string
    std::vector<FriendInfo>* friends = nullptr; // FriendsUpdated only; poll() deletes
    AvatarData* avatar = nullptr;               // AvatarData_ only; poll() deletes
    LobbyState* lobby = nullptr;                // LobbyUpdated only; poll() deletes
};

class SteamService;

// ---------------------------------------------------------------------------
// SteamSubscriber — a per-JSContext handle into the SteamService.
//
// Method calls push commands; poll() drains the event queue and fires
// callbacks synchronously on the calling thread, once per frame.
// ---------------------------------------------------------------------------
class SteamSubscriber {
public:
    uint32_t id() const { return id_; }

    /// Drain queued events and fire callbacks. Call once per frame.
    void poll();

    // Callbacks — bindings set these. Fire on the subscriber's thread during
    // poll(). More land here as the lobby/voice/UGC layers come online.
    std::function<void(uint64_t tick)> onPulse;
    std::function<void(const std::vector<FriendInfo>&)> onFriends;
    std::function<void(bool active)> onOverlay;
    std::function<void(uint64_t friendSteamId, const std::string& connect)> onJoinRequest;
    std::function<void(uint32_t reqId, int w, int h, const uint8_t* rgba, size_t len)> onAvatar;
    // Lobbies (M3). onLobbyCreated/onLobbyEntered carry reqId==0 for events not
    // tied to a pending promise (e.g. entering via an accepted invite).
    std::function<void(uint32_t reqId, uint64_t lobbyId, bool success)> onLobbyCreated;
    // fireEvent=false means "settle a late-arriving promise only, don't refire
    // the onlobbyentered event" (the entry was already announced).
    std::function<void(uint32_t reqId, uint64_t lobbyId, int response, bool fireEvent)> onLobbyEntered;
    std::function<void(const LobbyState&)> onLobbyUpdated;
    std::function<void(uint64_t lobbyId)>  onLobbyLeft;

private:
    friend class SteamService;
    SteamSubscriber(SteamService* service, uint32_t id) : service_(service), id_(id) {}

    SteamService* service_;
    uint32_t id_;
    Spsc<SteamEvent> events_;
};

// ---------------------------------------------------------------------------
// SteamService — owns the Steamworks API on a dedicated thread.
//
// Construct once per process (Engine owns it). Subscribers are created and
// destroyed by JS bindings as contexts come and go. SteamAPI_Init() runs on
// the service thread at startup; ALL SteamAPI calls happen on that thread.
// ---------------------------------------------------------------------------
class SteamService {
public:
    SteamService();
    ~SteamService();

    SteamService(const SteamService&) = delete;
    SteamService& operator=(const SteamService&) = delete;

    /// Initialization status. Maps to a stable string via reason(); exposed as
    /// atomics so JS-thread reads never share a std::string with the service
    /// thread (no mutex). Identity fields below are published-before-Available.
    enum class Status : int {
        Initializing    = 0, // thread spinning up, loading + init in flight
        Available       = 1, // SteamAPI init succeeded; identity valid
        LibraryNotFound = 2, // steam_api64.dll / libsteam_api.* not loadable
        InitFailed      = 3, // SteamAPI init failed (client not running / not logged in)
    };

    bool available() const {
        return status_.load(std::memory_order_acquire) == Status::Available;
    }
    Status status() const { return status_.load(std::memory_order_acquire); }
    const char* reason() const;

    // Identity — valid only when available(). Written once on the service
    // thread before status_ flips to Available (release); JS reads only after
    // observing Available (acquire), so the publication is safe lock-free.
    uint64_t localSteamId() const { return localSteamId_.load(std::memory_order_acquire); }
    uint32_t appId() const { return appId_.load(std::memory_order_acquire); }
    const std::string& personaName() const { return personaName_; }

    // --- Friends actions (M2). Thread-safe: each enqueues a command that the
    // service thread runs against the Steam API. No-ops when unavailable. ---
    void setRichPresence(const std::string& key, const std::string& value);
    void clearRichPresence();
    void activateOverlay(const std::string& dialog);
    void activateOverlayToUser(const std::string& dialog, uint64_t steamId);

    /// Request a friend's avatar (size 0 small/1 medium/2 large). The result
    /// (RGBA pixels, or empty if not loaded) is delivered to `subscriberId` as an
    /// AvatarData_ event tagged with `reqId`.
    void requestAvatar(uint32_t subscriberId, uint32_t reqId, uint64_t steamId, int size);

    // --- Lobby actions (M3). Async ops (create/join) deliver their result to
    // `subscriberId` as a LobbyCreated/LobbyEntered event tagged with `reqId`. ---
    void createLobby(uint32_t subscriberId, uint32_t reqId, int lobbyType, int maxMembers);
    void joinLobby(uint32_t subscriberId, uint32_t reqId, uint64_t lobbyId);
    void leaveLobby(uint64_t lobbyId);
    void setLobbyData(uint64_t lobbyId, const std::string& key, const std::string& value);
    void setLobbyMemberData(uint64_t lobbyId, const std::string& key, const std::string& value);
    void setLobbyJoinable(uint64_t lobbyId, bool joinable);
    void setLobbyType(uint64_t lobbyId, int lobbyType);
    void setLobbyMemberLimit(uint64_t lobbyId, int maxMembers);

    /// Allocate a subscriber and register it. Non-owning pointer; release with
    /// destroySubscriber(). Safe to call from any thread.
    SteamSubscriber* createSubscriber();

    /// Release a subscriber. After this the pointer is invalid; the service
    /// thread tears it down asynchronously. Do not poll() it afterwards.
    void destroySubscriber(SteamSubscriber* sub);

private:
    friend class SteamSubscriber;

    void threadMain();
    void postCommand(SteamCommand* cmd);
    void postEventTo(uint32_t subscriberId, SteamEvent* ev);
    void handleCommand(SteamCommand& cmd);        // service thread only
    void buildAndEmitFriends();                   // service thread only
    void emitFriendsTo(uint32_t subscriberId);    // service thread only
    void buildAndEmitLobby(uint64_t lobbyId);     // service thread only
    // Mark a lobby entered (idempotent): on the first entry it snapshots the
    // lobby and broadcasts onlobbyentered; a duplicate physical enter only
    // resolves a still-pending promise (reqId) without re-broadcasting.
    void enterLobby(uint32_t requesterSub, uint32_t reqId,
                    uint64_t lobbyId, int response);     // service thread only
    void emitLobbyEntered(uint32_t requesterSub, uint32_t reqId,
                          uint64_t lobbyId, int response,
                          bool broadcast);               // service thread only
    void dispatchCallback(const struct CallbackMsg_t& msg); // service thread only
    void emitToAll(SteamEvent::Type type, uint64_t u64,
                   const std::string& str = {});  // service thread only

    // --- Service-thread-only state ---
    std::unordered_map<uint32_t, SteamSubscriber*> subscribers_; // id → subscriber

    // Steam API + resolved interface pointers. Written once in threadMain()
    // before the loop, then read only on the service thread — no sharing.
    SteamFlatApi api_;
    void* iUser_        = nullptr;
    void* iFriends_     = nullptr;
    void* iUtils_       = nullptr;
    void* iMatchmaking_ = nullptr;
    std::vector<FriendInfo> lastFriends_; // authoritative snapshot for diffing
    HSteamPipe pipe_ = 0;                 // for ManualDispatch_GetAPICallResult

    // In-flight async SteamAPICall_t handles → the request that issued them, so
    // a SteamAPICallCompleted_t can be routed back to the right promise. Service
    // thread only.
    struct PendingCall { uint32_t subscriberId; uint32_t reqId; int expectedCb; };
    std::unordered_map<uint64_t, PendingCall> pendingCalls_;
    std::unordered_set<uint64_t> joinedLobbies_; // lobbies I'm currently in (dedup)

    // --- Lock-free MPSC command ingress (intrusive atomic stack) ---
    std::atomic<SteamCommand*> cmdHead_{nullptr};

    // --- Published state (atomics + publish-before-Available strings) ---
    std::atomic<Status>   status_{Status::Initializing};
    std::atomic<uint64_t> localSteamId_{0};
    std::atomic<uint32_t> appId_{0};
    std::string           personaName_; // see note above — no concurrent access after publish

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> nextSubscriberId_{1};

    static SteamService* s_instance;
};

} // namespace bro::steam
