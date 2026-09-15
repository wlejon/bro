#include "natives/sense/native_sense_decl.h"
#include <vector>
#include <cstdint>

namespace {

struct BroSenseStreamViewImpl {
    bool active = false;
    int32_t sampleRate = 16000;
};

struct SenseSnapshotData {
    double frames = 0.0;
    double t = 0.0;
    double rms = 0.0;
    double peak = 0.0;
    double db = -100.0;
    bool voice = false;
    double noiseFloorDb = -60.0;
    double snrDb = 0.0;
    double voiceFrames = 0.0;
    double voiceEvents = 0.0;
    double lastVoiceFrame = 0.0;
    double flux = 0.0;
    bool onset = false;
    double onsets = 0.0;
    double lastOnsetFrame = 0.0;
    double periodicity = 0.0;
    double dominantHz = 0.0;
    bool tonal = false;
    double tonalFrames = 0.0;
    double tonalEvents = 0.0;
    double lastTonalFrame = 0.0;
    double centroid = 0.0;
};

struct SenseStatsData {
    double framesDelivered = 0.0;
    double samplesDelivered = 0.0;
    double rollingPeak = 0.0;
};

struct SenseAnalysisData {
    double frames = 0.0;
    double hop = 160.0;
    double win = 320.0;
    double rate = 16000.0;
    double frameMs = 10.0;
    std::vector<float> db;
    std::vector<float> dominantHz;
    std::vector<float> periodicity;
    std::vector<float> centroid;
    std::vector<int32_t> flags;
};

static bool s_senseActive = false;
static int32_t s_senseSampleRate = 16000;

static thread_local SenseSnapshotData tl_viewSnapshot;
static thread_local SenseStatsData    tl_viewStats;
static thread_local SenseSnapshotData tl_viewFeed;
static thread_local SenseAnalysisData tl_viewAnalyze;

static thread_local SenseSnapshotData tl_modSnapshot;
static thread_local SenseStatsData    tl_modStats;
static thread_local SenseSnapshotData tl_modFeed;
static thread_local SenseAnalysisData tl_modAnalyze;

} // namespace

