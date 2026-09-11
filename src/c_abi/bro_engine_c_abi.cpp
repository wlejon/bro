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

static BroMenuBridge s_menu_bridge = {};

void bro_set_menu_bridge(const BroMenuBridge* bridge) {
    if (bridge) {
        s_menu_bridge = *bridge;
    } else {
        s_menu_bridge = {};
    }
}

const BroMenuBridge* bro_get_menu_bridge(void) {
    return &s_menu_bridge;
}

static BroMicBridge s_mic_bridge = {};

void bro_set_mic_bridge(const BroMicBridge* bridge) {
    if (bridge) {
        s_mic_bridge = *bridge;
    } else {
        s_mic_bridge = {};
    }
}

const BroMicBridge* bro_get_mic_bridge(void) {
    return &s_mic_bridge;
}

static BroGamepadBridge s_gamepad_bridge = {};

void bro_set_gamepad_bridge(const BroGamepadBridge* bridge) {
    if (bridge) {
        s_gamepad_bridge = *bridge;
    } else {
        s_gamepad_bridge = {};
    }
}

const BroGamepadBridge* bro_get_gamepad_bridge(void) {
    return &s_gamepad_bridge;
}

static BroMediaBridge s_media_bridge = {};

void bro_set_media_bridge(const BroMediaBridge* bridge) {
    if (bridge) {
        s_media_bridge = *bridge;
    } else {
        s_media_bridge = {};
    }
}

const BroMediaBridge* bro_get_media_bridge(void) {
    return &s_media_bridge;
}

static BroListenBridge s_listen_bridge = {};

void bro_set_listen_bridge(const BroListenBridge* bridge) {
    if (bridge) {
        s_listen_bridge = *bridge;
    } else {
        s_listen_bridge = {};
    }
}

const BroListenBridge* bro_get_listen_bridge(void) {
    return &s_listen_bridge;
}

static BroSteamBridge s_steam_bridge = {};

void bro_set_steam_bridge(const BroSteamBridge* bridge) {
    if (bridge) {
        s_steam_bridge = *bridge;
    } else {
        s_steam_bridge = {};
    }
}

const BroSteamBridge* bro_get_steam_bridge(void) {
    return &s_steam_bridge;
}

static BroServerBridge s_server_bridge = {};

void bro_set_server_bridge(const BroServerBridge* bridge) {
    if (bridge) {
        s_server_bridge = *bridge;
    } else {
        s_server_bridge = {};
    }
}

const BroServerBridge* bro_get_server_bridge(void) {
    return &s_server_bridge;
}

static BroNetBridge s_net_bridge = {};

void bro_set_net_bridge(const BroNetBridge* bridge) {
    if (bridge) {
        s_net_bridge = *bridge;
    } else {
        s_net_bridge = {};
    }
}

const BroNetBridge* bro_get_net_bridge(void) {
    return &s_net_bridge;
}

static BroTextBridge s_text_bridge = {};

void bro_set_text_bridge(const BroTextBridge* bridge) {
    if (bridge) {
        s_text_bridge = *bridge;
    } else {
        s_text_bridge = {};
    }
}

const BroTextBridge* bro_get_text_bridge(void) {
    return &s_text_bridge;
}

static BroGpuBridge s_gpu_bridge = {};

void bro_set_gpu_bridge(const BroGpuBridge* bridge) {
    if (bridge) {
        s_gpu_bridge = *bridge;
    } else {
        s_gpu_bridge = {};
    }
}

const BroGpuBridge* bro_get_gpu_bridge(void) {
    return &s_gpu_bridge;
}

static BroGizmoBridge s_gizmo_bridge = {};

void bro_set_gizmo_bridge(const BroGizmoBridge* bridge) {
    if (bridge) {
        s_gizmo_bridge = *bridge;
    } else {
        s_gizmo_bridge = {};
    }
}

const BroGizmoBridge* bro_get_gizmo_bridge(void) {
    return &s_gizmo_bridge;
}

static BroPhysicsBridge s_physics_bridge = {};

void bro_set_physics_bridge(const BroPhysicsBridge* bridge) {
    if (bridge) {
        s_physics_bridge = *bridge;
    } else {
        s_physics_bridge = {};
    }
}

const BroPhysicsBridge* bro_get_physics_bridge(void) {
    return &s_physics_bridge;
}

static BroFloraBridge s_flora_bridge = {};

void bro_set_flora_bridge(const BroFloraBridge* bridge) {
    if (bridge) {
        s_flora_bridge = *bridge;
    } else {
        s_flora_bridge = {};
    }
}

const BroFloraBridge* bro_get_flora_bridge(void) {
    return &s_flora_bridge;
}

static BroMotionBridge s_motion_bridge = {};

void bro_set_motion_bridge(const BroMotionBridge* bridge) {
    if (bridge) {
        s_motion_bridge = *bridge;
    } else {
        s_motion_bridge = {};
    }
}

const BroMotionBridge* bro_get_motion_bridge(void) {
    return &s_motion_bridge;
}

}


