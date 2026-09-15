#include "natives/mic/native_mic_decl.h"
#include <vector>
#include <cstdint>

namespace {

struct MicStatsSlot {
    double framesDelivered = 0.0;
    double samplesDelivered = 0.0;
    double rollingPeak = 0.0;
    double chunkCount = 0.0;
    double dropped = 0.0;
    int32_t chunkFrames = 512;
};

static bool s_micActive = false;
static int32_t s_engineRate = 48000;
static thread_local MicStatsSlot tl_micStats;
static thread_local std::vector<double> tl_levels;

} // namespace

extern "C" {

void bro_mic_start(bool /*opts_chunkFrames_given*/, int32_t /*opts_chunkFrames*/,
                   bool /*opts_targetRate_given*/, int32_t /*opts_targetRate*/,
                   bool /*opts_agc_given*/, bool /*opts_agc*/,
                   bool /*opts_live_given*/, bool /*opts_live*/,
                   bool /*opts_samples_given*/, bool /*opts_samples*/,
                   uint64_t /*opts_onChunk*/,
                   bool /*opts_targetPeak_given*/, double /*opts_targetPeak*/,
                   bool /*opts_halfLifeSec_given*/, double /*opts_halfLifeSec*/,
                   bool /*opts_noiseGate_given*/, double /*opts_noiseGate*/,
                   bool /*opts_maxGain_given*/, double /*opts_maxGain*/) {
    s_micActive = true;
}

void bro_mic_stop(void) {
    s_micActive = false;
}

bool bro_mic_isActive(void) {
    return s_micActive;
}

int32_t bro_mic_engineRate(void) {
    return s_engineRate;
}

bool bro_mic_stats(void) {
    tl_micStats.framesDelivered = 0.0;
    tl_micStats.samplesDelivered = 0.0;
    tl_micStats.rollingPeak = 0.0;
    tl_micStats.chunkCount = 0.0;
    tl_micStats.dropped = 0.0;
    tl_micStats.chunkFrames = 512;
    return true;
}

double bro_mic_stats_framesDelivered(void) {
    return tl_micStats.framesDelivered;
}

double bro_mic_stats_samplesDelivered(void) {
    return tl_micStats.samplesDelivered;
}

double bro_mic_stats_rollingPeak(void) {
    return tl_micStats.rollingPeak;
}

double bro_mic_stats_chunkCount(void) {
    return tl_micStats.chunkCount;
}

double bro_mic_stats_dropped(void) {
    return tl_micStats.dropped;
}

int32_t bro_mic_stats_chunkFrames(void) {
    return tl_micStats.chunkFrames;
}

void bro_mic_levels(bool maxCount_given, int32_t maxCount, bronze_native_buffer* out) {
    if (!out) return;
    int32_t count = (maxCount_given && maxCount > 0) ? maxCount : 16;
    tl_levels.assign(count, 0.0);
    out->data = tl_levels.data();
    out->length = static_cast<uint32_t>(tl_levels.size());
    out->release = nullptr;
    out->ctx = nullptr;
}

void bro_mic_feed(const float* /*samples*/, uint32_t /*samples_len*/,
                  bool /*sampleRate_given*/, int32_t /*sampleRate*/) {}

} // extern "C"

namespace bro::bronze_host {
bool registerNatives_mic(std::string* error);
bool registerMicNatives(std::string* error) {
    return registerNatives_mic(error);
}
} // namespace bro::bronze_host
