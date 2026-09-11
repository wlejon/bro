#ifndef BRO_ENGINE_C_ABI_H
#define BRO_ENGINE_C_ABI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

void  bro_set_active_engine(void* engine);
void* bro_get_active_engine(void);

typedef struct BroTimeBridge {
    double (*getTimeScale)(void);
    void   (*setTimeScale)(double scale);
    bool   (*getTimePaused)(void);
    void   (*setTimePaused)(bool paused);
    double (*getTimeNowMs)(void);
} BroTimeBridge;

void bro_set_time_bridge(const BroTimeBridge* bridge);
const BroTimeBridge* bro_get_time_bridge(void);

typedef struct BroPathsBridge {
    const char* (*getAppDir)(void);
    const char* (*getUserDataDir)(void);
    const char* (*resolvePath)(const char* src);
    const char* (*resolveWritePath)(const char* src);
} BroPathsBridge;

void bro_set_paths_bridge(const BroPathsBridge* bridge);
const BroPathsBridge* bro_get_paths_bridge(void);

typedef struct BroDialogsBridge {
    void        (*alert)(const char* message);
    bool        (*confirm)(const char* message);
    const char* (*prompt)(const char* message, const char* defaultText);
    const char* (*showSaveFileDialog)(const char* filter, const char* defaultName);
    const char* (*showOpenFileDialog)(const char* filter, bool allowMultiple);
    const char* (*showOpenFolderDialog)(const char* defaultLocation, bool allowMultiple);
} BroDialogsBridge;

void bro_set_dialogs_bridge(const BroDialogsBridge* bridge);
const BroDialogsBridge* bro_get_dialogs_bridge(void);

typedef struct BroWindowBridge {
    const char* (*getState)(void);
    bool        (*getBorderless)(void);
    void        (*setBorderless)(bool val);
    bool        (*getAlwaysOnTop)(void);
    void        (*setAlwaysOnTop)(bool val);
    void        (*minimize)(void);
    void        (*maximize)(void);
    void        (*restore)(void);
    int32_t     (*getPositionX)(void);
    int32_t     (*getPositionY)(void);
    void        (*setPosition)(int32_t x, int32_t y);
    int32_t     (*getMinWidth)(void);
    int32_t     (*getMinHeight)(void);
    void        (*setMinSize)(int32_t width, int32_t height);
    int32_t     (*getMaxWidth)(void);
    int32_t     (*getMaxHeight)(void);
    void        (*setMaxSize)(int32_t width, int32_t height);
    int32_t     (*getDisplayCount)(void);
    bool        (*moveToDisplay)(uint32_t id);
} BroWindowBridge;

void bro_set_window_bridge(const BroWindowBridge* bridge);
const BroWindowBridge* bro_get_window_bridge(void);

typedef struct BroSettingsBridge {
    void        (*load)(void);
    void        (*save)(void);
    const char* (*get)(const char* key);
    void        (*set)(const char* key, const char* val);
    void        (*reset)(const char* category);
    bool        (*isActionPressed)(const char* action);
    double      (*getActionStrength)(const char* action);
} BroSettingsBridge;

void bro_set_settings_bridge(const BroSettingsBridge* bridge);
const BroSettingsBridge* bro_get_settings_bridge(void);

typedef struct BroMenuBridge {
    bool (*getVisible)(void);
    void (*show)(void);
    void (*hide)(void);
    void (*set)(void* items);
    bool (*addItem)(const char* parentId, void* item, int32_t index);
    bool (*updateItem)(const char* id, void* props);
    bool (*removeItem)(const char* id);
    void (*on)(const char* id, void* callback);
} BroMenuBridge;

void bro_set_menu_bridge(const BroMenuBridge* bridge);
const BroMenuBridge* bro_get_menu_bridge(void);

typedef struct BroMicBridge {
    void    (*start)(void* opts);
    void    (*stop)(void);
    bool    (*isActive)(void);
    int32_t (*engineRate)(void);
    void*   (*stats)(void);
    void*   (*levels)(int32_t maxCount);
    void    (*feed)(void* samples, int32_t sampleRate);
} BroMicBridge;

void bro_set_mic_bridge(const BroMicBridge* bridge);
const BroMicBridge* bro_get_mic_bridge(void);

typedef struct BroGamepadBridge {
    bool   (*isConnected)(int32_t index);
    double (*getAxis)(int32_t index, int32_t axis);
    double (*getButton)(int32_t index, int32_t button);
    bool   (*rumble)(int32_t index, float strong, float weak, int32_t duration);
    bool   (*rumbleTriggers)(int32_t index, float left, float right, int32_t duration);
} BroGamepadBridge;

