// =============================================================================
// bro_steam_c_abi.cpp — C++ forwarding implementations for bro.steam
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_steam_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"

extern "C" {

static void* s_steam_onpulse = nullptr;
static void* s_steam_onfriends = nullptr;
static void* s_steam_onoverlay = nullptr;
static void* s_steam_onjoinrequest = nullptr;
static void* s_steam_onlobbyentered = nullptr;
static void* s_steam_onlobbyupdated = nullptr;
static void* s_steam_onlobbyleft = nullptr;
static void* s_steam_onlobbyinvite = nullptr;
static void* s_steam_onlobbyjoinrequest = nullptr;
static void* s_steam_onvoicecaptured = nullptr;

bool bro_steam_get_available(void) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->getAvailable) return b->getAvailable();
    return false;
}

const char* bro_steam_get_reason(void) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->getReason) return b->getReason();
    return "Steam not available";
}

uint32_t bro_steam_get_appId(void) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->getAppId) return b->getAppId();
    return 0;
}

const char* bro_steam_get_steamId(void) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->getSteamId) return b->getSteamId();
    return "0";
}

const char* bro_steam_get_personaName(void) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->getPersonaName) return b->getPersonaName();
    return "";
}

bool bro_steam_get_isLoggedOn(void) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->getIsLoggedOn) return b->getIsLoggedOn();
    return false;
}

bool bro_steam_get_isVoiceRecording(void) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->getIsVoiceRecording) return b->getIsVoiceRecording();
    return false;
}

int32_t bro_steam_get_voiceSampleRate(void) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->getVoiceSampleRate) return b->getVoiceSampleRate();
    return 0;
}

void* bro_steam_get_onpulse(void) { return s_steam_onpulse; }
void bro_steam_set_onpulse(void* val) { s_steam_onpulse = val; }

void* bro_steam_get_onfriends(void) { return s_steam_onfriends; }
void bro_steam_set_onfriends(void* val) { s_steam_onfriends = val; }

void* bro_steam_get_onoverlay(void) { return s_steam_onoverlay; }
void bro_steam_set_onoverlay(void* val) { s_steam_onoverlay = val; }

void* bro_steam_get_onjoinrequest(void) { return s_steam_onjoinrequest; }
void bro_steam_set_onjoinrequest(void* val) { s_steam_onjoinrequest = val; }

void* bro_steam_get_onlobbyentered(void) { return s_steam_onlobbyentered; }
void bro_steam_set_onlobbyentered(void* val) { s_steam_onlobbyentered = val; }

void* bro_steam_get_onlobbyupdated(void) { return s_steam_onlobbyupdated; }
void bro_steam_set_onlobbyupdated(void* val) { s_steam_onlobbyupdated = val; }

void* bro_steam_get_onlobbyleft(void) { return s_steam_onlobbyleft; }
void bro_steam_set_onlobbyleft(void* val) { s_steam_onlobbyleft = val; }

void* bro_steam_get_onlobbyinvite(void) { return s_steam_onlobbyinvite; }
void bro_steam_set_onlobbyinvite(void* val) { s_steam_onlobbyinvite = val; }

void* bro_steam_get_onlobbyjoinrequest(void) { return s_steam_onlobbyjoinrequest; }
void bro_steam_set_onlobbyjoinrequest(void* val) { s_steam_onlobbyjoinrequest = val; }

void* bro_steam_get_onvoicecaptured(void) { return s_steam_onvoicecaptured; }
void bro_steam_set_onvoicecaptured(void* val) { s_steam_onvoicecaptured = val; }

bool bro_steam_getAchievement(const char* name) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->getAchievement) return b->getAchievement(name);
    return false;
}

bool bro_steam_setAchievement(const char* name) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->setAchievement) return b->setAchievement(name);
    return false;
}

bool bro_steam_clearAchievement(const char* name) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->clearAchievement) return b->clearAchievement(name);
    return false;
}

double bro_steam_getStat(const char* name) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->getStat) return b->getStat(name);
    return 0.0;
}

bool bro_steam_setStat(const char* name, double value) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->setStat) return b->setStat(name, value);
    return false;
}

bool bro_steam_storeStats(void) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->storeStats) return b->storeStats();
    return false;
}

void bro_steam_activateOverlay(const char* dialog) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->activateOverlay) b->activateOverlay(dialog);
}

void bro_steam_activateOverlayToWebPage(const char* url) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->activateOverlayToWebPage) b->activateOverlayToWebPage(url);
}

void* bro_steam_getFriends(void) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->getFriends) return b->getFriends();
    return nullptr;
}

void* bro_steam_getAvatar(const char* steamId, void* size) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->getAvatar) return b->getAvatar(steamId, size);
    return nullptr;
}

bool bro_steam_setRichPresence(const char* key, const char* value) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->setRichPresence) return b->setRichPresence(key, value);
    return false;
}

void bro_steam_clearRichPresence(void) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->clearRichPresence) b->clearRichPresence();
}

void* bro_steam_createLobby(const char* type, int32_t maxMembers) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->createLobby) return b->createLobby(type, maxMembers);
    return nullptr;
}

void* bro_steam_joinLobby(const char* lobbyId) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->joinLobby) return b->joinLobby(lobbyId);
    return nullptr;
}

void bro_steam_leaveLobby(const char* lobbyId) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->leaveLobby) b->leaveLobby(lobbyId);
}

bool bro_steam_setLobbyData(const char* lobbyId, const char* key, const char* value) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->setLobbyData) return b->setLobbyData(lobbyId, key, value);
    return false;
}

void* bro_steam_getLobbyMembers(const char* lobbyId) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->getLobbyMembers) return b->getLobbyMembers(lobbyId);
    return nullptr;
}

const char* bro_steam_getLobbyOwner(const char* lobbyId) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->getLobbyOwner) return b->getLobbyOwner(lobbyId);
    return "0";
}

const char* bro_steam_getLobbyData(const char* lobbyId, const char* key) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->getLobbyData) return b->getLobbyData(lobbyId, key);
    return "";
}

void bro_steam_requestLobbyList(void* filter) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->requestLobbyList) b->requestLobbyList(filter);
}

bool bro_steam_inviteUserToLobby(const char* lobbyId, const char* steamId) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->inviteUserToLobby) return b->inviteUserToLobby(lobbyId, steamId);
    return false;
}

void bro_steam_startVoiceRecording(void) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->startVoiceRecording) b->startVoiceRecording();
}

void bro_steam_stopVoiceRecording(void) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->stopVoiceRecording) b->stopVoiceRecording();
}

void* bro_steam_decodeVoice(void* data, int32_t sampleRate) {
    const auto* b = bro_get_steam_bridge();
    if (b && b->decodeVoice) return b->decodeVoice(data, sampleRate);
    return nullptr;
}

} // extern "C"
