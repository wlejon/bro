#include "natives/kws/native_kws_decl.h"
#include <cstdint>
#include <string>
#include <vector>

namespace {

struct KwsStatsData {
    double framesDelivered = 0.0;
    double samplesDelivered = 0.0;
    double rollingPeak = 0.0;
};

struct KwsInspectionData {
    std::string name;
    double threshold = 0.40;
    double frameMs = 10.0;
    bool hasGaps = false;
    std::string states = "[]";
};

struct KwsProgressData {
    double frames = 0.0;
    double generation = 0.0;
    std::string templates = "[]";
};

struct KwsPosteriorData {
    double frame = 0.0;
    std::string top = "[]";
};

struct BroKwsStreamViewImpl {
    bool active = false;
    bool suspended = false;
    bool loaded = false;
    int32_t sampleRate = 16000;
    double prefixProgress = 0.0;
    std::vector<std::string> templateNames;
};

static bool s_kwsLoaded = false;
static bool s_kwsActive = false;
static bool s_kwsSuspended = false;
static int32_t s_kwsSampleRate = 0;
static double s_kwsPrefixProgress = 0.0;
static std::vector<std::string> s_kwsTemplates;

static thread_local KwsStatsData tl_viewStats;
static thread_local KwsStatsData tl_nsStats;
static thread_local KwsInspectionData tl_viewInspection;
static thread_local KwsInspectionData tl_nsInspection;
static thread_local KwsProgressData tl_viewProgress;
static thread_local KwsProgressData tl_nsProgress;
static thread_local KwsPosteriorData tl_viewPosterior;
static thread_local KwsPosteriorData tl_nsPosterior;
static thread_local std::string tl_viewTemplatesJson = "[]";
static thread_local std::string tl_nsTemplatesJson = "[]";

} // namespace

extern "C" {

void bro_kws_KwsStreamView_dtor(void* self) {
    delete static_cast<BroKwsStreamViewImpl*>(self);
}

void* bro_kws_KwsStreamView_ctor(void) {
    return new BroKwsStreamViewImpl();
}

bool bro_kws_KwsStreamView_active_get(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    return v ? v->active : false;
}

int32_t bro_kws_KwsStreamView_enroll(
    void* self, const char* name,
    const int32_t* /*phonemeIds*/, uint32_t phonemeIds_len,
    bool /*policy_weights_given*/, const char* /*policy_weights*/,
    bool /*policy_device_given*/, const char* /*policy_device*/,
    double /*policy_threshold*/, int32_t /*policy_refractoryMs*/,
    bool /*policy_smoothing_hits_given*/, int32_t /*policy_smoothing_hits*/,
    bool /*policy_smoothing_window_given*/, int32_t /*policy_smoothing_window*/,
    int32_t /*policy_minPhonemes*/, int32_t /*policy_entrySilenceFrames*/,
    double /*policy_emissionFloor*/, double /*policy_minCoverage*/,
    double /*policy_scoreNorm*/, bool /*policy_enrollGaps*/,
    int32_t /*policy_gapMinFrames*/, double /*policy_gapTolerance*/)
{
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    if (v && name) v->templateNames.push_back(name);
    return static_cast<int32_t>(phonemeIds_len);
}

int32_t bro_kws_KwsStreamView_enrollFromAudio(
    void* self, const char* name,
    const float* /*samples*/, uint32_t samples_len,
    bool /*policy_weights_given*/, const char* /*policy_weights*/,
    bool /*policy_device_given*/, const char* /*policy_device*/,
    double /*policy_threshold*/, int32_t /*policy_refractoryMs*/,
    bool /*policy_smoothing_hits_given*/, int32_t /*policy_smoothing_hits*/,
    bool /*policy_smoothing_window_given*/, int32_t /*policy_smoothing_window*/,
    int32_t /*policy_minPhonemes*/, int32_t /*policy_entrySilenceFrames*/,
    double /*policy_emissionFloor*/, double /*policy_minCoverage*/,
    double /*policy_scoreNorm*/, bool /*policy_enrollGaps*/,
    int32_t /*policy_gapMinFrames*/, double /*policy_gapTolerance*/)
{
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    if (v && name) v->templateNames.push_back(name);
    return static_cast<int32_t>(samples_len / 160);
}

int32_t bro_kws_KwsStreamView_enrollFromClasses(
    void* self, const char* name,
    const int32_t* /*classIds*/, uint32_t classIds_len,
    bool /*policy_weights_given*/, const char* /*policy_weights*/,
    bool /*policy_device_given*/, const char* /*policy_device*/,
    double /*policy_threshold*/, int32_t /*policy_refractoryMs*/,
    bool /*policy_smoothing_hits_given*/, int32_t /*policy_smoothing_hits*/,
    bool /*policy_smoothing_window_given*/, int32_t /*policy_smoothing_window*/,
    int32_t /*policy_minPhonemes*/, int32_t /*policy_entrySilenceFrames*/,
    double /*policy_emissionFloor*/, double /*policy_minCoverage*/,
    double /*policy_scoreNorm*/, bool /*policy_enrollGaps*/,
    int32_t /*policy_gapMinFrames*/, double /*policy_gapTolerance*/)
{
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    if (v && name) v->templateNames.push_back(name);
    return static_cast<int32_t>(classIds_len);
}

bool bro_kws_KwsStreamView_inspect(void* /*self*/, const char* /*name*/) {
    return false;
}

const char* bro_kws_KwsStreamView_inspect_name(void) {
    return tl_viewInspection.name.c_str();
}

double bro_kws_KwsStreamView_inspect_threshold(void) {
    return tl_viewInspection.threshold;
}

double bro_kws_KwsStreamView_inspect_frameMs(void) {
    return tl_viewInspection.frameMs;
}

bool bro_kws_KwsStreamView_inspect_hasGaps(void) {
    return tl_viewInspection.hasGaps;
}

const char* bro_kws_KwsStreamView_inspect_states(void) {
    return tl_viewInspection.states.c_str();
}

bool bro_kws_KwsStreamView_remove(void* self, const char* name) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    if (!v || !name) return false;
    for (auto it = v->templateNames.begin(); it != v->templateNames.end(); ++it) {
        if (*it == name) {
            v->templateNames.erase(it);
            return true;
        }
    }
    return false;
}

