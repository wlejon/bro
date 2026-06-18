#pragma once

#include <cstdint>

// Minimal, self-authored declarations of the Steamworks *flat C API* — the
// stable C entry points exported by the `steam_api64.dll` / `libsteam_api.{so,
// dylib}` redistributable. We resolve them at runtime (LoadLibrary/dlopen +
// GetProcAddress/dlsym), so bro depends on NO Steamworks SDK headers or import
// libs at build time. The proprietary SDK never touches the repo; only the
// redistributable ships with the product (and is loaded if present).
//
// This is the same technique every other-language Steam binding uses
// (steamworks.js, steamworks-rs): the flat API is purpose-built for FFI. The
// flat *method* wrappers (SteamAPI_ISteamUser_GetSteamID, ...) have stable,
// unversioned names; only the interface *accessor* version strings
// ("SteamUser023") track SDK releases, so loadSteamFlat() probes a descending
// list of known versions.
//
// Scope today: lifecycle + identity (M1). Friends / matchmaking / voice / UGC
// add more flat-function pointers to SteamFlatApi as those layers come online.

namespace bro::steam {

using HSteamUser = int32_t;
using HSteamPipe = int32_t;

// One queued Steam callback, delivered by the ManualDispatch API. This is the
// flat-API way to receive callbacks (PersonaStateChange, overlay, join requests,
// lobby events, …) without the C++ CCallback/vtable machinery. Layout must match
// Steam's CallbackMsg_t exactly: int32, int32, ptr (8-aligned), int32 → 24 bytes.
struct CallbackMsg_t {
    HSteamUser m_hSteamUser;
    int32_t    m_iCallback;   // callback id (e.g. PersonaStateChange_t == 304)
    uint8_t*   m_pubParam;    // pointer to the callback's param struct
    int32_t    m_cubParam;    // size of that struct
};

// Resolved flat-API entry points. A null pointer means the symbol was absent in
// the loaded library (older/newer redistributable) — callers must null-check.
struct SteamFlatApi {
    void* handle = nullptr;            // dlopen/LoadLibrary handle

    // --- lifecycle ---
    // Classic export; absent in some newer redistributables (see InitFlat).
    bool (*Init)() = nullptr;
    // Newer export: writes an error string, returns ESteamAPIInitResult (0 == OK).
    int  (*InitFlat)(char* outErrMsg) = nullptr;
    void (*Shutdown)() = nullptr;
    void (*RunCallbacks)() = nullptr;
    HSteamUser (*GetHSteamUser)() = nullptr;
    HSteamPipe (*GetHSteamPipe)() = nullptr;
    void* (*FindOrCreateUserInterface)(HSteamUser, const char* version) = nullptr;

    // --- ManualDispatch: pull queued callbacks ourselves (replaces RunCallbacks
    // when present). Init() switches the client into manual-dispatch mode; each
    // pump calls RunFrame then drains GetNextCallback/FreeLastCallback. ---
    void (*ManualDispatch_Init)() = nullptr;
    void (*ManualDispatch_RunFrame)(HSteamPipe) = nullptr;
    bool (*ManualDispatch_GetNextCallback)(HSteamPipe, CallbackMsg_t*) = nullptr;
    void (*ManualDispatch_FreeLastCallback)(HSteamPipe) = nullptr;

    // --- method wrappers (interface pointer passed as the first "self" arg) ---
    uint64_t    (*User_GetSteamID)(void* iSteamUser) = nullptr;
    const char* (*Friends_GetPersonaName)(void* iSteamFriends) = nullptr;
    uint32_t    (*Utils_GetAppID)(void* iSteamUtils) = nullptr;

    // --- friends (M2): list, personas, rich presence, overlay ---
    // CSteamID is passed by value as a uint64 in the flat ABI.
    int         (*Friends_GetFriendCount)(void* iSteamFriends, int iFriendFlags) = nullptr;
    uint64_t    (*Friends_GetFriendByIndex)(void* iSteamFriends, int iFriend, int iFriendFlags) = nullptr;
    const char* (*Friends_GetFriendPersonaName)(void* iSteamFriends, uint64_t steamIDFriend) = nullptr;
    int         (*Friends_GetFriendPersonaState)(void* iSteamFriends, uint64_t steamIDFriend) = nullptr;
    int         (*Friends_GetFriendRelationship)(void* iSteamFriends, uint64_t steamIDFriend) = nullptr;
    bool        (*Friends_SetRichPresence)(void* iSteamFriends, const char* key, const char* value) = nullptr;
    void        (*Friends_ClearRichPresence)(void* iSteamFriends) = nullptr;
    const char* (*Friends_GetFriendRichPresence)(void* iSteamFriends, uint64_t steamIDFriend, const char* key) = nullptr;
    void        (*Friends_ActivateGameOverlay)(void* iSteamFriends, const char* dialog) = nullptr;
    void        (*Friends_ActivateGameOverlayToUser)(void* iSteamFriends, const char* dialog, uint64_t steamID) = nullptr;

    // Avatars: friends return an int image handle (0 none, -1 loading); the
    // pixels are fetched from ISteamUtils by that handle as RGBA.
    int  (*Friends_GetSmallFriendAvatar)(void* iSteamFriends, uint64_t steamIDFriend) = nullptr;
    int  (*Friends_GetMediumFriendAvatar)(void* iSteamFriends, uint64_t steamIDFriend) = nullptr;
    int  (*Friends_GetLargeFriendAvatar)(void* iSteamFriends, uint64_t steamIDFriend) = nullptr;
    bool (*Utils_GetImageSize)(void* iSteamUtils, int iImage, uint32_t* w, uint32_t* h) = nullptr;
    bool (*Utils_GetImageRGBA)(void* iSteamUtils, int iImage, uint8_t* dest, int destBufferSize) = nullptr;

    bool loaded() const { return handle != nullptr; }
};

/// Load the Steam redistributable and resolve the flat entry points above.
/// Returns true if the library loaded and the lifecycle symbols resolved.
/// On failure, api.handle is null and api is left safe to ignore.
bool loadSteamFlat(SteamFlatApi& api);

/// Unload the library (if loaded) and clear the struct.
void unloadSteamFlat(SteamFlatApi& api);

} // namespace bro::steam
