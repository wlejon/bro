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

static BroAIBridge s_ai_bridge = {};

void bro_set_ai_bridge(const BroAIBridge* bridge) {
    if (bridge) {
        s_ai_bridge = *bridge;
    } else {
        s_ai_bridge = {};
    }
}

const BroAIBridge* bro_get_ai_bridge(void) {
    return &s_ai_bridge;
}

static BroTensorBridge s_tensor_bridge = {};

void bro_set_tensor_bridge(const BroTensorBridge* bridge) {
    if (bridge) {
        s_tensor_bridge = *bridge;
    } else {
        s_tensor_bridge = {};
    }
}

const BroTensorBridge* bro_get_tensor_bridge(void) {
    return &s_tensor_bridge;
}

static BroVisionBridge s_vision_bridge = {};

void bro_set_vision_bridge(const BroVisionBridge* bridge) {
    if (bridge) {
        s_vision_bridge = *bridge;
    } else {
        s_vision_bridge = {};
    }
}

const BroVisionBridge* bro_get_vision_bridge(void) {
    return &s_vision_bridge;
}

static BroDiffusionBridge s_diffusion_bridge = {};

void bro_set_diffusion_bridge(const BroDiffusionBridge* bridge) {
    if (bridge) {
        s_diffusion_bridge = *bridge;
    } else {
        s_diffusion_bridge = {};
    }
}

const BroDiffusionBridge* bro_get_diffusion_bridge(void) {
    return &s_diffusion_bridge;
}

static BroLMBridge s_lm_bridge = {};

void bro_set_lm_bridge(const BroLMBridge* bridge) {
    if (bridge) {
        s_lm_bridge = *bridge;
    } else {
        s_lm_bridge = {};
    }
}

const BroLMBridge* bro_get_lm_bridge(void) {
    return &s_lm_bridge;
}

static BroSttBridge s_stt_bridge = {};

void bro_set_stt_bridge(const BroSttBridge* bridge) {
    if (bridge) {
        s_stt_bridge = *bridge;
    } else {
        s_stt_bridge = {};
    }
}

const BroSttBridge* bro_get_stt_bridge(void) {
    return &s_stt_bridge;
}

static BroTtsBridge s_tts_bridge = {};

void bro_set_tts_bridge(const BroTtsBridge* bridge) {
    if (bridge) {
        s_tts_bridge = *bridge;
    } else {
        s_tts_bridge = {};
    }
}

const BroTtsBridge* bro_get_tts_bridge(void) {
    return &s_tts_bridge;
}

static BroKwsBridge s_kws_bridge = {};

void bro_set_kws_bridge(const BroKwsBridge* bridge) {
    if (bridge) {
        s_kws_bridge = *bridge;
    } else {
        s_kws_bridge = {};
    }
}

const BroKwsBridge* bro_get_kws_bridge(void) {
    return &s_kws_bridge;
}

static BroDiarBridge s_diar_bridge = {};

void bro_set_diar_bridge(const BroDiarBridge* bridge) {
    if (bridge) {
        s_diar_bridge = *bridge;
    } else {
        s_diar_bridge = {};
    }
}

const BroDiarBridge* bro_get_diar_bridge(void) {
    return &s_diar_bridge;
}

static BroRaveBridge s_rave_bridge = {};

void bro_set_rave_bridge(const BroRaveBridge* bridge) {
    if (bridge) {
        s_rave_bridge = *bridge;
    } else {
        s_rave_bridge = {};
    }
}

const BroRaveBridge* bro_get_rave_bridge(void) {
    return &s_rave_bridge;
}

static BroGestureBridge s_gesture_bridge = {};

void bro_set_gesture_bridge(const BroGestureBridge* bridge) {
    if (bridge) {
        s_gesture_bridge = *bridge;
    } else {
        s_gesture_bridge = {};
    }
}

const BroGestureBridge* bro_get_gesture_bridge(void) {
    return &s_gesture_bridge;
}

