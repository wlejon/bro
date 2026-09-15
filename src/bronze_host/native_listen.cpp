#include "natives/listen/native_listen_decl.h"
#include "natives/wake/native_wake_decl.h"
#include "natives/kws/native_kws_decl.h"
#include "natives/sense/native_sense_decl.h"
#include "natives/gesture/native_gesture_decl.h"
#include <cstdint>
#include <string>
#include <vector>

namespace {

struct ListenRetentionData {
    bool active = false;
    int32_t seconds = 0;
    int32_t rate = 16000;
    int32_t hop = 160;
    double frameRate = 100.0;
    double streamFrame = 0.0;
    double heldFrames = 0.0;
    double heldSeconds = 0.0;
};

struct BroListenStreamImpl {
    uint32_t id = 1;
    std::string kind = "mic";
    bool valid = true;
    ListenRetentionData retention;
    void* wake = nullptr;
    void* kws = nullptr;
    void* sense = nullptr;
    void* gesture = nullptr;

    BroListenStreamImpl() {
        wake = bro_wake_WakeStreamView_ctor();
        kws = bro_kws_KwsStreamView_ctor();
        sense = bro_sense_SenseStreamView_ctor();
        gesture = bro_gesture_GestureStreamView_ctor();
    }

    ~BroListenStreamImpl() {
        if (wake) bro_wake_WakeStreamView_dtor(wake);
        if (kws) bro_kws_KwsStreamView_dtor(kws);
        if (sense) bro_sense_SenseStreamView_dtor(sense);
        if (gesture) bro_gesture_GestureStreamView_dtor(gesture);
    }
};

static uint32_t s_nextStreamId = 1;
static bool s_listenSupported = true;
static ListenRetentionData s_globalRetention;

static thread_local ListenRetentionData tl_streamRetention;
static thread_local ListenRetentionData tl_globalRetention;
static thread_local std::vector<std::pair<double, std::string>> tl_apps;

} // namespace

