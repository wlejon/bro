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
// So the version MATCHING the shipped redistributable must come first. Our
// reference redistributable is now the OFFICIAL SDK 1.64 steam_api64.dll
// (sdk/redistributable_bin/win64), so these lists lead with 1.64's versions;
// 1.57's versions follow as fallbacks for an older bundled DLL. Between 1.57 and
// 1.64 only ISteamFriends bumped (017 -> 018); User/Utils/Matchmaking are
// unchanged.
namespace {
const char* kUserVersions[]    = { "SteamUser023", "SteamUser022", "SteamUser021", "SteamUser020" };
const char* kFriendsVersions[] = { "SteamFriends018", "SteamFriends017", "SteamFriends015" };
const char* kUtilsVersions[]   = { "SteamUtils010", "SteamUtils009" };
// ISteamMatchmaking has been version 009 since ~SDK 1.36 and is still 009 in the
// 1.64 reference redistributable. Lead with the matching version (see the vtable
// hazard note above); 008 is the only realistic older fallback.
const char* kMatchmakingVersions[] = { "SteamMatchMaking009", "SteamMatchMaking008" };

// k_EFriendFlagImmediate — "regular" friends (the people on your friends list).
constexpr int kFriendFlagImmediate = 0x04;

// Rebuild the friends snapshot at ~2 Hz (every 50 * 10 ms pump iterations).
constexpr uint64_t kFriendsRebuildEvery = 50;

// Steam callback ids (k_iSteamFriendsCallbacks == 300). Stable across SDKs.
constexpr int kCbPersonaStateChange          = 300 + 4;  // 304
constexpr int kCbGameOverlayActivated        = 300 + 31; // 331
constexpr int kCbGameRichPresenceJoinRequest = 300 + 37; // 337

// Matchmaking callback ids (k_iSteamMatchmakingCallbacks == 500) + the async
// call-result completion id (k_iSteamUtilsCallbacks + 3 == 703).
constexpr int kCbLobbyInvite        = 500 + 3;  // 503
constexpr int kCbLobbyEnter         = 500 + 4;  // 504 (also JoinLobby call result)
constexpr int kCbLobbyDataUpdate    = 500 + 5;  // 505
constexpr int kCbLobbyChatUpdate    = 500 + 6;  // 506
constexpr int kCbLobbyMatchList     = 500 + 10; // 510 (RequestLobbyList call result)
constexpr int kCbLobbyCreated       = 500 + 13; // 513 (CreateLobby call result)
constexpr int kCbGameLobbyJoinRequested = 500 + 33; // 533
constexpr int kCbSteamAPICallCompleted = 700 + 3; // 703

// EChatMemberStateChange bits that mean "this member is gone".
constexpr uint32_t kChatMemberLeftMask = 0x0002 /*Left*/ | 0x0004 /*Disconnected*/ |
                                         0x0008 /*Kicked*/ | 0x0010 /*Banned*/;
constexpr int kEResultOK = 1;

// EVoiceResult: 0 OK, 1 NotInitialized, 2 NotRecording, 3 NoData, 4 BufferTooSmall,
// 5 DataCorrupted, 6 Restricted.
constexpr int kEVoiceResultOK          = 0;
constexpr int kEVoiceResultBufferSmall = 4;
constexpr uint32_t kVoiceFallbackRate  = 24000; // if GetVoiceOptimalSampleRate absent
constexpr uint32_t kVoiceDecodeChunk   = 22050 * 2 * 4; // generous PCM scratch (≈4 s @ 22 kHz int16)


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
            case SteamEvent::OverlayActivated:
                if (onOverlay) onOverlay(ev->u64 != 0);
                break;
            case SteamEvent::JoinRequested:
                if (onJoinRequest) onJoinRequest(ev->u64, ev->str);
                break;
            case SteamEvent::AvatarData_:
                if (ev->avatar) {
                    if (onAvatar)
                        onAvatar(ev->avatar->reqId, ev->avatar->w, ev->avatar->h,
                                 ev->avatar->rgba.data(), ev->avatar->rgba.size());
                    delete ev->avatar; // ownership transferred via the event
                }
                break;
            case SteamEvent::LobbyCreated:
                if (onLobbyCreated) onLobbyCreated(ev->reqId, ev->u64, ev->i32 != 0);
                break;
            case SteamEvent::LobbyEntered:
                if (onLobbyEntered) onLobbyEntered(ev->reqId, ev->u64, ev->i32, ev->u64b == 0);
                break;
            case SteamEvent::LobbyUpdated:
                if (ev->lobby) {
                    if (onLobbyUpdated) onLobbyUpdated(*ev->lobby);
                    delete ev->lobby; // ownership transferred via the event
                }
                break;
            case SteamEvent::LobbyLeft:
                if (onLobbyLeft) onLobbyLeft(ev->u64);
                break;
            case SteamEvent::LobbyList:
                if (ev->lobbyList) {
                    if (onLobbyList) onLobbyList(ev->reqId, *ev->lobbyList);
                    delete ev->lobbyList; // ownership transferred via the event
                }
                break;
            case SteamEvent::LobbyInvite:
                if (onLobbyInvite) onLobbyInvite(ev->u64, ev->u64b);
                break;
            case SteamEvent::LobbyJoinRequested:
                if (onLobbyJoinRequested) onLobbyJoinRequested(ev->u64, ev->u64b);
                break;
            case SteamEvent::VoiceCaptured:
                if (ev->blob) {
                    if (onVoiceCaptured) onVoiceCaptured(ev->blob->data(), ev->blob->size());
                    delete ev->blob; // ownership transferred via the event
                }
                break;
            case SteamEvent::VoiceDecoded:
                if (ev->blob) {
                    if (onVoiceDecoded)
                        onVoiceDecoded(ev->reqId, ev->i32, ev->blob->data(), ev->blob->size());
                    delete ev->blob; // ownership transferred via the event
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
    running_ = true;
    thread_ = std::thread(&SteamService::threadMain, this);
}

SteamService::~SteamService() {
    {
        std::lock_guard<std::mutex> lk(m_);
        running_ = false;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();

    // Free any command nodes that arrived after the thread drained/exited. Do
    // NOT delete subscriberPtr here — the thread's shutdown already freed the
    // subscribers it owned (double-free risk otherwise).
    {
        std::lock_guard<std::mutex> lk(m_);
        cmdQueue_.clear();
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

void SteamService::activateInviteDialog(uint64_t lobbyId) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::ActivateInviteDialog;
    c->u64 = lobbyId;
    postCommand(c);
}

void SteamService::requestAvatar(uint32_t subscriberId, uint32_t reqId, uint64_t steamId, int size) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::RequestAvatar;
    c->subscriberId = subscriberId;
    c->reqId = reqId;
    c->u64 = steamId;
    c->i32 = size;
    postCommand(c);
}

void SteamService::postCommand(SteamCommand* cmd) {
    {
        std::lock_guard<std::mutex> lk(m_);
        cmdQueue_.emplace_back(cmd);
    }
    cv_.notify_one();
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
        case SteamCommand::ActivateInviteDialog:
            if (iFriends_ && api_.Friends_ActivateGameOverlayInviteDialog)
                api_.Friends_ActivateGameOverlayInviteDialog(iFriends_, cmd.u64);
            break;
        case SteamCommand::RequestAvatar: {
            // Calling Get*FriendAvatar nudges Steam to download the image if it's
            // not cached. If it's not ready yet, pend the request and deliver when
            // the avatar-loaded PersonaStateChange arrives — so the app's
            // getAvatar() promise resolves once, when the pixels are actually
            // available, instead of resolving null and forcing it to poll.
            if (!emitAvatarIfReady(cmd.subscriberId, cmd.reqId, cmd.u64, cmd.i32))
                pendingAvatars_[cmd.u64].push_back({ cmd.subscriberId, cmd.reqId, cmd.i32 });
            break;
        }
        case SteamCommand::CreateLobby: {
            uint64_t call = 0;
            if (iMatchmaking_ && api_.Matchmaking_CreateLobby)
                call = api_.Matchmaking_CreateLobby(iMatchmaking_, cmd.i32, cmd.i32b);
            if (call) {
                pendingCalls_[call] = { cmd.subscriberId, cmd.reqId, kCbLobbyCreated };
            } else {
                // Couldn't even issue the call — settle the promise as failure.
                auto* ev = new SteamEvent();
                ev->type = SteamEvent::LobbyCreated;
                ev->reqId = cmd.reqId;
                postEventTo(cmd.subscriberId, ev);
            }
            break;
        }
        case SteamCommand::JoinLobby: {
            uint64_t call = 0;
            if (iMatchmaking_ && api_.Matchmaking_JoinLobby)
                call = api_.Matchmaking_JoinLobby(iMatchmaking_, cmd.u64);
            if (call) {
                pendingCalls_[call] = { cmd.subscriberId, cmd.reqId, kCbLobbyEnter };
            } else {
                auto* ev = new SteamEvent();
                ev->type = SteamEvent::LobbyEntered;
                ev->reqId = cmd.reqId;
                ev->u64 = cmd.u64;
                ev->i32 = 0; // no response
                postEventTo(cmd.subscriberId, ev);
            }
            break;
        }
        case SteamCommand::LeaveLobby:
            if (iMatchmaking_ && api_.Matchmaking_LeaveLobby)
                api_.Matchmaking_LeaveLobby(iMatchmaking_, cmd.u64);
            joinedLobbies_.erase(cmd.u64);
            // Steam doesn't deliver a self-leave callback, so notify here.
            emitToAll(SteamEvent::LobbyLeft, cmd.u64);
            break;
        case SteamCommand::SetLobbyData:
            if (iMatchmaking_ && api_.Matchmaking_SetLobbyData)
                api_.Matchmaking_SetLobbyData(iMatchmaking_, cmd.u64, cmd.strA.c_str(), cmd.strB.c_str());
            break;
        case SteamCommand::SetLobbyMemberData:
            if (iMatchmaking_ && api_.Matchmaking_SetLobbyMemberData)
                api_.Matchmaking_SetLobbyMemberData(iMatchmaking_, cmd.u64, cmd.strA.c_str(), cmd.strB.c_str());
            break;
        case SteamCommand::SetLobbyJoinable:
            if (iMatchmaking_ && api_.Matchmaking_SetLobbyJoinable)
                api_.Matchmaking_SetLobbyJoinable(iMatchmaking_, cmd.u64, cmd.i32 != 0);
            break;
        case SteamCommand::SetLobbyType:
            if (iMatchmaking_ && api_.Matchmaking_SetLobbyType)
                api_.Matchmaking_SetLobbyType(iMatchmaking_, cmd.u64, cmd.i32);
            break;
        case SteamCommand::SetLobbyMemberLimit:
            if (iMatchmaking_ && api_.Matchmaking_SetLobbyMemberLimit)
                api_.Matchmaking_SetLobbyMemberLimit(iMatchmaking_, cmd.u64, cmd.i32);
            break;
        case SteamCommand::RequestLobbyList: {
            uint64_t call = 0;
            if (iMatchmaking_ && api_.Matchmaking_RequestLobbyList) {
                // Filters are interface state — apply them right before the call.
                for (const auto& f : cmd.filters) {
                    switch (f.kind) {
                        case LobbyListFilter::String:
                            if (api_.Matchmaking_AddRequestLobbyListStringFilter)
                                api_.Matchmaking_AddRequestLobbyListStringFilter(
                                    iMatchmaking_, f.key.c_str(), f.sval.c_str(), f.comparison);
                            break;
                        case LobbyListFilter::Numeric:
                            if (api_.Matchmaking_AddRequestLobbyListNumericalFilter)
                                api_.Matchmaking_AddRequestLobbyListNumericalFilter(
                                    iMatchmaking_, f.key.c_str(), f.ival, f.comparison);
                            break;
                        case LobbyListFilter::ResultCount:
                            if (api_.Matchmaking_AddRequestLobbyListResultCountFilter)
                                api_.Matchmaking_AddRequestLobbyListResultCountFilter(iMatchmaking_, f.ival);
                            break;
                        case LobbyListFilter::Distance:
                            if (api_.Matchmaking_AddRequestLobbyListDistanceFilter)
                                api_.Matchmaking_AddRequestLobbyListDistanceFilter(iMatchmaking_, f.ival);
                            break;
                    }
                }
                call = api_.Matchmaking_RequestLobbyList(iMatchmaking_);
            }
            if (call) {
                pendingCalls_[call] = { cmd.subscriberId, cmd.reqId, kCbLobbyMatchList };
            } else {
                auto* ev = new SteamEvent();
                ev->type = SteamEvent::LobbyList;
                ev->reqId = cmd.reqId;
                ev->lobbyList = new std::vector<LobbyState>(); // empty result
                postEventTo(cmd.subscriberId, ev);
            }
            break;
        }
        case SteamCommand::InviteToLobby:
            if (iMatchmaking_ && api_.Matchmaking_InviteUserToLobby)
                api_.Matchmaking_InviteUserToLobby(iMatchmaking_, cmd.u64, cmd.u64b);
            break;
        case SteamCommand::StartVoice:
            if (iUser_ && api_.User_StartVoiceRecording) {
                api_.User_StartVoiceRecording(iUser_);
                voiceRecording_.store(true, std::memory_order_release);
            }
            break;
        case SteamCommand::StopVoice:
            if (iUser_ && api_.User_StopVoiceRecording)
                api_.User_StopVoiceRecording(iUser_);
            voiceRecording_.store(false, std::memory_order_release);
            break;
        case SteamCommand::DecodeVoice: {
            auto* pcm = new std::vector<uint8_t>();
            uint32_t rate = cmd.i32 > 0 ? static_cast<uint32_t>(cmd.i32)
                                        : voiceSampleRate_.load(std::memory_order_relaxed);
            if (rate == 0) rate = kVoiceFallbackRate;
            if (iUser_ && api_.User_DecompressVoice && !cmd.bytes.empty()) {
                // Decode into a scratch buffer; grow + retry once on BufferTooSmall.
                uint32_t cap = kVoiceDecodeChunk;
                for (int attempt = 0; attempt < 2; ++attempt) {
                    pcm->resize(cap);
                    uint32_t written = 0;
                    int vr = api_.User_DecompressVoice(iUser_, cmd.bytes.data(),
                                                       static_cast<uint32_t>(cmd.bytes.size()),
                                                       pcm->data(), cap, &written, rate);
                    if (vr == kEVoiceResultOK) { pcm->resize(written); break; }
                    if (vr == kEVoiceResultBufferSmall) { cap *= 2; continue; }
                    pcm->clear(); break; // NoData / corrupted / restricted
                }
            }
            auto* ev = new SteamEvent();
            ev->type = SteamEvent::VoiceDecoded;
            ev->reqId = cmd.reqId;
            ev->i32 = static_cast<int>(rate);
            ev->blob = pcm;
            postEventTo(cmd.subscriberId, ev);
            break;
        }
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

bool SteamService::emitAvatarIfReady(uint32_t subscriberId, uint32_t reqId,
                                     uint64_t steamId, int size) {
    int handle = 0;
    if (iFriends_) {
        if (size <= 0 && api_.Friends_GetSmallFriendAvatar)
            handle = api_.Friends_GetSmallFriendAvatar(iFriends_, steamId);
        else if (size == 1 && api_.Friends_GetMediumFriendAvatar)
            handle = api_.Friends_GetMediumFriendAvatar(iFriends_, steamId);
        else if (api_.Friends_GetLargeFriendAvatar)
            handle = api_.Friends_GetLargeFriendAvatar(iFriends_, steamId);
    }
    // -1 == Steam is still downloading the image. Not resolvable yet — tell the
    // caller to pend and retry when the avatar-loaded PersonaStateChange fires.
    if (handle < 0) return false;

    auto* ad = new AvatarData();
    ad->reqId = reqId;
    // handle > 0: a real image handle. Read its pixels; if the bytes aren't
    // materialized yet (GetImageSize/RGBA fails transiently), treat as not-ready
    // and retry rather than resolving a premature null.
    if (handle > 0) {
        if (!(iUtils_ && api_.Utils_GetImageSize && api_.Utils_GetImageRGBA)) {
            delete ad; return false; // utils missing — can't read; keep pending
        }
        uint32_t w = 0, h = 0;
        if (!(api_.Utils_GetImageSize(iUtils_, handle, &w, &h) && w > 0 && h > 0)) {
            delete ad; return false; // size not ready yet
        }
        ad->rgba.resize(static_cast<size_t>(w) * h * 4);
        if (!api_.Utils_GetImageRGBA(iUtils_, handle, ad->rgba.data(),
                                     static_cast<int>(ad->rgba.size()))) {
            delete ad; return false; // pixels not ready yet
        }
        ad->w = static_cast<int>(w);
        ad->h = static_cast<int>(h);
    }
    // handle == 0: the user has no avatar set — deliver an empty AvatarData so the
    // app's promise resolves null (a settled answer, not a stuck request).

    auto* ev = new SteamEvent();
    ev->type = SteamEvent::AvatarData_;
    ev->avatar = ad;
    postEventTo(subscriberId, ev);
    return true;
}

void SteamService::retryPendingAvatars(uint64_t steamId) {
    auto it = pendingAvatars_.find(steamId);
    if (it == pendingAvatars_.end()) return;
    std::vector<PendingAvatar> stillLoading;
    for (const auto& r : it->second) {
        if (!emitAvatarIfReady(r.subscriberId, r.reqId, steamId, r.size))
            stillLoading.push_back(r);
    }
    if (stillLoading.empty()) pendingAvatars_.erase(it);
    else it->second = std::move(stillLoading);
}

void SteamService::emitToAll(SteamEvent::Type type, uint64_t u64, const std::string& str) {
    for (auto& [sid, sub] : subscribers_) {
        auto* ev = new SteamEvent();
        ev->type = type;
        ev->u64 = u64;
        ev->str = str;
        postEventTo(sid, ev);
    }
}


// ---------------------------------------------------------------------------
// Service thread main — loads the Steam redistributable, owns SteamAPI init,
// the RunCallbacks pump, and every callback handler. ALL SteamAPI calls happen
// on this thread.
//
// The command loop runs whether or not Steam is available, so subscriber
// register/unregister is always handled correctly — when Steam is down the loop
// simply skips the pump.
// ---------------------------------------------------------------------------
void SteamService::threadMain() {
    bool steamUp = false;
    bool manualDispatch = false;
    HSteamPipe pipe = 0;

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
            iMatchmaking_ = findInterface(api_, hUser, kMatchmakingVersions,
                                      sizeof(kMatchmakingVersions)/sizeof(*kMatchmakingVersions));

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
            if (iUser_ && api_.User_GetVoiceOptimalSampleRate)
                voiceSampleRate_.store(api_.User_GetVoiceOptimalSampleRate(iUser_),
                                       std::memory_order_relaxed);

            // Switch to ManualDispatch so we can pull callbacks ourselves on
            // this thread (PersonaStateChange, overlay, join requests, lobby
            // events). Falls back to RunCallbacks if the symbols aren't
            // present (older redistributable).
            if (api_.ManualDispatch_Init && api_.GetHSteamPipe &&
                api_.ManualDispatch_RunFrame && api_.ManualDispatch_GetNextCallback &&
                api_.ManualDispatch_FreeLastCallback) {
                api_.ManualDispatch_Init();
                pipe = api_.GetHSteamPipe();
                pipe_ = pipe; // dispatchCallback uses it for GetAPICallResult
                manualDispatch = (pipe != 0);
            }

            status_.store(Status::Available, std::memory_order_release);
            steamUp = true;
            LOG_INFO("[steam] SteamService started (steamId=%llu persona='%s' appId=%u dispatch=%s)",
                     static_cast<unsigned long long>(localSteamId_.load(std::memory_order_relaxed)),
                     personaName_.c_str(),
                     appId_.load(std::memory_order_relaxed),
                     manualDispatch ? "manual" : "auto");
        }
    }

    uint64_t iter = 0;
    std::deque<std::unique_ptr<SteamCommand>> batch;
    while (true) {
        // 1. Take pending commands (FIFO) and process them outside the lock;
        //    pace the loop. With Steam up we tick at 10ms so the callback pump
        //    stays serviced; with Steam absent/failed we block until a command
        //    arrives — register/unregister still works, with zero idle wakeups.
        {
            std::unique_lock<std::mutex> lk(m_);
            if (running_ && cmdQueue_.empty()) {
                if (steamUp) {
                    cv_.wait_for(lk, std::chrono::milliseconds(10), [this] {
                        return !cmdQueue_.empty() || !running_;
                    });
                } else {
                    cv_.wait(lk, [this] {
                        return !cmdQueue_.empty() || !running_;
                    });
                }
            }
            if (!running_ && cmdQueue_.empty()) break;
            batch.swap(cmdQueue_);
        }
        for (auto& cmd : batch) handleCommand(*cmd);
        batch.clear();

        if (steamUp) {
            // 2. Pump Steam callbacks. ManualDispatch lets us translate each
            //    callback into our own events on this thread; otherwise the
            //    classic auto path still keeps the client serviced.
            if (manualDispatch) {
                api_.ManualDispatch_RunFrame(pipe);
                CallbackMsg_t msg;
                while (api_.ManualDispatch_GetNextCallback(pipe, &msg)) {
                    dispatchCallback(msg);
                    api_.ManualDispatch_FreeLastCallback(pipe);
                }
            } else {
                api_.RunCallbacks();
            }

            ++iter;

            // 3. Voice capture (every pump while recording — Steam's VAD keeps
            //    this quiet when no one is speaking).
            if (voiceRecording_.load(std::memory_order_relaxed)) pumpVoiceCapture();

            // 4. Friends snapshot (~2 Hz). Emits only on a diff, so a steady
            //    list produces no events.
            if (iter % kFriendsRebuildEvery == 0) buildAndEmitFriends();

            // 5. Heartbeat: ~1 Hz pulse to every subscriber so callers can
            //    confirm the pump is alive.
            if (iter % 100 == 0) {
                for (auto& [sid, sub] : subscribers_) {
                    auto* ev = new SteamEvent();
                    ev->type = SteamEvent::Pulse;
                    ev->u64 = iter / 100;
                    postEventTo(sid, ev);
                }
            }
        }
    }
    // Late commands (e.g. Unregister posted around the running_=false
    // observation) were drained by the loop's exit condition above, so
    // subscribers don't leak.

    for (auto& [sid, sub] : subscribers_) delete sub;
    subscribers_.clear();

    if (steamUp) {
        api_.Shutdown();
        unloadSteamFlat(api_);
    }
    LOG_INFO("[steam] SteamService stopped");
}

} // namespace bro::steam
