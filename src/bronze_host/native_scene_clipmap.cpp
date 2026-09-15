// native_scene_clipmap.cpp — ClipmapTerrain binding and C entry points.

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "natives/clipmap/native_clipmap_decl.h"
#include "scene/clipmap_terrain.h"
#include "scene/scene_node.h"

namespace bro::bronze_host {

bool registerNatives_clipmap(std::string* error);

extern "C" {
void* bro_scene_SceneGraph_createClipmapTerrain(void* self, bool, int32_t, bool, int32_t, bool, double, bool, double, bool, double, bool, double, bool, double, bool, double, bool, bool, bool, bool, bool, bool, bool, bool, bool, double, bool, double, bool, double, bool, int32_t);
void bro_clipmap_ClipmapTerrain_setHeightLayer(void* self, int32_t index, const float* data, uint32_t data_len, int32_t width, int32_t height, double originX, double originZ, double metresPerCell, bool wrapX, bool bandLimited);
void bro_clipmap_ClipmapTerrain_setChartCenter(void* self, bool has_xz, double x, double z);
void bro_clipmap_ClipmapTerrain_setSurfaceLayer(void* self, int32_t index, const float* data, uint32_t data_len, int32_t width, int32_t height, double originX, double originZ, double metresPerCell, int32_t components);
}

namespace {

static thread_local std::string tl_shaderSource;

}  // namespace

bool registerClipmapNatives(std::string* error) {
    if (!registerNatives_clipmap(error)) return false;
    bronze::embed::NativeSignature s;
    s.returnType = "__bro_native.clipmap.ClipmapTerrain";
    s.paramTypes = {"__bro_native.scene.SceneGraph", "bool", "i32", "bool", "i32", "bool", "f64", "bool", "f64",
                    "bool", "f64", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "bool", "bool", "bool",
                    "bool", "bool", "bool", "bool", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "i32"};
    s.kind = bronze::embed::NativeKind::Function;
    if (!bronze::embed::registerNative(
            "__bro_native.clipmap.createClipmapTerrain",
            reinterpret_cast<void*>(&bro_scene_SceneGraph_createClipmapTerrain),
            s, error)) return false;

    bronze::embed::NativeSignature sHl;
    sHl.returnType = "void";
    sHl.paramTypes = {"__bro_native.clipmap.ClipmapTerrain", "i32", "f32[]", "i32", "i32", "f64", "f64", "f64", "bool", "bool"};
    sHl.kind = bronze::embed::NativeKind::Function;
    if (!bronze::embed::registerNative(
            "__bro_native.clipmap.ClipmapTerrain_setHeightLayer",
            reinterpret_cast<void*>(&bro_clipmap_ClipmapTerrain_setHeightLayer),
            sHl, error)) return false;

    bronze::embed::NativeSignature sCc;
    sCc.returnType = "void";
    sCc.paramTypes = {"__bro_native.clipmap.ClipmapTerrain", "bool", "f64", "f64"};
    sCc.kind = bronze::embed::NativeKind::Function;
    if (!bronze::embed::registerNative(
            "__bro_native.clipmap.ClipmapTerrain_setChartCenter",
            reinterpret_cast<void*>(&bro_clipmap_ClipmapTerrain_setChartCenter),
            sCc, error)) return false;

    bronze::embed::NativeSignature sSl;
    sSl.returnType = "void";
    sSl.paramTypes = {"__bro_native.clipmap.ClipmapTerrain", "i32", "f32[]", "i32", "i32", "f64", "f64", "f64", "i32"};
    sSl.kind = bronze::embed::NativeKind::Function;
    return bronze::embed::registerNative(
        "__bro_native.clipmap.ClipmapTerrain_setSurfaceLayer",
        reinterpret_cast<void*>(&bro_clipmap_ClipmapTerrain_setSurfaceLayer),
        sSl, error);
}

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

void bro_clipmap_ClipmapTerrain_dtor(void* self) {
    auto* c = clipmapCellOf(self);
    if (c) {
        if (c->terrain) {
            c->terrain->destroy();
            c->terrain.reset();
        }
        delete c;
    }
}

void* bro_clipmap_ClipmapTerrain_ctor(void) {
    return new HostClipmapCell();
}

void* bro_clipmap_ClipmapTerrain_node_get(void* self) {
    auto* c = clipmapCellOf(self);
    if (!c || !c->clipmap()) return nullptr;
    auto* node = c->clipmap()->node();
    return node ? wrapNode(node, c->graph()) : nullptr;
}

int32_t bro_clipmap_ClipmapTerrain_levels_get(void* self) {
    auto* c = clipmapCellOf(self);
    return (c && c->clipmap()) ? c->clipmap()->config().levels : 0;
}

int32_t bro_clipmap_ClipmapTerrain_resolution_get(void* self) {
    auto* c = clipmapCellOf(self);
    return (c && c->clipmap()) ? c->clipmap()->config().resolution : 0;
}

double bro_clipmap_ClipmapTerrain_cellSize_get(void* self) {
    auto* c = clipmapCellOf(self);
    return (c && c->clipmap()) ? static_cast<double>(c->clipmap()->config().cellSize) : 0.0;
}

int32_t bro_clipmap_ClipmapTerrain_layerCount_get(void* self) {
    auto* c = clipmapCellOf(self);
    return (c && c->clipmap()) ? c->clipmap()->layerCount() : 0;
}

int32_t bro_clipmap_ClipmapTerrain_triangleCount_get(void* self) {
    auto* c = clipmapCellOf(self);
    return (c && c->clipmap()) ? c->clipmap()->triangleCount() : 0;
}

int32_t bro_clipmap_ClipmapTerrain_vertexCount_get(void* self) {
    auto* c = clipmapCellOf(self);
    return (c && c->clipmap()) ? c->clipmap()->vertexCount() : 0;
}

double bro_clipmap_ClipmapTerrain_farDistance_get(void* self) {
    auto* c = clipmapCellOf(self);
    return (c && c->clipmap()) ? static_cast<double>(c->clipmap()->farDistance()) : 0.0;
}

double bro_clipmap_ClipmapTerrain_cellScale_get(void* self) {
    auto* c = clipmapCellOf(self);
    return (c && c->clipmap()) ? static_cast<double>(c->clipmap()->cellScale()) : 1.0;
}

double bro_clipmap_ClipmapTerrain_planetRadius_get(void* self) {
    auto* c = clipmapCellOf(self);
    return (c && c->clipmap()) ? static_cast<double>(c->clipmap()->config().planetRadius) : 0.0;
}

void* bro_clipmap_ClipmapTerrain_setSnowLine(void* self, double m) {
    auto* c = clipmapCellOf(self);
    if (c && c->clipmap()) c->clipmap()->setSnowLine(static_cast<float>(m));
    return self;
}

void* bro_clipmap_ClipmapTerrain_setDetail(void* self, bool desc_wavelength_given, double desc_wavelength,
                                          bool desc_relief_given, double desc_relief,
                                          bool desc_gain_given, double desc_gain,
                                          bool desc_octaves_given, int32_t desc_octaves) {
    auto* c = clipmapCellOf(self);
    if (c && c->clipmap()) {
        float wl = desc_wavelength_given ? static_cast<float>(desc_wavelength) : 10.0f;
        float r = desc_relief_given ? static_cast<float>(desc_relief) : 1.0f;
        float g = desc_gain_given ? static_cast<float>(desc_gain) : 0.5f;
        int oct = desc_octaves_given ? desc_octaves : 4;
        c->clipmap()->setDetail(wl, r, g, oct);
    }
    return self;
}

void* bro_clipmap_ClipmapTerrain_setMaterials(void* self,
                                             const double* desc_rock_albedo, uint32_t desc_rock_albedo_len,
                                             bool desc_rock_roughness_given, double desc_rock_roughness,
                                             const double* desc_snow_albedo, uint32_t desc_snow_albedo_len,
                                             bool desc_snow_roughness_given, double desc_snow_roughness,
                                             const double* desc_sand_albedo, uint32_t desc_sand_albedo_len,
                                             bool desc_sand_roughness_given, double desc_sand_roughness,
                                             const double* desc_grass_albedo, uint32_t desc_grass_albedo_len,
                                             bool desc_grass_roughness_given, double desc_grass_roughness) {
    auto* c = clipmapCellOf(self);
    if (c && c->clipmap()) {
        float rockAlb[3] = {0.5f, 0.5f, 0.5f};
        if (desc_rock_albedo && desc_rock_albedo_len >= 3) {
            rockAlb[0] = static_cast<float>(desc_rock_albedo[0]);
            rockAlb[1] = static_cast<float>(desc_rock_albedo[1]);
            rockAlb[2] = static_cast<float>(desc_rock_albedo[2]);
        }
        float rockRough = desc_rock_roughness_given ? static_cast<float>(desc_rock_roughness) : 0.9f;

        float snowAlb[3] = {0.95f, 0.95f, 0.95f};
        if (desc_snow_albedo && desc_snow_albedo_len >= 3) {
            snowAlb[0] = static_cast<float>(desc_snow_albedo[0]);
            snowAlb[1] = static_cast<float>(desc_snow_albedo[1]);
            snowAlb[2] = static_cast<float>(desc_snow_albedo[2]);
        }
        float snowRough = desc_snow_roughness_given ? static_cast<float>(desc_snow_roughness) : 0.4f;

        float sandAlb[3] = {0.76f, 0.70f, 0.50f};
        if (desc_sand_albedo && desc_sand_albedo_len >= 3) {
            sandAlb[0] = static_cast<float>(desc_sand_albedo[0]);
            sandAlb[1] = static_cast<float>(desc_sand_albedo[1]);
            sandAlb[2] = static_cast<float>(desc_sand_albedo[2]);
        }
        float sandRough = desc_sand_roughness_given ? static_cast<float>(desc_sand_roughness) : 0.8f;

        float grassAlb[3] = {0.25f, 0.45f, 0.15f};
        if (desc_grass_albedo && desc_grass_albedo_len >= 3) {
            grassAlb[0] = static_cast<float>(desc_grass_albedo[0]);
            grassAlb[1] = static_cast<float>(desc_grass_albedo[1]);
            grassAlb[2] = static_cast<float>(desc_grass_albedo[2]);
        }
        float grassRough = desc_grass_roughness_given ? static_cast<float>(desc_grass_roughness) : 0.7f;

        c->clipmap()->setMaterials(rockAlb, rockRough, snowAlb, snowRough,
                                   sandAlb, sandRough, grassAlb, grassRough);
    }
    return self;
}

void* bro_clipmap_ClipmapTerrain_setForest(void* self, const double* desc_albedo, uint32_t desc_albedo_len,
                                          bool desc_strength_given, double desc_strength) {
    auto* c = clipmapCellOf(self);
    if (c && c->clipmap()) {
        float alb[3] = {0.1f, 0.3f, 0.1f};
        if (desc_albedo && desc_albedo_len >= 3) {
            alb[0] = static_cast<float>(desc_albedo[0]);
            alb[1] = static_cast<float>(desc_albedo[1]);
            alb[2] = static_cast<float>(desc_albedo[2]);
        }
        float str = desc_strength_given ? static_cast<float>(desc_strength) : 1.0f;
        c->clipmap()->setForest(alb, str);
    }
    return self;
}

void* bro_clipmap_ClipmapTerrain_update(void* self, double camX, double camY, double camZ) {
    auto* c = clipmapCellOf(self);
    if (c && c->clipmap()) {
        c->clipmap()->update(static_cast<float>(camX), static_cast<float>(camY), static_cast<float>(camZ));
    }
    return self;
}

const char* bro_clipmap_ClipmapTerrain_shaderSource(void* self, const char* stage) {
    auto* c = clipmapCellOf(self);
    if (!c || !c->clipmap() || !stage) return "";
    tl_shaderSource = c->clipmap()->shaderSource(stage);
    return tl_shaderSource.c_str();
}

double bro_clipmap_ClipmapTerrain_elevationAt(void* self, double x, double z) {
    auto* c = clipmapCellOf(self);
    return (c && c->clipmap()) ? static_cast<double>(c->clipmap()->elevationAt(static_cast<float>(x), static_cast<float>(z))) : 0.0;
}

double bro_clipmap_ClipmapTerrain_renderedElevationAt(void* self, double x, double z) {
    auto* c = clipmapCellOf(self);
    return (c && c->clipmap()) ? static_cast<double>(c->clipmap()->renderedElevationAt(static_cast<float>(x), static_cast<float>(z))) : 0.0;
}

double bro_clipmap_ClipmapTerrain_coverageDistance(void* self, double eyeAboveSeaLevel) {
    auto* c = clipmapCellOf(self);
    return (c && c->clipmap()) ? static_cast<double>(c->clipmap()->coverageDistance(static_cast<float>(eyeAboveSeaLevel))) : 0.0;
}

double bro_clipmap_ClipmapTerrain_horizonDistance(void* self, double eyeAboveSeaLevel) {
    auto* c = clipmapCellOf(self);
    return (c && c->clipmap()) ? static_cast<double>(c->clipmap()->horizonDistance(static_cast<float>(eyeAboveSeaLevel))) : 0.0;
}

void bro_clipmap_ClipmapTerrain_destroy(void* self) {
    auto* c = clipmapCellOf(self);
    if (c && c->terrain) {
        c->terrain->destroy();
        c->terrain.reset();
    }
}

void bro_clipmap_ClipmapTerrain_setHeightLayer(void* self, int32_t index, const float* data, uint32_t data_len,
                                               int32_t width, int32_t height, double originX, double originZ,
                                               double metresPerCell, bool wrapX, bool bandLimited) {
    auto* c = clipmapCellOf(self);
    if (!c || !c->clipmap()) return;
    if (index < 0 || index >= scene::ClipmapTerrain::kMaxLayers) return;
    if (width <= 0 || height <= 0 || !data || data_len < static_cast<uint32_t>(width * height)) {
        c->clipmap()->setHeightLayer(index, nullptr, 0, 0, 0, 0, 1);
        return;
    }
    c->clipmap()->setHeightLayer(index, data, width, height, static_cast<float>(originX),
                                 static_cast<float>(originZ), static_cast<float>(metresPerCell),
                                 wrapX, bandLimited);
}

void bro_clipmap_ClipmapTerrain_setChartCenter(void* self, bool has_xz, double x, double z) {
    auto* c = clipmapCellOf(self);
    if (!c || !c->clipmap()) return;
    if (has_xz) {
        c->clipmap()->setChartCenter(static_cast<float>(x), static_cast<float>(z));
    } else {
        c->clipmap()->clearChartCenter();
    }
}

void bro_clipmap_ClipmapTerrain_setSurfaceLayer(void* self, int32_t index, const float* data, uint32_t /*data_len*/,
                                                int32_t width, int32_t height, double originX, double originZ,
                                                double metresPerCell, int32_t components) {
    auto* c = clipmapCellOf(self);
    if (!c || !c->clipmap()) return;
    if (index < 0 || index >= scene::ClipmapTerrain::kMaxLayers) return;
    if (width <= 0 || height <= 0 || !data) {
        c->clipmap()->setSurfaceLayer(index, nullptr, 0, 0, 0, 0, 1, 4);
        return;
    }
    c->clipmap()->setSurfaceLayer(index, data, width, height, static_cast<float>(originX),
                                  static_cast<float>(originZ), static_cast<float>(metresPerCell),
                                  components > 0 ? components : 4);
}

}  // extern "C"
