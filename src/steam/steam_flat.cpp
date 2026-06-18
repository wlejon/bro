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
    resolve(h, "SteamInternal_FindOrCreateUserInterface", api.FindOrCreateUserInterface);
    resolve(h, "SteamAPI_ISteamUser_GetSteamID",         api.User_GetSteamID);
    resolve(h, "SteamAPI_ISteamFriends_GetPersonaName",  api.Friends_GetPersonaName);
    resolve(h, "SteamAPI_ISteamUtils_GetAppID",          api.Utils_GetAppID);

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