static BroSenseBridge s_sense_bridge = {};

void bro_set_sense_bridge(const BroSenseBridge* bridge) {
    if (bridge) {
        s_sense_bridge = *bridge;
    } else {
        s_sense_bridge = {};
    }
}

const BroSenseBridge* bro_get_sense_bridge(void) {
    return &s_sense_bridge;
}

static BroWakeBridge s_wake_bridge = {};

void bro_set_wake_bridge(const BroWakeBridge* bridge) {
    if (bridge) {
        s_wake_bridge = *bridge;
    } else {
        s_wake_bridge = {};
    }
}

const BroWakeBridge* bro_get_wake_bridge(void) {
    return &s_wake_bridge;
}

static BroCustomElementsBridge s_custom_elements_bridge = {};

void bro_set_custom_elements_bridge(const BroCustomElementsBridge* bridge) {
    if (bridge) {
        s_custom_elements_bridge = *bridge;
    } else {
        s_custom_elements_bridge = {};
    }
}

const BroCustomElementsBridge* bro_get_custom_elements_bridge(void) {
    return &s_custom_elements_bridge;
}

static BroIframeBridge s_iframe_bridge = {};

void bro_set_iframe_bridge(const BroIframeBridge* bridge) {
    if (bridge) {
        s_iframe_bridge = *bridge;
    } else {
        s_iframe_bridge = {};
    }
}

const BroIframeBridge* bro_get_iframe_bridge(void) {
    return &s_iframe_bridge;
}

static BroMatchMediaBridge s_matchmedia_bridge = {};

void bro_set_matchmedia_bridge(const BroMatchMediaBridge* bridge) {
    if (bridge) {
        s_matchmedia_bridge = *bridge;
    } else {
        s_matchmedia_bridge = {};
    }
}

const BroMatchMediaBridge* bro_get_matchmedia_bridge(void) {
    return &s_matchmedia_bridge;
}

static BroVendorGlobalsBridge s_vendor_globals_bridge = {};

void bro_set_vendor_globals_bridge(const BroVendorGlobalsBridge* bridge) {
    if (bridge) {
        s_vendor_globals_bridge = *bridge;
    } else {
        s_vendor_globals_bridge = {};
    }
}

const BroVendorGlobalsBridge* bro_get_vendor_globals_bridge(void) {
    return &s_vendor_globals_bridge;
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
static void* bro_get_module_proc(void* handle, const char* name) {
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), name));
}
#else
#include <dlfcn.h>
static void* bro_get_module_proc(void* handle, const char* name) {
    return ::dlsym(handle, name);
}
#endif

