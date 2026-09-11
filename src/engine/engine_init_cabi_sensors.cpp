#include "engine/engine_init_cabi_sensors.h"
#include "bro/c_abi/bro_engine_c_abi.h"

namespace bro::engine {

void bro_engine_register_cabi_sensor_bridges() {
    static BroGestureBridge s_engine_gesture_bridge = {
        .init = []() {},
        .enrollFromAudio = [](const char* /*name*/, void* /*samples*/, void* /*policy*/) -> int32_t { return 0; },
        .remove = [](const char* /*name*/) -> bool { return false; },
        .clear = []() {},
        .templates = []() -> void* { return nullptr; },
        .inspect = [](const char* /*name*/) -> void* { return nullptr; },
        .reset = []() {},
        .listen = [](void* /*opts*/) {},
        .stop = []() {},
        .isActive = []() -> bool { return false; },
        .sampleRate = []() -> int32_t { return 16000; }
    };
    bro_set_gesture_bridge(&s_engine_gesture_bridge);

    static BroSenseBridge s_engine_sense_bridge = {
        .init = []() {},
        .start = [](void* /*opts*/) {},
        .stop = []() {},
        .isActive = []() -> bool { return false; },
        .snapshot = []() -> void* { return nullptr; },
        .sampleRate = []() -> int32_t { return 16000; },
        .stats = []() -> void* { return nullptr; },
        .feed = [](void* /*samples*/) -> void* { return nullptr; },
        .analyze = [](void* /*samples*/, void* /*opts*/) -> void* { return nullptr; }
    };
    bro_set_sense_bridge(&s_engine_sense_bridge);

    static BroWakeBridge s_engine_wake_bridge = {
        .init = []() {},
        .load = [](void* /*opts*/) {},
        .unload = []() {},
        .listen = [](void* /*opts*/) {},
        .stop = []() {},
        .suspend = []() {},
        .resume = []() {},
        .lastScore = []() -> double { return 0.0; },
        .isActive = []() -> bool { return false; },
        .isSuspended = []() -> bool { return false; },
        .isLoaded = []() -> bool { return true; },
        .setThreshold = [](double /*threshold*/) {},
        .stats = []() -> void* { return nullptr; },
        .feed = [](void* /*samples*/, int32_t /*sampleRate*/) -> void* { return nullptr; }
    };
    bro_set_wake_bridge(&s_engine_wake_bridge);
}

} // namespace bro::engine
