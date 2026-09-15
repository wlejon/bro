#include "natives/wake/native_wake_decl.h"
#include <cstdint>
#include <string>

namespace {

struct WakeStatsData {
    double framesDelivered = 0.0;
    double samplesDelivered = 0.0;
    double rollingPeak = 0.0;
    double scoreMax = 0.0;
};

struct BroWakeStreamViewImpl {
    bool active = false;
    bool suspended = false;
    bool loaded = false;
    double threshold = 0.85;
    double lastScore = 0.0;
};

static bool s_wakeLoaded = false;
static bool s_wakeActive = false;
static bool s_wakeSuspended = false;
static double s_wakeThreshold = 0.85;
static double s_wakeLastScore = 0.0;

static thread_local WakeStatsData tl_viewStats;
static thread_local WakeStatsData tl_nsStats;

} // namespace

extern "C" {

void bro_wake_WakeStreamView_dtor(void* self) {
    delete static_cast<BroWakeStreamViewImpl*>(self);
}

void* bro_wake_WakeStreamView_ctor(void) {
    return new BroWakeStreamViewImpl();
}

bool bro_wake_WakeStreamView_active_get(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    return v ? v->active : false;
}

void bro_wake_WakeStreamView_listen(
    void* self,
    bool /*opts_weights_given*/, const char* /*opts_weights*/,
    uint64_t /*opts_onFire*/, double opts_threshold,
    bool /*opts_smoothing_hits_given*/, int32_t /*opts_smoothing_hits*/,
    bool /*opts_smoothing_window_given*/, int32_t /*opts_smoothing_window*/,
    int32_t /*opts_refractoryMs*/,
    bool /*opts_device_given*/, const char* /*opts_device*/)
{
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    if (v) {
        v->active = true;
        v->threshold = opts_threshold;
    }
}

void bro_wake_WakeStreamView_stop(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    if (v) v->active = false;
}

void bro_wake_WakeStreamView_suspend(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    if (v) v->suspended = true;
}

void bro_wake_WakeStreamView_resume(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    if (v) v->suspended = false;
}

double bro_wake_WakeStreamView_lastScore(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    return v ? v->lastScore : 0.0;
}

bool bro_wake_WakeStreamView_isActive(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    return v ? v->active : false;
}

bool bro_wake_WakeStreamView_isSuspended(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    return v ? v->suspended : false;
}

bool bro_wake_WakeStreamView_isLoaded(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    return v ? v->loaded : false;
}

void bro_wake_WakeStreamView_setThreshold(void* self, double threshold) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    if (v) v->threshold = threshold;
}

bool bro_wake_WakeStreamView_stats(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    if (!v || !v->active) return false;
    tl_viewStats = WakeStatsData{};
    return true;
}

double bro_wake_WakeStreamView_stats_framesDelivered(void) {
    return tl_viewStats.framesDelivered;
}

double bro_wake_WakeStreamView_stats_samplesDelivered(void) {
    return tl_viewStats.samplesDelivered;
}

double bro_wake_WakeStreamView_stats_rollingPeak(void) {
    return tl_viewStats.rollingPeak;
}

double bro_wake_WakeStreamView_stats_scoreMax(void) {
    return tl_viewStats.scoreMax;
}

// --- Namespace bro.wake ---

void bro_wake_init(void) {}

void bro_wake_load(const char* /*opts_weights*/, bool /*opts_device_given*/, const char* /*opts_device*/) {
    s_wakeLoaded = true;
}

void bro_wake_unload(void) {
    s_wakeLoaded = false;
    s_wakeActive = false;
    s_wakeSuspended = false;
}

void bro_wake_listen(
    bool /*opts_weights_given*/, const char* /*opts_weights*/,
    uint64_t /*opts_onFire*/, double opts_threshold,
    bool /*opts_smoothing_hits_given*/, int32_t /*opts_smoothing_hits*/,
    bool /*opts_smoothing_window_given*/, int32_t /*opts_smoothing_window*/,
    int32_t /*opts_refractoryMs*/,
    bool /*opts_device_given*/, const char* /*opts_device*/)
{
    s_wakeActive = true;
    s_wakeThreshold = opts_threshold;
}

void bro_wake_stop(void) {
    s_wakeActive = false;
}

void bro_wake_suspend(void) {
    s_wakeSuspended = true;
}

void bro_wake_resume(void) {
    s_wakeSuspended = false;
}

double bro_wake_lastScore(void) {
    return s_wakeLastScore;
}

bool bro_wake_isActive(void) {
    return s_wakeActive;
}

bool bro_wake_isSuspended(void) {
    return s_wakeSuspended;
}

bool bro_wake_isLoaded(void) {
    return s_wakeLoaded;
}

void bro_wake_setThreshold(double threshold) {
    s_wakeThreshold = threshold;
}

bool bro_wake_stats(void) {
    if (!s_wakeActive) return false;
    tl_nsStats = WakeStatsData{};
    return true;
}

double bro_wake_stats_framesDelivered(void) {
    return tl_nsStats.framesDelivered;
}

double bro_wake_stats_samplesDelivered(void) {
    return tl_nsStats.samplesDelivered;
}

double bro_wake_stats_rollingPeak(void) {
    return tl_nsStats.rollingPeak;
}

double bro_wake_stats_scoreMax(void) {
    return tl_nsStats.scoreMax;
}

} // extern "C"

namespace bro::bronze_host {
bool registerNatives_wake(std::string* error);
bool registerWakeNatives(std::string* error) {
    return registerNatives_wake(error);
}
} // namespace bro::bronze_host
