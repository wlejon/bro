#include "bro/c_abi/bro_engine_c_abi.h"

static void* s_active_engine = nullptr;
static BroTimeBridge s_time_bridge = {};

extern "C" {

void bro_set_active_engine(void* engine) {
    s_active_engine = engine;
}

void* bro_get_active_engine(void) {
    return s_active_engine;
}

void bro_set_time_bridge(const BroTimeBridge* bridge) {
    if (bridge) {
        s_time_bridge = *bridge;
    } else {
        s_time_bridge = {};
    }
}

const BroTimeBridge* bro_get_time_bridge(void) {
    return &s_time_bridge;
}

static BroPathsBridge s_paths_bridge = {};

void bro_set_paths_bridge(const BroPathsBridge* bridge) {
    if (bridge) {
        s_paths_bridge = *bridge;
    } else {
        s_paths_bridge = {};
    }
}

const BroPathsBridge* bro_get_paths_bridge(void) {
    return &s_paths_bridge;
}

static BroDialogsBridge s_dialogs_bridge = {};

void bro_set_dialogs_bridge(const BroDialogsBridge* bridge) {
    if (bridge) {
        s_dialogs_bridge = *bridge;
    } else {
        s_dialogs_bridge = {};
    }
}

const BroDialogsBridge* bro_get_dialogs_bridge(void) {
    return &s_dialogs_bridge;
}

static BroWindowBridge s_window_bridge = {};

void bro_set_window_bridge(const BroWindowBridge* bridge) {
    if (bridge) {
        s_window_bridge = *bridge;
    } else {
        s_window_bridge = {};
    }
}

const BroWindowBridge* bro_get_window_bridge(void) {
    return &s_window_bridge;
}

static BroSettingsBridge s_settings_bridge = {};

void bro_set_settings_bridge(const BroSettingsBridge* bridge) {
    if (bridge) {
        s_settings_bridge = *bridge;
    } else {
        s_settings_bridge = {};
    }
}

const BroSettingsBridge* bro_get_settings_bridge(void) {
    return &s_settings_bridge;
}

}