void bro_kws_KwsStreamView_clear(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    if (v) v->templateNames.clear();
}

const char* bro_kws_KwsStreamView_templates(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    if (!v || v->templateNames.empty()) {
        tl_viewTemplatesJson = "[]";
    } else {
        std::string j = "[";
        for (size_t i = 0; i < v->templateNames.size(); ++i) {
            if (i > 0) j += ",";
            j += "\"" + v->templateNames[i] + "\"";
        }
        j += "]";
        tl_viewTemplatesJson = std::move(j);
    }
    return tl_viewTemplatesJson.c_str();
}

void bro_kws_KwsStreamView_reset(void* /*self*/) {}

void bro_kws_KwsStreamView_listen(void* self, uint64_t /*opts_onSpot*/) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    if (v) v->active = true;
}

void bro_kws_KwsStreamView_stop(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    if (v) v->active = false;
}

void bro_kws_KwsStreamView_suspend(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    if (v) v->suspended = true;
}

void bro_kws_KwsStreamView_resume(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    if (v) v->suspended = false;
}

bool bro_kws_KwsStreamView_isActive(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    return v ? v->active : false;
}

bool bro_kws_KwsStreamView_isSuspended(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    return v ? v->suspended : false;
}

bool bro_kws_KwsStreamView_isLoaded(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    return v ? v->loaded : false;
}

int32_t bro_kws_KwsStreamView_sampleRate(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    return v ? v->sampleRate : 16000;
}

double bro_kws_KwsStreamView_prefixProgress(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    return v ? v->prefixProgress : 0.0;
}

bool bro_kws_KwsStreamView_progress(void* /*self*/) {
    return false;
}

double bro_kws_KwsStreamView_progress_frames(void) {
    return tl_viewProgress.frames;
}

double bro_kws_KwsStreamView_progress_generation(void) {
    return tl_viewProgress.generation;
}

const char* bro_kws_KwsStreamView_progress_templates(void) {
    return tl_viewProgress.templates.c_str();
}

bool bro_kws_KwsStreamView_posterior(void* /*self*/, int32_t /*topK*/) {
    return false;
}