extern "C" {

void bro_listen_ListenStream_dtor(void* self) {
    delete static_cast<BroListenStreamImpl*>(self);
}

void* bro_listen_ListenStream_ctor(void) {
    auto* s = new BroListenStreamImpl();
    s->id = s_nextStreamId++;
    return s;
}

double bro_listen_ListenStream_id_get(void* self) {
    auto* s = static_cast<BroListenStreamImpl*>(self);
    return s ? static_cast<double>(s->id) : 0.0;
}

const char* bro_listen_ListenStream_kind_get(void* self) {
    auto* s = static_cast<BroListenStreamImpl*>(self);
    return s ? s->kind.c_str() : "mic";
}

bool bro_listen_ListenStream_valid_get(void* self) {
    auto* s = static_cast<BroListenStreamImpl*>(self);
    return s ? s->valid : false;
}

void* bro_listen_ListenStream_wake_get(void* self) {
    auto* s = static_cast<BroListenStreamImpl*>(self);
    return s ? s->wake : nullptr;
}

void* bro_listen_ListenStream_kws_get(void* self) {
    auto* s = static_cast<BroListenStreamImpl*>(self);
    return s ? s->kws : nullptr;
}

void* bro_listen_ListenStream_sense_get(void* self) {
    auto* s = static_cast<BroListenStreamImpl*>(self);
    return s ? s->sense : nullptr;
}

void* bro_listen_ListenStream_gesture_get(void* self) {
    auto* s = static_cast<BroListenStreamImpl*>(self);
    return s ? s->gesture : nullptr;
}

void bro_listen_ListenStream_retain(void* self, int32_t seconds) {
    auto* s = static_cast<BroListenStreamImpl*>(self);
    if (s) {
        s->retention.seconds = seconds;
        s->retention.active = (seconds > 0);
        if (s->retention.active) {
            s->retention.heldFrames = (seconds * s->retention.rate) / s->retention.hop;
            s->retention.heldSeconds = seconds;
        } else {
            s->retention.heldFrames = 0.0;
            s->retention.heldSeconds = 0.0;
        }
    }
}

double bro_listen_ListenStream_frame(void* self) {
    auto* s = static_cast<BroListenStreamImpl*>(self);
    return s ? s->retention.streamFrame : 0.0;
}

void bro_listen_ListenStream_info(void* self) {
    auto* s = static_cast<BroListenStreamImpl*>(self);
    if (s) {
        tl_streamRetention = s->retention;
    } else {
        tl_streamRetention = ListenRetentionData{};
    }
}

bool bro_listen_ListenStream_info_active(void) {
    return tl_streamRetention.active;
}

int32_t bro_listen_ListenStream_info_seconds(void) {
    return tl_streamRetention.seconds;
}

int32_t bro_listen_ListenStream_info_rate(void) {
    return tl_streamRetention.rate;
}

int32_t bro_listen_ListenStream_info_hop(void) {
    return tl_streamRetention.hop;
}

double bro_listen_ListenStream_info_frameRate(void) {
    return tl_streamRetention.frameRate;
}

double bro_listen_ListenStream_info_streamFrame(void) {
    return tl_streamRetention.streamFrame;
}

double bro_listen_ListenStream_info_heldFrames(void) {
    return tl_streamRetention.heldFrames;
}

double bro_listen_ListenStream_info_heldSeconds(void) {
    return tl_streamRetention.heldSeconds;
}

void bro_listen_ListenStream_feed(void* self, const float* /*samples*/, uint32_t samples_len) {
    auto* s = static_cast<BroListenStreamImpl*>(self);
    if (s && s->valid) {
        s->retention.streamFrame += static_cast<double>(samples_len) / s->retention.hop;
    }
}

void bro_listen_ListenStream_close(void* self) {
    auto* s = static_cast<BroListenStreamImpl*>(self);
    if (s) s->valid = false;
}

// --- Namespace bro.listen ---

bool bro_listen_supported(void) {
    return s_listenSupported;
}

int32_t bro_listen_apps(void) {
    tl_apps.clear();
    return static_cast<int32_t>(tl_apps.size());
}

double bro_listen_apps_pid(int32_t index) {
    if (index >= 0 && index < static_cast<int32_t>(tl_apps.size())) {
        return tl_apps[index].first;
    }
    return 0.0;
}

const char* bro_listen_apps_name(int32_t index) {
    if (index >= 0 && index < static_cast<int32_t>(tl_apps.size())) {
        return tl_apps[index].second.c_str();
    }
    return "";
}

void bro_listen_retain(int32_t seconds) {
    s_globalRetention.seconds = seconds;
    s_globalRetention.active = (seconds > 0);
    if (s_globalRetention.active) {
        s_globalRetention.heldFrames = (seconds * s_globalRetention.rate) / s_globalRetention.hop;
        s_globalRetention.heldSeconds = seconds;
    } else {
        s_globalRetention.heldFrames = 0.0;
        s_globalRetention.heldSeconds = 0.0;
    }
}

double bro_listen_frame(void) {
    return s_globalRetention.streamFrame;
}

void bro_listen_info(void) {
    tl_globalRetention = s_globalRetention;
}

bool bro_listen_info_active(void) {
    return tl_globalRetention.active;
}

int32_t bro_listen_info_seconds(void) {
    return tl_globalRetention.seconds;
}

int32_t bro_listen_info_rate(void) {
    return tl_globalRetention.rate;
}

int32_t bro_listen_info_hop(void) {
    return tl_globalRetention.hop;
}

double bro_listen_info_frameRate(void) {
    return tl_globalRetention.frameRate;
}

double bro_listen_info_streamFrame(void) {
    return tl_globalRetention.streamFrame;
}

double bro_listen_info_heldFrames(void) {
    return tl_globalRetention.heldFrames;
}

double bro_listen_info_heldSeconds(void) {
    return tl_globalRetention.heldSeconds;
}

} // extern "C"

namespace bro::bronze_host {
bool registerNatives_listen(std::string* error);
bool registerListenNatives(std::string* error) {
    return registerNatives_listen(error);
}
} // namespace bro::bronze_host
