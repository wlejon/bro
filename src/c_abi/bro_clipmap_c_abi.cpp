// =============================================================================
// bro_clipmap_c_abi.cpp — C++ forwarding implementations for bro.clipmap
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_clipmap_c_abi.h"
#include <cstdint>
#include <string>

struct BroClipmapTerrainImpl {
    int32_t levels = 6;
    int32_t resolution = 64;
    double cellSize = 1.0;
    int32_t layerCount = 1;
    int32_t triangleCount = 8192;
    int32_t vertexCount = 4096;
    double farDistance = 1000.0;
    double cellScale = 1.0;
    double planetRadius = 0.0;
    double snowLine = 1200.0;
    double chartCenterX = 0.0;
    double chartCenterZ = 0.0;
    double camX = 0.0, camY = 0.0, camZ = 0.0;
};

extern "C" {

void* bro_ClipmapTerrain_create(void) {
    return new BroClipmapTerrainImpl();
}

void bro_ClipmapTerrain_destroy(void* self) {
    delete static_cast<BroClipmapTerrainImpl*>(self);
}

void* bro_ClipmapTerrain_get_node(void* /*self*/) {
    return nullptr;
}

int32_t bro_ClipmapTerrain_get_levels(void* self) {
    auto* c = static_cast<BroClipmapTerrainImpl*>(self);
    return c ? c->levels : 0;
}

int32_t bro_ClipmapTerrain_get_resolution(void* self) {
    auto* c = static_cast<BroClipmapTerrainImpl*>(self);
    return c ? c->resolution : 0;
}

double bro_ClipmapTerrain_get_cellSize(void* self) {
    auto* c = static_cast<BroClipmapTerrainImpl*>(self);
    return c ? c->cellSize : 0.0;
}

int32_t bro_ClipmapTerrain_get_layerCount(void* self) {
    auto* c = static_cast<BroClipmapTerrainImpl*>(self);
    return c ? c->layerCount : 0;
}

int32_t bro_ClipmapTerrain_get_triangleCount(void* self) {
    auto* c = static_cast<BroClipmapTerrainImpl*>(self);
    return c ? c->triangleCount : 0;
}

int32_t bro_ClipmapTerrain_get_vertexCount(void* self) {
    auto* c = static_cast<BroClipmapTerrainImpl*>(self);
    return c ? c->vertexCount : 0;
}

double bro_ClipmapTerrain_get_farDistance(void* self) {
    auto* c = static_cast<BroClipmapTerrainImpl*>(self);
    return c ? c->farDistance : 0.0;
}

double bro_ClipmapTerrain_get_cellScale(void* self) {
    auto* c = static_cast<BroClipmapTerrainImpl*>(self);
    return c ? c->cellScale : 1.0;
}

double bro_ClipmapTerrain_get_planetRadius(void* self) {
    auto* c = static_cast<BroClipmapTerrainImpl*>(self);
    return c ? c->planetRadius : 0.0;
}

void* bro_ClipmapTerrain_setHeightLayer(void* self, int32_t /*index*/, void* /*desc*/) {
    return self;
}

void* bro_ClipmapTerrain_setSnowLine(void* self, double m) {
    auto* c = static_cast<BroClipmapTerrainImpl*>(self);
    if (c) c->snowLine = m;
    return self;
}

void* bro_ClipmapTerrain_setChartCenter(void* self, double x, double z) {
    auto* c = static_cast<BroClipmapTerrainImpl*>(self);
    if (c) { c->chartCenterX = x; c->chartCenterZ = z; }
    return self;
}

void* bro_ClipmapTerrain_setDetail(void* self, void* /*desc*/) {
    return self;
}

void* bro_ClipmapTerrain_setMaterials(void* self, void* /*desc*/) {
    return self;
}

void* bro_ClipmapTerrain_setForest(void* self, void* /*desc*/) {
    return self;
}

void* bro_ClipmapTerrain_setSurfaceLayer(void* self, void* /*indexOrDesc*/, void* /*desc*/) {
    return self;
}

void* bro_ClipmapTerrain_update(void* self, double camX, double camY, double camZ) {
    auto* c = static_cast<BroClipmapTerrainImpl*>(self);
    if (c) { c->camX = camX; c->camY = camY; c->camZ = camZ; }
    return self;
}

const char* bro_ClipmapTerrain_shaderSource(void* /*self*/, const char* /*stage*/) {
    return "// clipmap shader source";
}

double bro_ClipmapTerrain_elevationAt(void* /*self*/, double /*x*/, double /*z*/) {
    return 0.0;
}

double bro_ClipmapTerrain_renderedElevationAt(void* /*self*/, double /*x*/, double /*z*/) {
    return 0.0;
}

double bro_ClipmapTerrain_coverageDistance(void* self, double /*eyeAboveSeaLevel*/) {
    auto* c = static_cast<BroClipmapTerrainImpl*>(self);
    return c ? c->farDistance : 1000.0;
}

double bro_ClipmapTerrain_horizonDistance(void* self, double /*eyeAboveSeaLevel*/) {
    auto* c = static_cast<BroClipmapTerrainImpl*>(self);
    return c ? c->farDistance * 2.0 : 2000.0;
}

} // extern "C"