void bro_c_abi_sync_bridges_to_module(void* moduleHandle) {
    if (!moduleHandle) return;

    using SetEngineFn = void (*)(void*);
    auto set_eng = reinterpret_cast<SetEngineFn>(bro_get_module_proc(moduleHandle, "bro_set_active_engine"));
    if (set_eng) {
        set_eng(bro_get_active_engine());
    }

#define BRO_SYNC_ONE_BRIDGE(name_suffix, BridgeType, getter_fn) \
    do { \
        using SetterFn = void (*)(const BridgeType*); \
        auto pSetter = reinterpret_cast<SetterFn>(bro_get_module_proc(moduleHandle, "bro_set_" #name_suffix "_bridge")); \
        if (pSetter) { \
            pSetter(getter_fn()); \
        } \
    } while (0)

    BRO_SYNC_ONE_BRIDGE(time, BroTimeBridge, bro_get_time_bridge);
    BRO_SYNC_ONE_BRIDGE(paths, BroPathsBridge, bro_get_paths_bridge);
    BRO_SYNC_ONE_BRIDGE(dialogs, BroDialogsBridge, bro_get_dialogs_bridge);
    BRO_SYNC_ONE_BRIDGE(window, BroWindowBridge, bro_get_window_bridge);
    BRO_SYNC_ONE_BRIDGE(settings, BroSettingsBridge, bro_get_settings_bridge);
    BRO_SYNC_ONE_BRIDGE(menu, BroMenuBridge, bro_get_menu_bridge);
    BRO_SYNC_ONE_BRIDGE(mic, BroMicBridge, bro_get_mic_bridge);
    BRO_SYNC_ONE_BRIDGE(gamepad, BroGamepadBridge, bro_get_gamepad_bridge);
    BRO_SYNC_ONE_BRIDGE(media, BroMediaBridge, bro_get_media_bridge);
    BRO_SYNC_ONE_BRIDGE(listen, BroListenBridge, bro_get_listen_bridge);
    BRO_SYNC_ONE_BRIDGE(steam, BroSteamBridge, bro_get_steam_bridge);
    BRO_SYNC_ONE_BRIDGE(server, BroServerBridge, bro_get_server_bridge);
    BRO_SYNC_ONE_BRIDGE(net, BroNetBridge, bro_get_net_bridge);
    BRO_SYNC_ONE_BRIDGE(text, BroTextBridge, bro_get_text_bridge);
    BRO_SYNC_ONE_BRIDGE(gpu, BroGpuBridge, bro_get_gpu_bridge);
    BRO_SYNC_ONE_BRIDGE(gizmo, BroGizmoBridge, bro_get_gizmo_bridge);
    BRO_SYNC_ONE_BRIDGE(physics, BroPhysicsBridge, bro_get_physics_bridge);
    BRO_SYNC_ONE_BRIDGE(flora, BroFloraBridge, bro_get_flora_bridge);
    BRO_SYNC_ONE_BRIDGE(motion, BroMotionBridge, bro_get_motion_bridge);
    BRO_SYNC_ONE_BRIDGE(ai, BroAIBridge, bro_get_ai_bridge);
    BRO_SYNC_ONE_BRIDGE(tensor, BroTensorBridge, bro_get_tensor_bridge);
    BRO_SYNC_ONE_BRIDGE(vision, BroVisionBridge, bro_get_vision_bridge);
    BRO_SYNC_ONE_BRIDGE(diffusion, BroDiffusionBridge, bro_get_diffusion_bridge);
    BRO_SYNC_ONE_BRIDGE(lm, BroLMBridge, bro_get_lm_bridge);
    BRO_SYNC_ONE_BRIDGE(stt, BroSttBridge, bro_get_stt_bridge);
    BRO_SYNC_ONE_BRIDGE(tts, BroTtsBridge, bro_get_tts_bridge);
    BRO_SYNC_ONE_BRIDGE(kws, BroKwsBridge, bro_get_kws_bridge);
    BRO_SYNC_ONE_BRIDGE(diar, BroDiarBridge, bro_get_diar_bridge);
    BRO_SYNC_ONE_BRIDGE(rave, BroRaveBridge, bro_get_rave_bridge);
    BRO_SYNC_ONE_BRIDGE(gesture, BroGestureBridge, bro_get_gesture_bridge);
    BRO_SYNC_ONE_BRIDGE(sense, BroSenseBridge, bro_get_sense_bridge);
    BRO_SYNC_ONE_BRIDGE(wake, BroWakeBridge, bro_get_wake_bridge);
    BRO_SYNC_ONE_BRIDGE(custom_elements, BroCustomElementsBridge, bro_get_custom_elements_bridge);
    BRO_SYNC_ONE_BRIDGE(iframe, BroIframeBridge, bro_get_iframe_bridge);
    BRO_SYNC_ONE_BRIDGE(matchmedia, BroMatchMediaBridge, bro_get_matchmedia_bridge);
    BRO_SYNC_ONE_BRIDGE(vendor_globals, BroVendorGlobalsBridge, bro_get_vendor_globals_bridge);

#undef BRO_SYNC_ONE_BRIDGE
}

}




