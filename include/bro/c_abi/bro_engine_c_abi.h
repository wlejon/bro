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

#ifdef __cplusplus
}
#endif

#endif // BRO_ENGINE_C_ABI_H