extern "C" {

void bro_sense_SenseStreamView_dtor(void* self) {
    delete static_cast<BroSenseStreamViewImpl*>(self);
}

void* bro_sense_SenseStreamView_ctor(void) {
    return new BroSenseStreamViewImpl();
}

bool bro_sense_SenseStreamView_active_get(void* self) {
    auto* v = static_cast<BroSenseStreamViewImpl*>(self);
    return v ? v->active : false;
}

void bro_sense_SenseStreamView_start(void* self, double /*opts_vadFloorDb*/, double /*opts_vadSnrDb*/,
                                     double /*opts_vadRiseDbps*/, int32_t /*opts_vadHangFrames*/,
                                     double /*opts_onsetRatio*/, double /*opts_onsetAbs*/,
                                     double /*opts_onsetEma*/, int32_t /*opts_onsetRefractoryFrames*/,
                                     double /*opts_tonalMinPeriodicity*/, double /*opts_tonalFminHz*/,
                                     double /*opts_tonalFmaxHz*/) {
    auto* v = static_cast<BroSenseStreamViewImpl*>(self);
    if (v) v->active = true;
}

void bro_sense_SenseStreamView_stop(void* self) {
    auto* v = static_cast<BroSenseStreamViewImpl*>(self);
    if (v) v->active = false;
}

bool bro_sense_SenseStreamView_isActive(void* self) {
    auto* v = static_cast<BroSenseStreamViewImpl*>(self);
    return v ? v->active : false;
}

bool bro_sense_SenseStreamView_snapshot(void* /*self*/) {
    tl_viewSnapshot = SenseSnapshotData{};
    return true;
}

#define DEF_SNAPSHOT_GETTERS(PREFIX, SLOT) \
double PREFIX##_frames(void) { return SLOT.frames; } \
double PREFIX##_t(void) { return SLOT.t; } \
double PREFIX##_rms(void) { return SLOT.rms; } \
double PREFIX##_peak(void) { return SLOT.peak; } \
double PREFIX##_db(void) { return SLOT.db; } \
bool   PREFIX##_voice(void) { return SLOT.voice; } \
double PREFIX##_noiseFloorDb(void) { return SLOT.noiseFloorDb; } \
double PREFIX##_snrDb(void) { return SLOT.snrDb; } \
double PREFIX##_voiceFrames(void) { return SLOT.voiceFrames; } \
double PREFIX##_voiceEvents(void) { return SLOT.voiceEvents; } \
double PREFIX##_lastVoiceFrame(void) { return SLOT.lastVoiceFrame; } \
double PREFIX##_flux(void) { return SLOT.flux; } \
bool   PREFIX##_onset(void) { return SLOT.onset; } \
double PREFIX##_onsets(void) { return SLOT.onsets; } \
double PREFIX##_lastOnsetFrame(void) { return SLOT.lastOnsetFrame; } \
double PREFIX##_periodicity(void) { return SLOT.periodicity; } \
double PREFIX##_dominantHz(void) { return SLOT.dominantHz; } \
bool   PREFIX##_tonal(void) { return SLOT.tonal; } \
double PREFIX##_tonalFrames(void) { return SLOT.tonalFrames; } \
double PREFIX##_tonalEvents(void) { return SLOT.tonalEvents; } \
double PREFIX##_lastTonalFrame(void) { return SLOT.lastTonalFrame; } \
double PREFIX##_centroid(void) { return SLOT.centroid; }

DEF_SNAPSHOT_GETTERS(bro_sense_SenseStreamView_snapshot, tl_viewSnapshot)

int32_t bro_sense_SenseStreamView_sampleRate(void* self) {
    auto* v = static_cast<BroSenseStreamViewImpl*>(self);
    return v ? v->sampleRate : 16000;
}

bool bro_sense_SenseStreamView_stats(void* /*self*/) {
    tl_viewStats = SenseStatsData{};
    return true;
}

double bro_sense_SenseStreamView_stats_framesDelivered(void) { return tl_viewStats.framesDelivered; }
double bro_sense_SenseStreamView_stats_samplesDelivered(void) { return tl_viewStats.samplesDelivered; }
double bro_sense_SenseStreamView_stats_rollingPeak(void) { return tl_viewStats.rollingPeak; }

bool bro_sense_SenseStreamView_feed(void* /*self*/, const float* /*samples*/, uint32_t /*samples_len*/) {
    tl_viewFeed = SenseSnapshotData{};
    return true;
}

DEF_SNAPSHOT_GETTERS(bro_sense_SenseStreamView_feed, tl_viewFeed)

bool bro_sense_SenseStreamView_analyze(void* /*self*/, const float* /*samples*/, uint32_t /*samples_len*/,
                                      double /*opts_vadFloorDb*/, double /*opts_vadSnrDb*/,
                                      double /*opts_vadRiseDbps*/, int32_t /*opts_vadHangFrames*/,
                                      double /*opts_onsetRatio*/, double /*opts_onsetAbs*/,
                                      double /*opts_onsetEma*/, int32_t /*opts_onsetRefractoryFrames*/,
                                      double /*opts_tonalMinPeriodicity*/, double /*opts_tonalFminHz*/,
                                      double /*opts_tonalFmaxHz*/) {
    tl_viewAnalyze.frames = 0.0;
    tl_viewAnalyze.hop = 160.0;
    tl_viewAnalyze.win = 320.0;
    tl_viewAnalyze.rate = 16000.0;
    tl_viewAnalyze.frameMs = 10.0;
    tl_viewAnalyze.db.clear();
    tl_viewAnalyze.dominantHz.clear();
    tl_viewAnalyze.periodicity.clear();
    tl_viewAnalyze.centroid.clear();
    tl_viewAnalyze.flags.clear();
    return true;
}

#define DEF_ANALYZE_GETTERS(PREFIX, SLOT) \
double PREFIX##_frames(void) { return SLOT.frames; } \
double PREFIX##_hop(void) { return SLOT.hop; } \
double PREFIX##_win(void) { return SLOT.win; } \
double PREFIX##_rate(void) { return SLOT.rate; } \
double PREFIX##_frameMs(void) { return SLOT.frameMs; } \
void PREFIX##_db(bronze_native_buffer* out) { \
    if (!out) return; \
    out->data = SLOT.db.data(); \
    out->length = static_cast<uint32_t>(SLOT.db.size()); \
    out->release = nullptr; \
    out->ctx = nullptr; \
} \
void PREFIX##_dominantHz(bronze_native_buffer* out) { \
    if (!out) return; \
    out->data = SLOT.dominantHz.data(); \
    out->length = static_cast<uint32_t>(SLOT.dominantHz.size()); \
    out->release = nullptr; \
    out->ctx = nullptr; \
} \
void PREFIX##_periodicity(bronze_native_buffer* out) { \
    if (!out) return; \
    out->data = SLOT.periodicity.data(); \
    out->length = static_cast<uint32_t>(SLOT.periodicity.size()); \
    out->release = nullptr; \
    out->ctx = nullptr; \
} \
void PREFIX##_centroid(bronze_native_buffer* out) { \
    if (!out) return; \
    out->data = SLOT.centroid.data(); \
    out->length = static_cast<uint32_t>(SLOT.centroid.size()); \
    out->release = nullptr; \
    out->ctx = nullptr; \
} \
void PREFIX##_flags(bronze_native_buffer* out) { \
    if (!out) return; \
    out->data = SLOT.flags.data(); \
    out->length = static_cast<uint32_t>(SLOT.flags.size()); \
    out->release = nullptr; \
    out->ctx = nullptr; \
}

