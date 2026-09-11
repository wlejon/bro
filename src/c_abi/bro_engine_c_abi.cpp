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

}