double bro_kws_KwsStreamView_posterior_frame(void) {
    return tl_viewPosterior.frame;
}

const char* bro_kws_KwsStreamView_posterior_top(void) {
    return tl_viewPosterior.top.c_str();
}

bool bro_kws_KwsStreamView_stats(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    if (!v || !v->active) return false;
    tl_viewStats = KwsStatsData{};
    return true;
}

double bro_kws_KwsStreamView_stats_framesDelivered(void) {
    return tl_viewStats.framesDelivered;
}

double bro_kws_KwsStreamView_stats_samplesDelivered(void) {
    return tl_viewStats.samplesDelivered;
}

double bro_kws_KwsStreamView_stats_rollingPeak(void) {
    return tl_viewStats.rollingPeak;
}

// --- Namespace bro.kws ---

void bro_kws_init(void) {}

void bro_kws_load(
    bool /*opts_weights_given*/, const char* /*opts_weights*/,
    bool /*opts_device_given*/, const char* /*opts_device*/,
    double /*opts_threshold*/, int32_t /*opts_refractoryMs*/,
    bool /*opts_smoothing_hits_given*/, int32_t /*opts_smoothing_hits*/,
    bool /*opts_smoothing_window_given*/, int32_t /*opts_smoothing_window*/,
    int32_t /*opts_minPhonemes*/, int32_t /*opts_entrySilenceFrames*/,
    double /*opts_emissionFloor*/, double /*opts_minCoverage*/,
    double /*opts_scoreNorm*/, bool /*opts_enrollGaps*/,
    int32_t /*opts_gapMinFrames*/, double /*opts_gapTolerance*/)
{
    s_kwsLoaded = true;
    s_kwsSampleRate = 16000;
}

void bro_kws_unload(void) {
    s_kwsLoaded = false;
    s_kwsActive = false;
    s_kwsSuspended = false;
    s_kwsSampleRate = 0;
    s_kwsTemplates.clear();
}

int32_t bro_kws_enroll(
    const char* name,
    const int32_t* /*phonemeIds*/, uint32_t phonemeIds_len,
    bool /*policy_weights_given*/, const char* /*policy_weights*/,
    bool /*policy_device_given*/, const char* /*policy_device*/,
    double /*policy_threshold*/, int32_t /*policy_refractoryMs*/,
    bool /*policy_smoothing_hits_given*/, int32_t /*policy_smoothing_hits*/,
    bool /*policy_smoothing_window_given*/, int32_t /*policy_smoothing_window*/,
    int32_t /*policy_minPhonemes*/, int32_t /*policy_entrySilenceFrames*/,
    double /*policy_emissionFloor*/, double /*policy_minCoverage*/,
    double /*policy_scoreNorm*/, bool /*policy_enrollGaps*/,
    int32_t /*policy_gapMinFrames*/, double /*policy_gapTolerance*/)
{
    if (name) s_kwsTemplates.push_back(name);
    return static_cast<int32_t>(phonemeIds_len);
}

int32_t bro_kws_enrollFromAudio(
    const char* name,
    const float* /*samples*/, uint32_t samples_len,
    bool /*policy_weights_given*/, const char* /*policy_weights*/,
    bool /*policy_device_given*/, const char* /*policy_device*/,
    double /*policy_threshold*/, int32_t /*policy_refractoryMs*/,
    bool /*policy_smoothing_hits_given*/, int32_t /*policy_smoothing_hits*/,
    bool /*policy_smoothing_window_given*/, int32_t /*policy_smoothing_window*/,
    int32_t /*policy_minPhonemes*/, int32_t /*policy_entrySilenceFrames*/,
    double /*policy_emissionFloor*/, double /*policy_minCoverage*/,
    double /*policy_scoreNorm*/, bool /*policy_enrollGaps*/,
    int32_t /*policy_gapMinFrames*/, double /*policy_gapTolerance*/)
{
    if (name) s_kwsTemplates.push_back(name);
    return static_cast<int32_t>(samples_len / 160);
}