DEF_ANALYZE_GETTERS(bro_sense_SenseStreamView_analyze, tl_viewAnalyze)

#if BRO_WITH_SOUNDML
void bro_sense_init(void) {}

void bro_sense_start(double /*opts_vadFloorDb*/, double /*opts_vadSnrDb*/,
                     double /*opts_vadRiseDbps*/, int32_t /*opts_vadHangFrames*/,
                     double /*opts_onsetRatio*/, double /*opts_onsetAbs*/,
                     double /*opts_onsetEma*/, int32_t /*opts_onsetRefractoryFrames*/,
                     double /*opts_tonalMinPeriodicity*/, double /*opts_tonalFminHz*/,
                     double /*opts_tonalFmaxHz*/) {
    s_senseActive = true;
}

void bro_sense_stop(void) {
    s_senseActive = false;
}

bool bro_sense_isActive(void) {
    return s_senseActive;
}

bool bro_sense_snapshot(void) {
    tl_modSnapshot = SenseSnapshotData{};
    return true;
}

DEF_SNAPSHOT_GETTERS(bro_sense_snapshot, tl_modSnapshot)

int32_t bro_sense_sampleRate(void) {
    return s_senseSampleRate;
}

bool bro_sense_stats(void) {
    tl_modStats = SenseStatsData{};
    return true;
}

double bro_sense_stats_framesDelivered(void) { return tl_modStats.framesDelivered; }
double bro_sense_stats_samplesDelivered(void) { return tl_modStats.samplesDelivered; }
double bro_sense_stats_rollingPeak(void) { return tl_modStats.rollingPeak; }

bool bro_sense_feed(const float* /*samples*/, uint32_t /*samples_len*/) {
    tl_modFeed = SenseSnapshotData{};
    return true;
}

DEF_SNAPSHOT_GETTERS(bro_sense_feed, tl_modFeed)

bool bro_sense_analyze(const float* /*samples*/, uint32_t /*samples_len*/,
                       double /*opts_vadFloorDb*/, double /*opts_vadSnrDb*/,
                       double /*opts_vadRiseDbps*/, int32_t /*opts_vadHangFrames*/,
                       double /*opts_onsetRatio*/, double /*opts_onsetAbs*/,
                       double /*opts_onsetEma*/, int32_t /*opts_onsetRefractoryFrames*/,
                       double /*opts_tonalMinPeriodicity*/, double /*opts_tonalFminHz*/,
                       double /*opts_tonalFmaxHz*/) {
    tl_modAnalyze.frames = 0.0;
    tl_modAnalyze.hop = 160.0;
    tl_modAnalyze.win = 320.0;
    tl_modAnalyze.rate = 16000.0;
    tl_modAnalyze.frameMs = 10.0;
    tl_modAnalyze.db.clear();
    tl_modAnalyze.dominantHz.clear();
    tl_modAnalyze.periodicity.clear();
    tl_modAnalyze.centroid.clear();
    tl_modAnalyze.flags.clear();
    return true;
}

DEF_ANALYZE_GETTERS(bro_sense_analyze, tl_modAnalyze)
#endif

} // extern "C"

namespace bro::bronze_host {
bool registerNatives_sense(std::string* error);
bool registerSenseNatives(std::string* error) {
    return registerNatives_sense(error);
}
} // namespace bro::bronze_host
