// =============================================================================
// bro_steam_c_abi.h — Pure C-ABI declarations for bro.steam
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_STEAM_C_ABI_H
#define BRO_STEAM_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.steam ---
bool bro_steam_get_available(void);
const char* bro_steam_get_reason(void);
uint32_t bro_steam_get_appId(void);
const char* bro_steam_get_steamId(void);
const char* bro_steam_get_personaName(void);
bool bro_steam_get_isLoggedOn(void);
bool bro_steam_get_isVoiceRecording(void);
int32_t bro_steam_get_voiceSampleRate(void);
void* bro_steam_get_onpulse(void);
void bro_steam_set_onpulse(void* val);
void* bro_steam_get_onfriends(void);
void bro_steam_set_onfriends(void* val);
void* bro_steam_get_onoverlay(void);
void bro_steam_set_onoverlay(void* val);
void* bro_steam_get_onjoinrequest(void);
void bro_steam_set_onjoinrequest(void* val);
void* bro_steam_get_onlobbyentered(void);
void bro_steam_set_onlobbyentered(void* val);
void* bro_steam_get_onlobbyupdated(void);
void bro_steam_set_onlobbyupdated(void* val);
void* bro_steam_get_onlobbyleft(void);
void bro_steam_set_onlobbyleft(void* val);
void* bro_steam_get_onlobbyinvite(void);
void bro_steam_set_onlobbyinvite(void* val);
void* bro_steam_get_onlobbyjoinrequest(void);
void bro_steam_set_onlobbyjoinrequest(void* val);
void* bro_steam_get_onvoicecaptured(void);
void bro_steam_set_onvoicecaptured(void* val);
bool bro_steam_getAchievement(const char* name);
bool bro_steam_setAchievement(const char* name);
bool bro_steam_clearAchievement(const char* name);
double bro_steam_getStat(const char* name);
bool bro_steam_setStat(const char* name, double value);
bool bro_steam_storeStats(void);
void bro_steam_activateOverlay(const char* dialog);
void bro_steam_activateOverlayToWebPage(const char* url);
void* bro_steam_getFriends(void);
void* bro_steam_getAvatar(const char* steamId, void* size);
bool bro_steam_setRichPresence(const char* key, const char* value);
void bro_steam_clearRichPresence(void);
void* bro_steam_createLobby(const char* type, int32_t maxMembers);
void* bro_steam_joinLobby(const char* lobbyId);
void bro_steam_leaveLobby(const char* lobbyId);
bool bro_steam_setLobbyData(const char* lobbyId, const char* key, const char* value);
void* bro_steam_getLobbyMembers(const char* lobbyId);
const char* bro_steam_getLobbyOwner(const char* lobbyId);
const char* bro_steam_getLobbyData(const char* lobbyId, const char* key);
void bro_steam_requestLobbyList(void* filter);
bool bro_steam_inviteUserToLobby(const char* lobbyId, const char* steamId);
void bro_steam_startVoiceRecording(void);
void bro_steam_stopVoiceRecording(void);
void* bro_steam_decodeVoice(void* data, int32_t sampleRate);

#ifdef __cplusplus
}
#endif

#endif // BRO_STEAM_C_ABI_H
