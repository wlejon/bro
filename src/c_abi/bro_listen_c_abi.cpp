// =============================================================================
// bro_listen_c_abi.cpp — C++ forwarding implementations for bro.listen
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_listen_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"

extern "C" {

static bool s_fallback_listen_supported = true;
static int32_t s_fallback_listen_seconds = 0;
static int64_t s_fallback_listen_frame = 0;

void* bro_listen_open(void* source) {
    const auto* b = bro_get_listen_bridge();
    if (b && b->open) return b->open(source);
    return nullptr;
}

bool bro_listen_supported(void) {
    const auto* b = bro_get_listen_bridge();
    if (b && b->supported) return b->supported();
    return s_fallback_listen_supported;
}

void* bro_listen_apps(void) {
    const auto* b = bro_get_listen_bridge();
    if (b && b->apps) return b->apps();
    return nullptr;
}

void bro_listen_retain(int32_t seconds) {
    const auto* b = bro_get_listen_bridge();
    if (b && b->retain) {
        b->retain(seconds);
    } else {
        s_fallback_listen_seconds = seconds;
    }
}

void* bro_listen_audio(int64_t startFrame, int64_t endFrame) {
    const auto* b = bro_get_listen_bridge();
    if (b && b->audio) return b->audio(startFrame, endFrame);
    return nullptr;
}

int64_t bro_listen_frame(void) {
    const auto* b = bro_get_listen_bridge();
    if (b && b->frame) return b->frame();
    return s_fallback_listen_frame;
}

void* bro_listen_info(void) {
    const auto* b = bro_get_listen_bridge();
    if (b && b->info) return b->info();
    return nullptr;
}

// ListenStream
void* bro_ListenStream_create(void) { return nullptr; }
void bro_ListenStream_destroy(void* /*self*/) {}
uint32_t bro_ListenStream_get_id(void* /*self*/) { return 0; }
const char* bro_ListenStream_get_kind(void* /*self*/) { return "mic"; }
bool bro_ListenStream_get_valid(void* /*self*/) { return false; }
void* bro_ListenStream_get_wake(void* /*self*/) { return nullptr; }
void* bro_ListenStream_get_kws(void* /*self*/) { return nullptr; }
void* bro_ListenStream_get_sense(void* /*self*/) { return nullptr; }
void* bro_ListenStream_get_gesture(void* /*self*/) { return nullptr; }
void bro_ListenStream_retain(void* /*self*/, int32_t /*seconds*/) {}
void* bro_ListenStream_audio(void* /*self*/, int64_t /*startFrame*/, int64_t /*endFrame*/) { return nullptr; }
int64_t bro_ListenStream_frame(void* /*self*/) { return 0; }
void* bro_ListenStream_info(void* /*self*/) { return nullptr; }
void bro_ListenStream_feed(void* /*self*/, void* /*samples*/) {}
void bro_ListenStream_close(void* /*self*/) {}

} // extern "C"
