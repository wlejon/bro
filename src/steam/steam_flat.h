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
    void* (*FindOrCreateUserInterface)(HSteamUser, const char* version) = nullptr;

    // --- method wrappers (interface pointer passed as the first "self" arg) ---
    uint64_t    (*User_GetSteamID)(void* iSteamUser) = nullptr;
    const char* (*Friends_GetPersonaName)(void* iSteamFriends) = nullptr;
    uint32_t    (*Utils_GetAppID)(void* iSteamUtils) = nullptr;

    bool loaded() const { return handle != nullptr; }
};

/// Load the Steam redistributable and resolve the flat entry points above.
/// Returns true if the library loaded and the lifecycle symbols resolved.
/// On failure, api.handle is null and api is left safe to ignore.
bool loadSteamFlat(SteamFlatApi& api);

/// Unload the library (if loaded) and clear the struct.
void unloadSteamFlat(SteamFlatApi& api);

} // namespace bro::steam