void bro_set_gamepad_bridge(const BroGamepadBridge* bridge);
const BroGamepadBridge* bro_get_gamepad_bridge(void);

typedef struct BroMediaBridge {
    bool  (*getAvailable)(void);
    void* (*peaks)(const char* path, void* options);
    void* (*thumbnails)(const char* path, void* options);
} BroMediaBridge;

void bro_set_media_bridge(const BroMediaBridge* bridge);
const BroMediaBridge* bro_get_media_bridge(void);

typedef struct BroListenBridge {
    void*   (*open)(void* source);
    bool    (*supported)(void);
    void*   (*apps)(void);
    void    (*retain)(int32_t seconds);
    void*   (*audio)(int64_t startFrame, int64_t endFrame);
    int64_t (*frame)(void);
    void*   (*info)(void);
} BroListenBridge;

void bro_set_listen_bridge(const BroListenBridge* bridge);
const BroListenBridge* bro_get_listen_bridge(void);

typedef struct BroSteamBridge {
    bool        (*getAvailable)(void);
    const char* (*getReason)(void);
    uint32_t    (*getAppId)(void);
    const char* (*getSteamId)(void);
    const char* (*getPersonaName)(void);
    bool        (*getIsLoggedOn)(void);
    bool        (*getIsVoiceRecording)(void);
    int32_t     (*getVoiceSampleRate)(void);
    bool        (*getAchievement)(const char* name);
    bool        (*setAchievement)(const char* name);
    bool        (*clearAchievement)(const char* name);
    double      (*getStat)(const char* name);
    bool        (*setStat)(const char* name, double value);
    bool        (*storeStats)(void);
    void        (*activateOverlay)(const char* dialog);
    void        (*activateOverlayToWebPage)(const char* url);
    void*       (*getFriends)(void);
    void*       (*getAvatar)(const char* steamId, void* size);
    bool        (*setRichPresence)(const char* key, const char* value);
    void        (*clearRichPresence)(void);
    void*       (*createLobby)(const char* type, int32_t maxMembers);
    void*       (*joinLobby)(const char* lobbyId);
    void        (*leaveLobby)(const char* lobbyId);
    bool        (*setLobbyData)(const char* lobbyId, const char* key, const char* value);
    void*       (*getLobbyMembers)(const char* lobbyId);
    const char* (*getLobbyOwner)(const char* lobbyId);
    const char* (*getLobbyData)(const char* lobbyId, const char* key);
    void        (*requestLobbyList)(void* filter);
    bool        (*inviteUserToLobby)(const char* lobbyId, const char* steamId);
    void        (*startVoiceRecording)(void);
    void        (*stopVoiceRecording)(void);
    void*       (*decodeVoice)(void* data, int32_t sampleRate);
} BroSteamBridge;

void bro_set_steam_bridge(const BroSteamBridge* bridge);
const BroSteamBridge* bro_get_steam_bridge(void);

typedef struct BroServerBridge {
    double (*getTickrate)(void);
    void   (*setTickrate)(double val);
    double (*getUptime)(void);
    void   (*stop)(void);
} BroServerBridge;

void bro_set_server_bridge(const BroServerBridge* bridge);
const BroServerBridge* bro_get_server_bridge(void);

typedef struct BroNetBridge {
    void        (*host)(int32_t port, void* callback);
    void        (*unhost)(void);
    int32_t     (*connect)(const char* address, int32_t port, void* callback);
    void        (*disconnect)(int32_t peerId);
    void        (*disconnectAll)(void);
    void        (*send)(int32_t peerId, void* data, int32_t channel);
    void        (*broadcast)(void* data, int32_t channel);
    void        (*sendClone)(int32_t peerId, void* value, int32_t channel);
    void        (*broadcastClone)(void* value, int32_t channel);
    void*       (*peers)(void);
    const char* (*getPeerAddress)(int32_t peerId);
    void*       (*stats)(void);
    void*       (*getPeerStats)(int32_t peerId);
    void        (*setPeerSimulatedLoss)(int32_t peerId, double chance, double latencyMin, double latencyMax);
} BroNetBridge;

void bro_set_net_bridge(const BroNetBridge* bridge);
const BroNetBridge* bro_get_net_bridge(void);