int32_t bro_kws_enrollFromClasses(
    const char* name,
    const int32_t* /*classIds*/, uint32_t classIds_len,
    bool /*policy_weights_given*/, const char* /*policy_weights*/,
    bool /*policy_device_given*/, const char* /*policy_device*/,
    double /*policy_threshold*/, int32_t /*policy_refractoryMs*/,
    bool /*policy_smoothing_hits_given*/, int32_t /*policy_smoothing_hits*/,
    bool /*policy_smoothing_window_given*/, int32_t /*policy_smoothing_window*/,
    int32_t /*policy_minPhonemes*/, int32_t /*policy_entrySilenceFrames*/,
    double /*policy_emissionFloor*/, double /*policy_minCoverage*/,
    double /*policy_scoreNorm*/, bool /*policy_enrollGaps*/,
    int32_t /*policy_gapMinFrames*/, double /*policy_gapTolerance*/)
{
    if (name) s_kwsTemplates.push_back(name);
    return static_cast<int32_t>(classIds_len);
}

bool bro_kws_inspect(const char* /*name*/) {
    return false;
}

const char* bro_kws_inspect_name(void) {
    return tl_nsInspection.name.c_str();
}

double bro_kws_inspect_threshold(void) {
    return tl_nsInspection.threshold;
}

double bro_kws_inspect_frameMs(void) {
    return tl_nsInspection.frameMs;
}

bool bro_kws_inspect_hasGaps(void) {
    return tl_nsInspection.hasGaps;
}

const char* bro_kws_inspect_states(void) {
    return tl_nsInspection.states.c_str();
}

bool bro_kws_remove(const char* name) {
    if (!name) return false;
    for (auto it = s_kwsTemplates.begin(); it != s_kwsTemplates.end(); ++it) {
        if (*it == name) {
            s_kwsTemplates.erase(it);
            return true;
        }
    }
    return false;
}

void bro_kws_clear(void) {
    s_kwsTemplates.clear();
}

const char* bro_kws_templates(void) {
    if (s_kwsTemplates.empty()) {
        tl_nsTemplatesJson = "[]";
    } else {
        std::string j = "[";
        for (size_t i = 0; i < s_kwsTemplates.size(); ++i) {
            if (i > 0) j += ",";
            j += "\"" + s_kwsTemplates[i] + "\"";
        }
        j += "]";
        tl_nsTemplatesJson = std::move(j);
    }
    return tl_nsTemplatesJson.c_str();
}

void bro_kws_reset(void) {}

void bro_kws_listen(uint64_t /*opts_onSpot*/) {
    s_kwsActive = true;
}

void bro_kws_stop(void) {
    s_kwsActive = false;
}

void bro_kws_suspend(void) {
    s_kwsSuspended = true;
}

void bro_kws_resume(void) {
    s_kwsSuspended = false;
}

bool bro_kws_isActive(void) {
    return s_kwsActive;
}

bool bro_kws_isSuspended(void) {
    return s_kwsSuspended;
}

bool bro_kws_isLoaded(void) {
    return s_kwsLoaded;
}

int32_t bro_kws_sampleRate(void) {
    return s_kwsSampleRate;
}

double bro_kws_prefixProgress(void) {
    return s_kwsPrefixProgress;
}

bool bro_kws_progress(void) {
    return false;
}

double bro_kws_progress_frames(void) {
    return tl_nsProgress.frames;
}

double bro_kws_progress_generation(void) {
    return tl_nsProgress.generation;
}

const char* bro_kws_progress_templates(void) {
    return tl_nsProgress.templates.c_str();
}

bool bro_kws_posterior(int32_t /*topK*/) {
    return false;
}

double bro_kws_posterior_frame(void) {
    return tl_nsPosterior.frame;
}

const char* bro_kws_posterior_top(void) {
    return tl_nsPosterior.top.c_str();
}

bool bro_kws_stats(void) {
    if (!s_kwsActive) return false;
    tl_nsStats = KwsStatsData{};
    return true;
}

double bro_kws_stats_framesDelivered(void) {
    return tl_nsStats.framesDelivered;
}

double bro_kws_stats_samplesDelivered(void) {
    return tl_nsStats.samplesDelivered;
}

double bro_kws_stats_rollingPeak(void) {
    return tl_nsStats.rollingPeak;
}

} // extern "C"

namespace bro::bronze_host {
bool registerNatives_kws(std::string* error);
bool registerKwsNatives(std::string* error) {
    return registerNatives_kws(error);
}
} // namespace bro::bronze_host
