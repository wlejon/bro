#include "steam/steam_flat.h"
#include "util/log.h"

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace bro::steam {

namespace {

#if defined(_WIN32)
const char* kLibNames[] = { "steam_api64.dll" };
void* dllOpen(const char* name) { return reinterpret_cast<void*>(LoadLibraryA(name)); }
void* dllSym(void* h, const char* sym) {
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), sym));
}
void dllClose(void* h) { FreeLibrary(reinterpret_cast<HMODULE>(h)); }
#elif defined(__APPLE__)
const char* kLibNames[] = { "libsteam_api.dylib", "./libsteam_api.dylib" };
void* dllOpen(const char* name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void* dllSym(void* h, const char* sym) { return dlsym(h, sym); }
void dllClose(void* h) { dlclose(h); }
#else
const char* kLibNames[] = { "libsteam_api.so", "./libsteam_api.so" };
void* dllOpen(const char* name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void* dllSym(void* h, const char* sym) { return dlsym(h, sym); }
void dllClose(void* h) { dlclose(h); }
#endif

template <typename Fn>
void resolve(void* h, const char* sym, Fn& out) {
    out = reinterpret_cast<Fn>(dllSym(h, sym));
}

} // namespace

bool loadSteamFlat(SteamFlatApi& api) {
    api = SteamFlatApi{};

    void* h = nullptr;
    for (const char* name : kLibNames) {
        h = dllOpen(name);
        if (h) break;
    }
    if (!h) {
        // Not an error — the redistributable simply isn't present (e.g. dev
        // build without Steam, or Steam not installed). The service reports
        // this as available=false with a clear reason.
        return false;
    }
    api.handle = h;

    resolve(h, "SteamAPI_Init",                          api.Init);
    resolve(h, "SteamAPI_InitFlat",                      api.InitFlat);
    resolve(h, "SteamAPI_Shutdown",                      api.Shutdown);
    resolve(h, "SteamAPI_RunCallbacks",                  api.RunCallbacks);
    resolve(h, "SteamAPI_GetHSteamUser",                 api.GetHSteamUser);
    resolve(h, "SteamAPI_GetHSteamPipe",                 api.GetHSteamPipe);
    resolve(h, "SteamInternal_FindOrCreateUserInterface", api.FindOrCreateUserInterface);

    resolve(h, "SteamAPI_ManualDispatch_Init",             api.ManualDispatch_Init);
    resolve(h, "SteamAPI_ManualDispatch_RunFrame",         api.ManualDispatch_RunFrame);
    resolve(h, "SteamAPI_ManualDispatch_GetNextCallback",  api.ManualDispatch_GetNextCallback);
    resolve(h, "SteamAPI_ManualDispatch_FreeLastCallback", api.ManualDispatch_FreeLastCallback);
    resolve(h, "SteamAPI_ManualDispatch_GetAPICallResult", api.ManualDispatch_GetAPICallResult);
    resolve(h, "SteamAPI_ISteamUser_GetSteamID",         api.User_GetSteamID);
    resolve(h, "SteamAPI_ISteamFriends_GetPersonaName",  api.Friends_GetPersonaName);
    resolve(h, "SteamAPI_ISteamUtils_GetAppID",          api.Utils_GetAppID);

    // Friends (M2) — optional; absence degrades the feature, not the lifecycle.
    resolve(h, "SteamAPI_ISteamFriends_GetFriendCount",            api.Friends_GetFriendCount);
    resolve(h, "SteamAPI_ISteamFriends_GetFriendByIndex",         api.Friends_GetFriendByIndex);
    resolve(h, "SteamAPI_ISteamFriends_GetFriendPersonaName",     api.Friends_GetFriendPersonaName);
    resolve(h, "SteamAPI_ISteamFriends_GetFriendPersonaState",    api.Friends_GetFriendPersonaState);
    resolve(h, "SteamAPI_ISteamFriends_GetFriendRelationship",    api.Friends_GetFriendRelationship);
    resolve(h, "SteamAPI_ISteamFriends_SetRichPresence",          api.Friends_SetRichPresence);
    resolve(h, "SteamAPI_ISteamFriends_ClearRichPresence",        api.Friends_ClearRichPresence);
    resolve(h, "SteamAPI_ISteamFriends_GetFriendRichPresence",    api.Friends_GetFriendRichPresence);
    resolve(h, "SteamAPI_ISteamFriends_ActivateGameOverlay",      api.Friends_ActivateGameOverlay);
    resolve(h, "SteamAPI_ISteamFriends_ActivateGameOverlayToUser", api.Friends_ActivateGameOverlayToUser);

    resolve(h, "SteamAPI_ISteamFriends_GetSmallFriendAvatar",     api.Friends_GetSmallFriendAvatar);
    resolve(h, "SteamAPI_ISteamFriends_GetMediumFriendAvatar",    api.Friends_GetMediumFriendAvatar);
    resolve(h, "SteamAPI_ISteamFriends_GetLargeFriendAvatar",     api.Friends_GetLargeFriendAvatar);
    resolve(h, "SteamAPI_ISteamUtils_GetImageSize",               api.Utils_GetImageSize);
    resolve(h, "SteamAPI_ISteamUtils_GetImageRGBA",               api.Utils_GetImageRGBA);

    // Matchmaking / lobbies (M3) — optional; absence degrades the feature.
    resolve(h, "SteamAPI_ISteamMatchmaking_CreateLobby",                      api.Matchmaking_CreateLobby);
    resolve(h, "SteamAPI_ISteamMatchmaking_JoinLobby",                        api.Matchmaking_JoinLobby);
    resolve(h, "SteamAPI_ISteamMatchmaking_LeaveLobby",                       api.Matchmaking_LeaveLobby);
    resolve(h, "SteamAPI_ISteamMatchmaking_SetLobbyData",                     api.Matchmaking_SetLobbyData);
    resolve(h, "SteamAPI_ISteamMatchmaking_GetLobbyData",                     api.Matchmaking_GetLobbyData);
    resolve(h, "SteamAPI_ISteamMatchmaking_GetLobbyDataCount",               api.Matchmaking_GetLobbyDataCount);
    resolve(h, "SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex",             api.Matchmaking_GetLobbyDataByIndex);
    resolve(h, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers",              api.Matchmaking_GetNumLobbyMembers);
    resolve(h, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex",           api.Matchmaking_GetLobbyMemberByIndex);
    resolve(h, "SteamAPI_ISteamMatchmaking_GetLobbyOwner",                    api.Matchmaking_GetLobbyOwner);
    resolve(h, "SteamAPI_ISteamMatchmaking_SetLobbyMemberData",              api.Matchmaking_SetLobbyMemberData);
    resolve(h, "SteamAPI_ISteamMatchmaking_GetLobbyMemberData",              api.Matchmaking_GetLobbyMemberData);
    resolve(h, "SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit",            api.Matchmaking_GetLobbyMemberLimit);
    resolve(h, "SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit",            api.Matchmaking_SetLobbyMemberLimit);
    resolve(h, "SteamAPI_ISteamMatchmaking_SetLobbyType",                     api.Matchmaking_SetLobbyType);
    resolve(h, "SteamAPI_ISteamMatchmaking_SetLobbyJoinable",                api.Matchmaking_SetLobbyJoinable);
    resolve(h, "SteamAPI_ISteamMatchmaking_InviteUserToLobby",              api.Matchmaking_InviteUserToLobby);
    resolve(h, "SteamAPI_ISteamMatchmaking_RequestLobbyList",                api.Matchmaking_RequestLobbyList);
    resolve(h, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex",                 api.Matchmaking_GetLobbyByIndex);
    resolve(h, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter", api.Matchmaking_AddRequestLobbyListStringFilter);
    resolve(h, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter", api.Matchmaking_AddRequestLobbyListNumericalFilter);
    resolve(h, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter", api.Matchmaking_AddRequestLobbyListResultCountFilter);

    // The init + pump symbols are the floor — without them the library is
    // unusable. (Either Init or InitFlat is enough.)
    if ((!api.Init && !api.InitFlat) || !api.Shutdown || !api.RunCallbacks ||
        !api.GetHSteamUser || !api.FindOrCreateUserInterface) {
        LOG_ERROR("[steam] loaded %s but core flat symbols are missing — "
                  "incompatible redistributable", kLibNames[0]);
        dllClose(h);
        api = SteamFlatApi{};
        return false;
    }
    return true;
}

void unloadSteamFlat(SteamFlatApi& api) {
    if (api.handle) dllClose(api.handle);
    api = SteamFlatApi{};
}

} // namespace bro::steam