typedef struct BroTextBridge {
    bool    (*getBidiAvailable)(void);
    void*   (*shape)(const char* text, void* options);
    void*   (*byteOffsetToX)(const char* text, void* options, int32_t byteOffset);
    int32_t (*xToByteOffset)(const char* text, void* options, double x);
    void*   (*clusterRange)(const char* text, void* options, int32_t byteOffset);
    void*   (*cacheStats)(void);
    void*   (*bidi)(const char* text, const char* base, bool override);
    void*   (*bidiReorder)(void* levels);
} BroTextBridge;

void bro_set_text_bridge(const BroTextBridge* bridge);
const BroTextBridge* bro_get_text_bridge(void);

typedef struct BroGpuBridge {
    bool        (*getAvailable)(void);
    const char* (*getBackend)(void);
    void*       (*getDevices)(void);
    int32_t     (*deviceCount)(const char* device);
    void*       (*getCompiledBackends)(void);
    void*       (*memoryInfo)(const char* device);
    const char* (*deviceName)(const char* device);
    bool        (*trim)(const char* device, uint64_t keepBytes);
} BroGpuBridge;

void bro_set_gpu_bridge(const BroGpuBridge* bridge);
const BroGpuBridge* bro_get_gpu_bridge(void);

typedef struct BroGizmoBridge {
    bool        (*getVisible)(void);
    bool        (*getDragging)(void);
    const char* (*getHovered)(void);
    void        (*show)(void);
    void        (*hide)(void);
    void        (*setMode)(const char* mode);
    void        (*setSpace)(const char* space);
    void        (*setPosition)(double x, double y, double z);
    void        (*setOrientation)(double x, double y, double z, double w);
    void        (*configure)(void* config);
    void        (*attach)(void* handlers);
    void        (*detach)(void);
} BroGizmoBridge;

void bro_set_gizmo_bridge(const BroGizmoBridge* bridge);
const BroGizmoBridge* bro_get_gizmo_bridge(void);

typedef struct BroPhysicsBridge {
    void    (*setGravity)(double x, double y, double z);
    void*   (*getGravity)(void);
    int32_t (*createBody)(void* config);
    void    (*destroyBody)(int32_t tag);
    void    (*destroyAll)(void);
    void*   (*getTransform)(int32_t tag);
    void*   (*getVelocity)(int32_t tag);
    void    (*setPosition)(int32_t tag, double x, double y, double z);
    void    (*setRotation)(int32_t tag, double x, double y, double z, double w);
    void    (*setLinearVelocity)(int32_t tag, double x, double y, double z);
    void    (*setAngularVelocity)(int32_t tag, double x, double y, double z);
    void    (*addForce)(int32_t tag, double x, double y, double z);
    void    (*addImpulse)(int32_t tag, double x, double y, double z);
    void    (*addTorque)(int32_t tag, double x, double y, double z);
    void*   (*raycast)(double ox, double oy, double oz, double dx, double dy, double dz, double maxDist, int32_t mask);
    void*   (*raycastClosest)(double ox, double oy, double oz, double dx, double dy, double dz, double maxDist, int32_t mask);
    void    (*step)(double dt);
    void    (*setTimeStep)(double dt);
    void    (*setInterpolation)(bool enabled);
    bool    (*getInterpolation)(void);
    bool    (*isActive)(int32_t tag);
    void    (*activate)(int32_t tag);
    void*   (*createCharacter)(void* config);
    void*   (*createVehicle)(void* config);
    void*   (*createRagdoll)(void* config);
    void*   (*createSoftBody)(void* config);
} BroPhysicsBridge;

void bro_set_physics_bridge(const BroPhysicsBridge* bridge);
const BroPhysicsBridge* bro_get_physics_bridge(void);

typedef struct BroFloraBridge {
    void   (*setWind)(double strength, double dirX, double dirY);
    void   (*setDensity)(double density);
    void   (*update)(double dt);
    void   (*clear)(void);
    void   (*placement)(void* config);
    void*  (*batches)(void);
    void*  (*createWorld)(void* opts);
} BroFloraBridge;

void bro_set_flora_bridge(const BroFloraBridge* bridge);
const BroFloraBridge* bro_get_flora_bridge(void);

typedef struct BroMotionBridge {
    void  (*init)(void);
    void* (*load)(void* opts);
} BroMotionBridge;

void bro_set_motion_bridge(const BroMotionBridge* bridge);
const BroMotionBridge* bro_get_motion_bridge(void);

#ifdef __cplusplus
}
#endif

#endif // BRO_ENGINE_C_ABI_H

