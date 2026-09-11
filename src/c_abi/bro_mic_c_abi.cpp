// =============================================================================
// bro_mic_c_abi.cpp — C++ forwarding implementations for bro.mic
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_mic_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"

extern "C" {

static bool s_fallback_mic_active = false;
static int32_t s_fallback_engine_rate = 48000;

void bro_mic_start(void* opts) {
    const auto* b = bro_get_mic_bridge();
    if (b && b->start) {
        b->start(opts);
    } else {
        s_fallback_mic_active = true;
    }
}

void bro_mic_stop(void) {
    const auto* b = bro_get_mic_bridge();
    if (b && b->stop) {
        b->stop();
    } else {
        s_fallback_mic_active = false;
    }
}

bool bro_mic_isActive(void) {
    const auto* b = bro_get_mic_bridge();
    if (b && b->isActive) {
        return b->isActive();
    }
    return s_fallback_mic_active;
}

int32_t bro_mic_engineRate(void) {
    const auto* b = bro_get_mic_bridge();
    if (b && b->engineRate) {
        return b->engineRate();
    }
    return s_fallback_engine_rate;
}

void* bro_mic_stats(void) {
    const auto* b = bro_get_mic_bridge();
    if (b && b->stats) {
        return b->stats();
    }
    return nullptr;
}

void* bro_mic_levels(int32_t maxCount) {
    const auto* b = bro_get_mic_bridge();
    if (b && b->levels) {
        return b->levels(maxCount);
    }
    return nullptr;
}

void bro_mic_feed(void* samples, int32_t sampleRate) {
    const auto* b = bro_get_mic_bridge();
    if (b && b->feed) {
        b->feed(samples, sampleRate);
    }
}

} // extern "C"
