// =============================================================================
// bro_diar_c_abi.cpp — C++ forwarding implementations for bro.diar
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_diar_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <cstdint>
#include <string>

struct BroSortformerImpl {
    std::string device = "CPU";
    bool busy = false;
};

struct BroSortformerSessionImpl {
    std::string device = "CPU";
    bool busy = false;
};

struct BroClusterDiarizerImpl {
    std::string device = "CPU";
    bool busy = false;
};

extern "C" {

// --- Interface bro.diar.Sortformer ---
void* bro_Sortformer_create(void) {
    return new BroSortformerImpl();
}

void bro_Sortformer_destroy(void* self) {
    delete static_cast<BroSortformerImpl*>(self);
}

const char* bro_Sortformer_get_device(void* self) {
    auto* s = static_cast<BroSortformerImpl*>(self);
    return s ? s->device.c_str() : "CPU";
}

bool bro_Sortformer_get_busy(void* self) {
    auto* s = static_cast<BroSortformerImpl*>(self);
    return s ? s->busy : false;
}

void* bro_Sortformer_diarize(void* /*self*/, void* /*audio*/) {
    return nullptr;
}

void* bro_Sortformer_createSession(void* /*self*/) {
    return new BroSortformerSessionImpl();
}

// --- Interface bro.diar.SortformerSession ---
void* bro_SortformerSession_create(void) {
    return new BroSortformerSessionImpl();
}

void bro_SortformerSession_destroy(void* self) {
    delete static_cast<BroSortformerSessionImpl*>(self);
}

const char* bro_SortformerSession_get_device(void* self) {
    auto* s = static_cast<BroSortformerSessionImpl*>(self);
    return s ? s->device.c_str() : "CPU";
}

bool bro_SortformerSession_get_busy(void* self) {
    auto* s = static_cast<BroSortformerSessionImpl*>(self);
    return s ? s->busy : false;
}

void* bro_SortformerSession_feed(void* /*self*/, void* /*audio*/, bool /*isLast*/) {
    return nullptr;
}

void bro_SortformerSession_reset(void* /*self*/) {
}

// --- Interface bro.diar.ClusterDiarizer ---
void* bro_ClusterDiarizer_create(void) {
    return new BroClusterDiarizerImpl();
}

void bro_ClusterDiarizer_destroy(void* self) {
    delete static_cast<BroClusterDiarizerImpl*>(self);
}

const char* bro_ClusterDiarizer_get_device(void* self) {
    auto* c = static_cast<BroClusterDiarizerImpl*>(self);
    return c ? c->device.c_str() : "CPU";
}

bool bro_ClusterDiarizer_get_busy(void* self) {
    auto* c = static_cast<BroClusterDiarizerImpl*>(self);
    return c ? c->busy : false;
}

void* bro_ClusterDiarizer_diarize(void* /*self*/, void* /*audio*/, void* /*opts*/) {
    return nullptr;
}

// --- Namespace bro.diar ---
void bro_diar_init(void) {
    const auto* b = bro_get_diar_bridge();
    if (b && b->init) b->init();
}

void* bro_diar_loadSortformer(const char* modelDir, void* opts) {
    const auto* b = bro_get_diar_bridge();
    if (b && b->loadSortformer) return b->loadSortformer(modelDir, opts);
    return new BroSortformerImpl();
}

void* bro_diar_diarize(void* model, void* audio, void* opts) {
    const auto* b = bro_get_diar_bridge();
    if (b && b->diarize) return b->diarize(model, audio, opts);
    return nullptr;
}

void* bro_diar_loadClusterDiarizer(const char* embeddingDir, const char* vadDir, void* opts) {
    const auto* b = bro_get_diar_bridge();
    if (b && b->loadClusterDiarizer) return b->loadClusterDiarizer(embeddingDir, vadDir, opts);
    return new BroClusterDiarizerImpl();
}

void* bro_diar_clusterDiarize(void* model, void* audio, void* opts) {
    const auto* b = bro_get_diar_bridge();
    if (b && b->clusterDiarize) return b->clusterDiarize(model, audio, opts);
    return nullptr;
}

}
