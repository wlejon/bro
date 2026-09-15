// native_flora.cpp — Bronze native entry points for bro.flora
#include "natives/flora/native_flora_decl.h"

#include <cstdint>

namespace {

struct BroFloraWorldImpl {
    double simTime = 0.0;
    int32_t plantCount = 0;
    int32_t prototypeCount = 0;
    int32_t moduleCount = 0;
};

#if BRO_WITH_FLORA
static double s_windStrength = 0.0;
static double s_windDirX = 0.0;
static double s_windDirY = 0.0;
static double s_density = 1.0;
#endif

}  // namespace

extern "C" {

void bro_flora_FloraWorld_dtor(void* self) {
    delete static_cast<BroFloraWorldImpl*>(self);
}

void* bro_flora_FloraWorld_ctor(void) {
    return new BroFloraWorldImpl();
}

void* bro_flora_FloraWorld_addVoronoiSite(void* self, int32_t /*prototypeIndex*/, double /*determinacy*/, double /*apicalControl*/) {
    return self;
}

bool bro_flora_FloraWorld_removePlant(void* self, int32_t plantIdx) {
    if (!self || plantIdx < 0) return false;
    auto* w = static_cast<BroFloraWorldImpl*>(self);
    if (w->plantCount > 0 && plantIdx < w->plantCount) {
        w->plantCount--;
        return true;
    }
    return false;
}

void* bro_flora_FloraWorld_step(void* self, double dt) {
    if (self) {
        static_cast<BroFloraWorldImpl*>(self)->simTime += dt;
    }
    return self;
}

double bro_flora_FloraWorld_simTime_get(void* self) {
    return self ? static_cast<BroFloraWorldImpl*>(self)->simTime : 0.0;
}

int32_t bro_flora_FloraWorld_plantCount_get(void* self) {
    return self ? static_cast<BroFloraWorldImpl*>(self)->plantCount : 0;
}

int32_t bro_flora_FloraWorld_prototypeCount_get(void* self) {
    return self ? static_cast<BroFloraWorldImpl*>(self)->prototypeCount : 0;
}

int32_t bro_flora_FloraWorld_moduleCount_get(void* self) {
    return self ? static_cast<BroFloraWorldImpl*>(self)->moduleCount : 0;
}

#if BRO_WITH_FLORA
void bro_flora_setWind(double strength, double dirX, double dirY) {
    s_windStrength = strength;
    s_windDirX = dirX;
    s_windDirY = dirY;
}

void bro_flora_wind(double strength, double dirX, double dirY) {
    bro_flora_setWind(strength, dirX, dirY);
}

void bro_flora_setDensity(double density) {
    s_density = density;
}

void bro_flora_density(double density) {
    bro_flora_setDensity(density);
}

void bro_flora_update(double /*dt*/) {
}

void bro_flora_clear(void) {
}
#endif

}  // extern "C"

namespace bro::bronze_host {
bool registerFloraNatives(std::string* error) {
    extern bool registerNatives_flora(std::string* error);
    return registerNatives_flora(error);
}
}
