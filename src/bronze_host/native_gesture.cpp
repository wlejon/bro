#include "natives/gesture/native_gesture_decl.h"
#include <vector>
#include <string>
#include <cstdint>

namespace {

struct BroGestureStreamViewImpl {
    bool active = false;
    int32_t sampleRate = 16000;
};

struct GestureInspectSlot {
    std::string name;
    std::string kind = "onset";
    double frameMs = 10.0;
    std::vector<double> intervalsMs;
    std::string onsetsJson = "[]";
    double toneHz = 0.0;
    double toneMs = 0.0;
    double toneSpread = 0.0;
};

static bool s_gestureActive = false;
static int32_t s_gestureSampleRate = 16000;
static thread_local GestureInspectSlot tl_viewInspectSlot;
static thread_local GestureInspectSlot tl_modInspectSlot;
static thread_local std::string tl_templatesJson = "[]";

} // namespace

extern "C" {

void bro_gesture_GestureStreamView_dtor(void* self) {
    delete static_cast<BroGestureStreamViewImpl*>(self);
}

void* bro_gesture_GestureStreamView_ctor(void) {
    return new BroGestureStreamViewImpl();
}

bool bro_gesture_GestureStreamView_active_get(void* self) {
    auto* v = static_cast<BroGestureStreamViewImpl*>(self);
    return v ? v->active : false;
}

int32_t bro_gesture_GestureStreamView_enrollFromAudio(void* /*self*/, const char* /*name*/,
                                                    const float* /*samples*/, uint32_t /*samples_len*/,
                                                    double /*policy_tempoTol*/, double /*policy_pitchTol*/,
                                                    double /*policy_pitchStabilityTol*/, double /*policy_shapeTol*/,
                                                    int32_t /*policy_refractoryFrames*/, int32_t /*policy_minOnsets*/,
                                                    int32_t /*policy_minToneFrames*/, int32_t /*policy_onsetSigFrames*/) {
    return 0;
}

bool bro_gesture_GestureStreamView_remove(void* /*self*/, const char* /*name*/) {
    return false;
}

void bro_gesture_GestureStreamView_clear(void* /*self*/) {}

const char* bro_gesture_GestureStreamView_templates(void* /*self*/) {
    tl_templatesJson = "[]";
    return tl_templatesJson.c_str();
}

bool bro_gesture_GestureStreamView_inspect(void* /*self*/, const char* name) {
    tl_viewInspectSlot.name = name ? name : "";
    tl_viewInspectSlot.kind = "onset";
    tl_viewInspectSlot.frameMs = 10.0;
    tl_viewInspectSlot.intervalsMs.clear();
    tl_viewInspectSlot.onsetsJson = "[]";
    tl_viewInspectSlot.toneHz = 0.0;
    tl_viewInspectSlot.toneMs = 0.0;
    tl_viewInspectSlot.toneSpread = 0.0;
    return true;
}

const char* bro_gesture_GestureStreamView_inspect_name(void) {
    return tl_viewInspectSlot.name.c_str();
}

const char* bro_gesture_GestureStreamView_inspect_kind(void) {
    return tl_viewInspectSlot.kind.c_str();
}

double bro_gesture_GestureStreamView_inspect_frameMs(void) {
    return tl_viewInspectSlot.frameMs;
}

void bro_gesture_GestureStreamView_inspect_intervalsMs(bronze_native_buffer* out) {
    if (!out) return;
    out->data = tl_viewInspectSlot.intervalsMs.data();
    out->length = static_cast<uint32_t>(tl_viewInspectSlot.intervalsMs.size());
    out->release = nullptr;
    out->ctx = nullptr;
}

const char* bro_gesture_GestureStreamView_inspect_onsets(void) {
    return tl_viewInspectSlot.onsetsJson.c_str();
}

double bro_gesture_GestureStreamView_inspect_toneHz(void) {
    return tl_viewInspectSlot.toneHz;
}

double bro_gesture_GestureStreamView_inspect_toneMs(void) {
    return tl_viewInspectSlot.toneMs;
}

double bro_gesture_GestureStreamView_inspect_toneSpread(void) {
    return tl_viewInspectSlot.toneSpread;
}

void bro_gesture_GestureStreamView_reset(void* /*self*/) {}

void bro_gesture_GestureStreamView_listen(void* self, uint64_t /*opts_onGesture*/) {
    auto* v = static_cast<BroGestureStreamViewImpl*>(self);
    if (v) v->active = true;
}

void bro_gesture_GestureStreamView_stop(void* self) {
    auto* v = static_cast<BroGestureStreamViewImpl*>(self);
    if (v) v->active = false;
}

bool bro_gesture_GestureStreamView_isActive(void* self) {
    auto* v = static_cast<BroGestureStreamViewImpl*>(self);
    return v ? v->active : false;
}

int32_t bro_gesture_GestureStreamView_sampleRate(void* self) {
    auto* v = static_cast<BroGestureStreamViewImpl*>(self);
    return v ? v->sampleRate : 16000;
}

#if BRO_WITH_SOUNDML
void bro_gesture_init(void) {}

int32_t bro_gesture_enrollFromAudio(const char* /*name*/, const float* /*samples*/, uint32_t /*samples_len*/,
                                   double /*policy_tempoTol*/, double /*policy_pitchTol*/,
                                   double /*policy_pitchStabilityTol*/, double /*policy_shapeTol*/,
                                   int32_t /*policy_refractoryFrames*/, int32_t /*policy_minOnsets*/,
                                   int32_t /*policy_minToneFrames*/, int32_t /*policy_onsetSigFrames*/) {
    return 0;
}

bool bro_gesture_remove(const char* /*name*/) {
    return false;
}

void bro_gesture_clear(void) {}

const char* bro_gesture_templates(void) {
    tl_templatesJson = "[]";
    return tl_templatesJson.c_str();
}

bool bro_gesture_inspect(const char* name) {
    tl_modInspectSlot.name = name ? name : "";
    tl_modInspectSlot.kind = "onset";
    tl_modInspectSlot.frameMs = 10.0;
    tl_modInspectSlot.intervalsMs.clear();
    tl_modInspectSlot.onsetsJson = "[]";
    tl_modInspectSlot.toneHz = 0.0;
    tl_modInspectSlot.toneMs = 0.0;
    tl_modInspectSlot.toneSpread = 0.0;
    return true;
}

const char* bro_gesture_inspect_name(void) {
    return tl_modInspectSlot.name.c_str();
}

const char* bro_gesture_inspect_kind(void) {
    return tl_modInspectSlot.kind.c_str();
}

double bro_gesture_inspect_frameMs(void) {
    return tl_modInspectSlot.frameMs;
}

void bro_gesture_inspect_intervalsMs(bronze_native_buffer* out) {
    if (!out) return;
    out->data = tl_modInspectSlot.intervalsMs.data();
    out->length = static_cast<uint32_t>(tl_modInspectSlot.intervalsMs.size());
    out->release = nullptr;
    out->ctx = nullptr;
}

const char* bro_gesture_inspect_onsets(void) {
    return tl_modInspectSlot.onsetsJson.c_str();
}

double bro_gesture_inspect_toneHz(void) {
    return tl_modInspectSlot.toneHz;
}

double bro_gesture_inspect_toneMs(void) {
    return tl_modInspectSlot.toneMs;
}

double bro_gesture_inspect_toneSpread(void) {
    return tl_modInspectSlot.toneSpread;
}

void bro_gesture_reset(void) {}

void bro_gesture_listen(uint64_t /*opts_onGesture*/) {
    s_gestureActive = true;
}

void bro_gesture_stop(void) {
    s_gestureActive = false;
}

bool bro_gesture_isActive(void) {
    return s_gestureActive;
}

int32_t bro_gesture_sampleRate(void) {
    return s_gestureSampleRate;
}
#endif

} // extern "C"

namespace bro::bronze_host {
bool registerNatives_gesture(std::string* error);
bool registerGestureNatives(std::string* error) {
    return registerNatives_gesture(error);
}
} // namespace bro::bronze_host
