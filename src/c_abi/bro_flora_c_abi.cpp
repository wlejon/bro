// =============================================================================
// bro_flora_c_abi.cpp — C++ forwarding implementations for bro.flora
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_flora_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <cstdint>

struct BroFloraWorldImpl {
    double simTime = 0.0;
    int32_t plantCount = 0;
    int32_t prototypeCount = 0;
    int32_t moduleCount = 0;
};

static double s_windStrength = 0.0;
static double s_windDirX = 1.0;
static double s_windDirY = 0.0;
static double s_density = 1.0;

extern "C" {

// --- Namespace bro.flora ---
void* bro_flora_createWorld(void* opts) {
    const auto* b = bro_get_flora_bridge();
    if (b && b->createWorld) return b->createWorld(opts);
    return new BroFloraWorldImpl();
}

void* bro_flora_leafCluster(void* /*phyllotaxy*/, void* /*opts*/) {
    return nullptr;
}

void bro_flora_setWind(double strength, double dirX, double dirY) {
    const auto* b = bro_get_flora_bridge();
    if (b && b->setWind) {
        b->setWind(strength, dirX, dirY);
        return;
    }
    s_windStrength = strength;
    s_windDirX = dirX;
    s_windDirY = dirY;
}

void bro_flora_wind(double strength, double dirX, double dirY) {
    bro_flora_setWind(strength, dirX, dirY);
}

void bro_flora_setDensity(double density) {
    const auto* b = bro_get_flora_bridge();
    if (b && b->setDensity) {
        b->setDensity(density);
        return;
    }
    s_density = density;
}

void bro_flora_density(double density) {
    bro_flora_setDensity(density);
}

void bro_flora_update(double dt) {
    const auto* b = bro_get_flora_bridge();
    if (b && b->update) {
        b->update(dt);
    }
}

void bro_flora_clear(void) {
    const auto* b = bro_get_flora_bridge();
    if (b && b->clear) {
        b->clear();
    }
}

void bro_flora_placement(void* config) {
    const auto* b = bro_get_flora_bridge();
    if (b && b->placement) {
        b->placement(config);
    }
}

void bro_flora_addPlacement(void* config) {
    bro_flora_placement(config);
}

void* bro_flora_batches(void) {
    const auto* b = bro_get_flora_bridge();
    if (b && b->batches) return b->batches();
    return nullptr;
}

void* bro_flora_getBatches(void) {
    return bro_flora_batches();
}

// --- Interface bro.flora.FloraWorld ---
void* bro_FloraWorld_create(void) {
    return new BroFloraWorldImpl();
}

void bro_FloraWorld_destroy(void* self) {
    delete static_cast<BroFloraWorldImpl*>(self);
}

int32_t bro_FloraWorld_addPrototype(void* self, void* /*spec*/) {
    auto* w = static_cast<BroFloraWorldImpl*>(self);
    return w ? w->prototypeCount++ : 0;
}

void* bro_FloraWorld_addVoronoiSite(void* self, int32_t /*prototypeIndex*/, double /*determinacy*/, double /*apicalControl*/) {
    return self;
}

int32_t bro_FloraWorld_addPlant(void* self, void* /*spec*/) {
    auto* w = static_cast<BroFloraWorldImpl*>(self);
    return w ? w->plantCount++ : 0;
}

bool bro_FloraWorld_removePlant(void* self, int32_t /*plantIdx*/) {
    auto* w = static_cast<BroFloraWorldImpl*>(self);
    if (w && w->plantCount > 0) {
        w->plantCount--;
        return true;
    }
    return false;
}

void* bro_FloraWorld_step(void* self, double dt) {
    auto* w = static_cast<BroFloraWorldImpl*>(self);
    if (w) w->simTime += dt;
    return self;
}

void* bro_FloraWorld_plantInfo(void* /*self*/, int32_t /*plantIdx*/) {
    return nullptr;
}

void* bro_FloraWorld_setClimate(void* self, void* /*opts*/) {
    return self;
}

double bro_FloraWorld_sampleShadow(void* /*self*/, void* /*pos*/) {
    return 1.0;
}

const char* bro_FloraWorld_validate(void* /*self*/) {
    return nullptr;
}

void* bro_FloraWorld_emitMesh(void* /*self*/, int32_t /*sides*/) {
    return nullptr;
}

void* bro_FloraWorld_emitSegments(void* /*self*/) {
    return nullptr;
}

void* bro_FloraWorld_emitFoliage(void* /*self*/) {
    return nullptr;
}

void* bro_FloraWorld_emitBloomAnchors(void* /*self*/) {
    return nullptr;
}

double bro_FloraWorld_get_simTime(void* self) {
    auto* w = static_cast<BroFloraWorldImpl*>(self);
    return w ? w->simTime : 0.0;
}

int32_t bro_FloraWorld_get_plantCount(void* self) {
    auto* w = static_cast<BroFloraWorldImpl*>(self);
    return w ? w->plantCount : 0;
}

int32_t bro_FloraWorld_get_prototypeCount(void* self) {
    auto* w = static_cast<BroFloraWorldImpl*>(self);
    return w ? w->prototypeCount : 0;
}

int32_t bro_FloraWorld_get_moduleCount(void* self) {
    auto* w = static_cast<BroFloraWorldImpl*>(self);
    return w ? w->moduleCount : 0;
}

} // extern "C"
