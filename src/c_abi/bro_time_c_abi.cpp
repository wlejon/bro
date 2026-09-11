// =============================================================================
// bro_time_c_abi.cpp — C++ forwarding implementations for bro.time
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_time_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"

extern "C" {


static double s_fallback_time_scale = 1.0;
static bool s_fallback_time_paused = false;

double bro_time_get_scale(void) {
    const auto* b = bro_get_time_bridge();
    return (b && b->getTimeScale) ? b->getTimeScale() : s_fallback_time_scale;
}

void bro_time_set_scale(double val) {
    const auto* b = bro_get_time_bridge();
    if (b && b->setTimeScale) {
        b->setTimeScale(val);
    } else {
        s_fallback_time_scale = val;
    }
}

bool bro_time_get_paused(void) {
    const auto* b = bro_get_time_bridge();
    return (b && b->getTimePaused) ? b->getTimePaused() : s_fallback_time_paused;
}

void bro_time_set_paused(bool val) {
    const auto* b = bro_get_time_bridge();
    if (b && b->setTimePaused) {
        b->setTimePaused(val);
    } else {
        s_fallback_time_paused = val;
    }
}

double bro_time_get_now(void) {
    const auto* b = bro_get_time_bridge();
    return (b && b->getTimeNowMs) ? b->getTimeNowMs() : 0.0;
}

} // extern "C"
