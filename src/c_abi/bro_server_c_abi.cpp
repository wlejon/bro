// =============================================================================
// bro_server_c_abi.cpp — C++ forwarding implementations for bro.server
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_server_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"

extern "C" {

static double s_fallback_tickrate = 60.0;
static double s_fallback_uptime = 0.0;

double bro_server_get_tickrate(void) {
    const auto* b = bro_get_server_bridge();
    if (b && b->getTickrate) return b->getTickrate();
    return s_fallback_tickrate;
}

void bro_server_set_tickrate(double val) {
    const auto* b = bro_get_server_bridge();
    if (b && b->setTickrate) {
        b->setTickrate(val);
    } else {
        s_fallback_tickrate = val;
    }
}

double bro_server_get_uptime(void) {
    const auto* b = bro_get_server_bridge();
    if (b && b->getUptime) return b->getUptime();
    return s_fallback_uptime;
}

void bro_server_stop(void) {
    const auto* b = bro_get_server_bridge();
    if (b && b->stop) b->stop();
}

} // extern "C"
