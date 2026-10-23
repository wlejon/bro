// native_scene_core.cpp — SceneGraph lifecycle, rendering, settings, camera, and raycasting.

#include "bronze_host/native_scene_internal.h"
#include "natives/tile_world/native_tile_world_decl.h"
#include "bronze_host/host_natives.h"
#include "bronze_host/host_js_modules.h"
#include "natives/scene/native_scene_decl.h"
#include "util/asset_path.h"
#include <bromesh/analysis/raycast.h>
#include <bromesh/analysis/bvh.h>
#include <bromesh/primitives/primitives.h>
#include <bromesh/manipulation/normals.h>
#include <broimage/decode.h>

#include "engine/scene_audio_sync.h"
#include <string_view>

namespace bro::bronze_host {

Value createSceneGraphValue(scene::SceneGraph* sg, dom::Element* canvas) {
    if (!sg) return ev::null();
    // The prototype chain js/scene*.js give SceneGraph objects, which may
    // not have run yet (installDeferredModules).
    ensure3DModules();
    auto* cell = new HostSceneGraphCell();
    cell->token = sg->livenessToken();
    cell->canvas = canvas;
    Value val = ev::wrapNative(cell, "__bro_native.scene.SceneGraph");
    if (ev::isNull(val)) {
        delete cell;
    }
    return val;
}

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

// --- SceneNode lifecycle ----------------------------------------------------

void bro_scene_SceneNode_dtor(void* self) {
    auto* cell = static_cast<HostSceneNodeCell*>(self);
    if (cell && cell->tag == kHostSceneNodeTag) {
        delete cell;
    }
}

void* bro_scene_SceneNode_ctor(void) {
    return nullptr;
}

// --- SceneGraph lifecycle ---------------------------------------------------

void bro_scene_SceneGraph_dtor(void* self) {
    auto* cell = static_cast<HostSceneGraphCell*>(self);
    if (cell && cell->tag == kHostSceneGraphTag) {
        delete cell;
    }
}

void* bro_scene_SceneGraph_ctor(void) {
    return nullptr;
}

void* bro_scene_SceneGraph_root_get(void* self) {
    auto* g = graphOf(self);
    return g ? wrapNode(g->root(), g) : nullptr;
}

bool bro_scene_SceneGraph_showLightIcons_get(void* self) {
    auto* g = graphOf(self);
    return g ? g->showLightIcons() : false;
}

void bro_scene_SceneGraph_showLightIcons_set(void* self, bool v) {
    auto* g = graphOf(self);
    if (g) g->setShowLightIcons(v);
}

bool bro_scene_SceneGraph_frustumCulling_get(void* self) {
    auto* g = graphOf(self);
    return g ? g->frustumCulling() : true;
}

void bro_scene_SceneGraph_frustumCulling_set(void* self, bool v) {
    auto* g = graphOf(self);
    if (g) g->setFrustumCulling(v);
}

double bro_scene_SceneGraph_renderScale_get(void* self) {
    auto* g = graphOf(self);
    return g ? g->renderScale() : 1.0;
}

void bro_scene_SceneGraph_renderScale_set(void* self, double v) {
    auto* g = graphOf(self);
    if (g) g->setRenderScale(static_cast<float>(v));
}

double bro_scene_SceneGraph_msaa_get(void* self) {
    auto* g = graphOf(self);
    return g ? g->msaa() : 1;
}

void bro_scene_SceneGraph_msaa_set(void* self, double v) {
    auto* g = graphOf(self);
    if (g) g->setMSAA(satCast<int>(v));
}

void* bro_scene_SceneGraph_createNode(void* self, bool opts_name_given, const char* opts_name,
                                     const double* opts_position, uint32_t opts_position_len,
                                     const double* opts_rotation, uint32_t opts_rotation_len,
                                     const double* opts_scale, uint32_t opts_scale_len,
                                     bool opts_visible_given, bool opts_visible) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    std::string name = (opts_name_given && opts_name) ? opts_name : "";
    auto* node = g->createNode(name);
    g->root()->addChild(node);
    if (opts_position && opts_position_len >= 3) {
        node->setPosition(static_cast<float>(opts_position[0]),
                          static_cast<float>(opts_position[1]),
                          static_cast<float>(opts_position[2]));
    }
    if (opts_rotation && opts_rotation_len >= 4) {
        node->setRotation(bromath::Quat(static_cast<float>(opts_rotation[0]),
                                        static_cast<float>(opts_rotation[1]),
                                        static_cast<float>(opts_rotation[2]),
                                        static_cast<float>(opts_rotation[3])));
    }
    if (opts_scale && opts_scale_len >= 3) {
        node->setScale(static_cast<float>(opts_scale[0]),
                       static_cast<float>(opts_scale[1]),
                       static_cast<float>(opts_scale[2]));
    }
    if (opts_visible_given) node->setVisible(opts_visible);
    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createHtmlNode(void* self, bool opts_html_given, const char* opts_html,
                                         bool opts_width_given, double opts_width,
                                         bool opts_height_given, double opts_height,
                                         const double* opts_position, uint32_t opts_position_len,
                                         const double* opts_rotation, uint32_t opts_rotation_len,
                                         const double* opts_scale, uint32_t opts_scale_len,
                                         bool opts_visible_given, bool opts_visible) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createHtml();
    g->root()->addChild(node);
    float w = opts_width_given ? static_cast<float>(opts_width) : 200.0f;
    float h = opts_height_given ? static_cast<float>(opts_height) : 50.0f;
    node->setLayoutSize(w, h);
    if (opts_html_given && opts_html) node->setHtml(opts_html);
    if (opts_position && opts_position_len >= 3) {
        node->setPosition(static_cast<float>(opts_position[0]),
                          static_cast<float>(opts_position[1]),
                          static_cast<float>(opts_position[2]));
    }
    // rotation: a quaternion [x,y,z,w], or a bare number = Z rotation (the
    // SceneNode.rotation setter's two spellings). scale: [x,y,z] or uniform.
    if (opts_rotation && opts_rotation_len >= 4) {
        node->setRotation(bromath::Quat(static_cast<float>(opts_rotation[0]),
                                        static_cast<float>(opts_rotation[1]),
                                        static_cast<float>(opts_rotation[2]),
                                        static_cast<float>(opts_rotation[3])));
    } else if (opts_rotation && opts_rotation_len == 1) {
        auto e = node->rotationEuler();
        node->setRotationEuler(e.x, e.y, static_cast<float>(opts_rotation[0]));
    }
    if (opts_scale && opts_scale_len >= 3) {
        node->setScale(static_cast<float>(opts_scale[0]),
                       static_cast<float>(opts_scale[1]),
                       static_cast<float>(opts_scale[2]));
    } else if (opts_scale && opts_scale_len == 1) {
        const float s = static_cast<float>(opts_scale[0]);
        node->setScale(s, s, s);
    }
    if (opts_visible_given) node->setVisible(opts_visible);
    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createLight(void* self, bool opts_type_given, const char* opts_type,
                                      const double* opts_color, uint32_t opts_color_len,
                                      bool opts_intensity_given, double opts_intensity,
                                      bool opts_range_given, double opts_range,
                                      bool opts_innerCone_given, double opts_innerCone,
                                      bool opts_outerCone_given, double opts_outerCone,
                                      bool opts_castShadow_given, bool opts_castShadow,
                                      const double* opts_position, uint32_t opts_position_len,
                                      const double* opts_rotation, uint32_t opts_rotation_len) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createLight();
    g->root()->addChild(node);
    if (opts_type_given && opts_type) {
        std::string t = opts_type;
        if (t == "directional") node->setKind(scene::LightNode::Kind::Directional);
        else if (t == "spot")   node->setKind(scene::LightNode::Kind::Spot);
        else                    node->setKind(scene::LightNode::Kind::Point);
    }
    if (opts_color && opts_color_len >= 3) {
        node->setColor({static_cast<float>(opts_color[0]),
                        static_cast<float>(opts_color[1]),
                        static_cast<float>(opts_color[2])});
    }
    if (opts_intensity_given) node->setIntensity(static_cast<float>(opts_intensity));
    if (opts_range_given) node->setRange(static_cast<float>(opts_range));
    if (opts_innerCone_given) node->setInnerAngle(static_cast<float>(opts_innerCone));
    if (opts_outerCone_given) node->setOuterAngle(static_cast<float>(opts_outerCone));
    if (opts_castShadow_given) node->setCastsShadow(opts_castShadow);
    if (opts_position && opts_position_len >= 3) {
        node->setPosition(static_cast<float>(opts_position[0]),
                          static_cast<float>(opts_position[1]),
                          static_cast<float>(opts_position[2]));
    }
    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createDecal(void* self, bool opts_texture_given, const char* opts_texture,
                                       const double* opts_size, uint32_t opts_size_len,
                                       const double* opts_position, uint32_t opts_position_len,
                                       const double* opts_rotation, uint32_t opts_rotation_len) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createDecal();
    g->root()->addChild(node);
    if (opts_texture_given && opts_texture && opts_texture[0] != '\0') {
        broimage::Image img;
        if (broimage::decode_file(bro::util::resolveAssetPath(opts_texture), img) && img.width > 0 && img.height > 0) {
            node->setAlbedoTexture(img.width, img.height, img.pixels.data());
        }
    }
    if (opts_size && opts_size_len >= 3) {
        node->setScale(static_cast<float>(opts_size[0]),
                       static_cast<float>(opts_size[1]),
                       static_cast<float>(opts_size[2]));
    }
    if (opts_position && opts_position_len >= 3) {
        node->setPosition(static_cast<float>(opts_position[0]),
                          static_cast<float>(opts_position[1]),
                          static_cast<float>(opts_position[2]));
    }
    if (opts_rotation && opts_rotation_len >= 3) {
        const float toRad = 3.14159265f / 180.0f;
        node->setRotationEuler(static_cast<float>(opts_rotation[0]) * toRad,
                               static_cast<float>(opts_rotation[1]) * toRad,
                               static_cast<float>(opts_rotation[2]) * toRad);
    }
    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createReflectionProbe(void* self, const double* opts_size, uint32_t opts_size_len,
                                                bool opts_resolution_given, int32_t opts_resolution,
                                                const double* opts_position, uint32_t opts_position_len) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createReflectionProbe();
    g->root()->addChild(node);
    if (opts_size && opts_size_len >= 3) {
        node->setScale(static_cast<float>(opts_size[0]),
                       static_cast<float>(opts_size[1]),
                       static_cast<float>(opts_size[2]));
    }
    if (opts_resolution_given) node->setResolution(opts_resolution);
    if (opts_position && opts_position_len >= 3) {
        node->setPosition(static_cast<float>(opts_position[0]),
                          static_cast<float>(opts_position[1]),
                          static_cast<float>(opts_position[2]));
    }
    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createTween(void* self) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* tw = g->createTween();
    auto* cell = new HostTweenCell();
    cell->token = g->livenessToken();
    cell->id = tw->id();
    return cell;
}

void* bro_scene_SceneGraph_createAnimationPlayer(void* self) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* player = g->createClipPlayer();
    auto* cell = new HostClipPlayerCell();
    cell->token = g->livenessToken();
    cell->id = player->id();
    return cell;
}

void* bro_scene_SceneGraph_createTerrain(void* self, const int32_t* opts_chunkSize, uint32_t opts_chunkSize_len,
                                        bool opts_cellSize_given, double opts_cellSize,
                                        bool opts_loadRadius_given, int32_t opts_loadRadius,
                                        bool opts_unloadRadius_given, int32_t opts_unloadRadius,
                                        bool opts_maxLoadsPerUpdate_given, int32_t opts_maxLoadsPerUpdate,
                                        bool opts_seed_given, int32_t opts_seed,
                                        bool opts_noise_frequency_given, double opts_noise_frequency,
                                        bool opts_noise_octaves_given, int32_t opts_noise_octaves,
                                        bool opts_noise_gain_given, double opts_noise_gain,
                                        bool opts_noise_lacunarity_given, double opts_noise_lacunarity,
                                        bool opts_baseHeight_given, int32_t opts_baseHeight,
                                        bool opts_heightAmplitude_given, int32_t opts_heightAmplitude,
                                        bool opts_seaLevel_given, int32_t opts_seaLevel,
                                        bool opts_meshMode_given, int32_t opts_meshMode,
                                        bool opts_terraceStep_given, double opts_terraceStep,
                                        bool opts_continentFrequency_given, double opts_continentFrequency,
                                        bool opts_continentMin_given, double opts_continentMin,
                                        bool opts_continentMax_given, double opts_continentMax,
                                        bool opts_mountainFrequency_given, double opts_mountainFrequency,
                                        bool opts_mountainAmplitude_given, double opts_mountainAmplitude,
                                        bool opts_mountainOctaves_given, int32_t opts_mountainOctaves,
                                        bool opts_lodLevels_given, int32_t opts_lodLevels,
                                        bool opts_lodScaleFactor_given, int32_t opts_lodScaleFactor,
                                        bool opts_planetRadius_given, double opts_planetRadius,
                                        const double* opts_origin, uint32_t opts_origin_len,
                                        const float* opts_palette, uint32_t opts_palette_len) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    scene::TerrainConfig cfg;
    if (opts_chunkSize && opts_chunkSize_len >= 3) {
        cfg.chunkSizeX = opts_chunkSize[0];
        cfg.chunkSizeY = opts_chunkSize[1];
        cfg.chunkSizeZ = opts_chunkSize[2];
    }
    if (opts_cellSize_given) cfg.cellSize = static_cast<float>(opts_cellSize);
    if (opts_loadRadius_given) cfg.loadRadius = opts_loadRadius;
    if (opts_unloadRadius_given) cfg.unloadRadius = opts_unloadRadius;
    if (opts_maxLoadsPerUpdate_given) cfg.maxLoadsPerUpdate = opts_maxLoadsPerUpdate;
    if (opts_seed_given) cfg.seed = opts_seed;
    if (opts_noise_frequency_given) cfg.noiseFrequency = static_cast<float>(opts_noise_frequency);
    if (opts_noise_octaves_given) cfg.noiseOctaves = opts_noise_octaves;
    if (opts_noise_gain_given) cfg.noiseGain = static_cast<float>(opts_noise_gain);
    if (opts_noise_lacunarity_given) cfg.noiseLacunarity = static_cast<float>(opts_noise_lacunarity);
    if (opts_baseHeight_given) cfg.baseHeight = opts_baseHeight;
    if (opts_heightAmplitude_given) cfg.heightAmplitude = opts_heightAmplitude;
    if (opts_seaLevel_given) cfg.seaLevel = opts_seaLevel;
    if (opts_meshMode_given) cfg.meshMode = opts_meshMode;
    if (opts_terraceStep_given) cfg.terraceStep = static_cast<float>(opts_terraceStep);
    if (opts_continentFrequency_given) cfg.continentFrequency = static_cast<float>(opts_continentFrequency);
    if (opts_continentMin_given) cfg.continentMin = static_cast<float>(opts_continentMin);
    if (opts_continentMax_given) cfg.continentMax = static_cast<float>(opts_continentMax);
    if (opts_mountainFrequency_given) cfg.mountainFrequency = static_cast<float>(opts_mountainFrequency);
    if (opts_mountainAmplitude_given) cfg.mountainAmplitude = static_cast<float>(opts_mountainAmplitude);
    if (opts_mountainOctaves_given) cfg.mountainOctaves = opts_mountainOctaves;
    if (opts_lodLevels_given) cfg.lodLevelCount = opts_lodLevels;
    if (opts_lodScaleFactor_given) cfg.lodScaleFactor = opts_lodScaleFactor;
    if (opts_planetRadius_given) cfg.planetRadius = static_cast<float>(opts_planetRadius);
    if (opts_origin && opts_origin_len >= 3) {
        cfg.origin = {static_cast<float>(opts_origin[0]), static_cast<float>(opts_origin[1]),
                      static_cast<float>(opts_origin[2])};
    }
    if (opts_palette && opts_palette_len > 0) {
        cfg.palette.assign(opts_palette, opts_palette + opts_palette_len);
    }

    auto* cell = new HostTerrainCell();
    cell->token = g->livenessToken();
    cell->manager = std::make_unique<scene::TerrainManager>(*g);
    cell->manager->configure(cfg);
    return cell;
}

void* bro_scene_SceneGraph_createTileWorld(void* self, bool opts_chunkSize_given, int32_t opts_chunkSize,
                                          bool opts_tileSize_given, double opts_tileSize,
                                          const char* opts_layers, bool opts_tileAtlas_given, const char* opts_tileAtlas,
                                          bool opts_atlasTileWidth_given, int32_t opts_atlasTileWidth,
                                          bool opts_atlasTileHeight_given, int32_t opts_atlasTileHeight) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    scene::TileWorldConfig cfg;
    if (opts_chunkSize_given) cfg.chunkSize = opts_chunkSize;
    if (opts_tileSize_given) cfg.cellSize = static_cast<float>(opts_tileSize);

    auto* cell = new HostTileWorldCell();
    cell->token = g->livenessToken();
    cell->world = std::make_unique<scene::TileWorld>(*g);
    cell->world->configure(cfg);
    return cell;
}

void* bro_scene_SceneGraph_findById(void* self, int32_t id) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->findById(static_cast<uint32_t>(id));
    return node ? wrapNode(node, g) : nullptr;
}

void* bro_scene_SceneGraph_findByName(void* self, const char* name) {
    auto* g = graphOf(self);
    if (!g || !name) return nullptr;
    auto* node = g->findByName(name);
    return node ? wrapNode(node, g) : nullptr;
}

void bro_scene_SceneGraph_destroyNode(void* self, void* node) {
    auto* g = graphOf(self);
    auto* n = nodeOf(node);
    if (g && n) g->destroyNode(n);
}

void bro_scene_SceneGraph_setToneMap(void* self, bool opts_mode_given, const char* opts_mode,
                                    bool opts_whitePoint_given, double opts_whitePoint) {
    auto* g = graphOf(self);
    if (!g) return;
    scene::SceneRenderer::ToneMap mode = scene::SceneRenderer::ToneMap::ACES;
    if (opts_mode_given && opts_mode) {
        std::string m = opts_mode;
        if (m == "linear")        mode = scene::SceneRenderer::ToneMap::Linear;
        else if (m == "reinhard") mode = scene::SceneRenderer::ToneMap::Reinhard;
        else                      mode = scene::SceneRenderer::ToneMap::ACES;
    }
    float gamma = opts_whitePoint_given ? static_cast<float>(opts_whitePoint) : 2.2f;
    g->setToneMap(mode, gamma);
}

void bro_scene_SceneGraph_setExposure(void* self, const double* params, uint32_t params_len) {
    auto* g = graphOf(self);
    if (!g) return;
    // Every call replaces the whole exposure; a field left out (NaN) is its default.
    scene::ExposureSettings e;
    auto at = [&](uint32_t i, float& field, float lo, float hi) {
        if (params && i < params_len && std::isfinite(params[i])) field = std::clamp(static_cast<float>(params[i]), lo, hi);
    };
    if (params && params_len > 0 && std::isfinite(params[0]))
        e.mode = params[0] > 0.5 ? scene::ExposureSettings::Mode::Auto : scene::ExposureSettings::Mode::Fixed;
    at(1, e.ev100, -30.0f, 40.0f);
    at(2, e.compensation, -20.0f, 20.0f);
    at(3, e.speedUp, 0.0f, 1000.0f);
    at(4, e.speedDown, 0.0f, 1000.0f);
    at(5, e.minEv, -30.0f, 40.0f);
    at(6, e.maxEv, -30.0f, 40.0f);
    at(7, e.lowPercent, 0.0f, 1.0f);
    at(8, e.highPercent, 0.0f, 1.0f);
    if (params && params_len > 9 && std::isfinite(params[9]))
        e.meter = params[9] > 0.5 ? scene::ExposureSettings::Meter::Incident : scene::ExposureSettings::Meter::Reflected;
    if (e.maxEv < e.minEv) std::swap(e.minEv, e.maxEv);
    if (e.highPercent < e.lowPercent) std::swap(e.lowPercent, e.highPercent);
    g->setExposure(e);
}

void bro_scene_SceneGraph_setAmbient(void* self, const double* opts_color, uint32_t opts_color_len,
                                    bool opts_intensity_given, double opts_intensity) {
    auto* g = graphOf(self);
    if (!g) return;
    float r = 0.03f, gr = 0.03f, b = 0.03f;
    if (opts_color && opts_color_len >= 3) {
        r = static_cast<float>(opts_color[0]);
        gr = static_cast<float>(opts_color[1]);
        b = static_cast<float>(opts_color[2]);
    }
    if (opts_intensity_given) {
        float i = static_cast<float>(opts_intensity);
        r *= i; gr *= i; b *= i;
    }
    g->setAmbient(r, gr, b);
}

// setWind({direction:[x,y,z], strength, frequency}) — global wind sway.
void bro_scene_SceneGraph_setWind(void* self, const double* opts_direction, uint32_t opts_direction_len,
                                 bool opts_strength_given, double opts_strength,
                                 bool opts_frequency_given, double opts_frequency) {
    auto* g = graphOf(self);
    if (!g) return;
    float dx = 1.0f, dy = 0.0f, dz = 0.0f;
    if (opts_direction && opts_direction_len >= 3) {
        dx = static_cast<float>(opts_direction[0]);
        dy = static_cast<float>(opts_direction[1]);
        dz = static_cast<float>(opts_direction[2]);
    }
    const float strength = opts_strength_given ? static_cast<float>(opts_strength) : 0.0f;
    const float frequency = opts_frequency_given ? static_cast<float>(opts_frequency) : 1.5f;
    g->setWind(dx, dy, dz, strength, frequency);
}

// setWind's wind-field half: [speed, gust, gustSpeed, gustScale, drift]
// (scene/world/wind_field.h), defaults filled in by the JS wrapper. Non-finite
// entries keep the default.
void bro_scene_SceneGraph_setWindField(void* self, const double* params, uint32_t params_len) {
    auto* g = graphOf(self);
    if (!g) return;
    // A field left out (NaN) keeps what it was, as setWind's legacy fields do.
    scene::WindFieldParams p = g->windFieldParams();
    auto at = [&](uint32_t i, float& field, float lo, float hi) {
        if (params && i < params_len && std::isfinite(params[i])) {
            field = std::clamp(static_cast<float>(params[i]), lo, hi);
        }
    };
    at(0, p.speed, 0.0f, 100.0f);
    at(1, p.gust, 0.0f, 10.0f);
    at(2, p.gustSpeed, 0.0f, 200.0f);
    at(3, p.gustScale, 1.0f, 10000.0f);
    at(4, p.drift, 0.0f, 3.14159265f);
    g->setWindField(p);
}

namespace {
thread_local double tl_windBuf[16];
}

// windAt(x, z) -> [vx, vy, vz, gust, speed]: the wind field at absolute
// world (x, z) now, as the vegetation shaders see it.
void bro_scene_SceneGraph_windAt(void* self, double x, double z, bronze_native_buffer* out) {
    auto* g = graphOf(self);
    if (!out) return;
    if (!g || !std::isfinite(x) || !std::isfinite(z)) { copyBuffer<double>(nullptr, 0, out); return; }
    double o[3];
    g->placeOrigin(o);   // the field is the body's, (x, z) the place frame's
    const scene::WindSample w =
        scene::windAt(g->windField(), static_cast<float>(x + o[0]), static_cast<float>(z + o[2]));
    tl_windBuf[0] = w.velocity[0];
    tl_windBuf[1] = w.velocity[1];
    tl_windBuf[2] = w.velocity[2];
    tl_windBuf[3] = w.gust;
    tl_windBuf[4] = w.speed;
    copyBuffer(tl_windBuf, 5, out);
}

// wind() -> [dirX, dirY, dirZ, strength, frequency, speed, gust, gustSpeed,
// gustScale, drift, time, nowDirX, nowDirZ, nowSpeed]: what setWind set and
// the field's drifting state now.
void bro_scene_SceneGraph_windState(void* self, bronze_native_buffer* out) {
    auto* g = graphOf(self);
    if (!out) return;
    if (!g) { copyBuffer<double>(nullptr, 0, out); return; }
    const float* d = g->windDirection();
    const scene::WindFieldParams& p = g->windFieldParams();
    const scene::WindFieldState s = g->windField();
    const double v[14] = { d[0], d[1], d[2], g->windStrength(), g->windFrequency(),
                           p.speed, p.gust, p.gustSpeed, p.gustScale, p.drift,
                           g->windTime(), s.dirX, s.dirZ, s.base };
    std::copy(v, v + 14, tl_windBuf);
    copyBuffer(tl_windBuf, 14, out);
}

// setShadowQuality({pcfTaps}) — the receivers' PCF grid side (1, 3 or 5).
// A non-positive value keeps the default.
void bro_scene_SceneGraph_setShadowQuality(void* self, bool opts_pcfTaps_given, int32_t opts_pcfTaps) {
    auto* g = graphOf(self);
    if (!g) return;
    g->setShadowQuality((opts_pcfTaps_given && opts_pcfTaps > 0) ? opts_pcfTaps : 3);
}

void bro_scene_SceneGraph_setFog(void* self, bool opts_mode_given, const char* opts_mode,
                                const double* opts_color, uint32_t opts_color_len,
                                bool opts_density_given, double opts_density,
                                bool opts_start_given, double opts_start,
                                bool opts_end_given, double opts_end,
                                bool opts_heightFalloff_given, double opts_heightFalloff,
                                bool opts_height_given, double opts_height) {
    auto* g = graphOf(self);
    if (!g) return;
    float start = opts_start_given ? static_cast<float>(opts_start) : 0.0f;
    float end = opts_end_given ? static_cast<float>(opts_end) : 0.0f;
    float r = 0.0f, gr = 0.0f, b = 0.0f;
    if (opts_color && opts_color_len >= 3) {
        r = static_cast<float>(opts_color[0]);
        gr = static_cast<float>(opts_color[1]);
        b = static_cast<float>(opts_color[2]);
    }
    float density = opts_density_given ? static_cast<float>(opts_density) : 0.0f;
    float hFalloff = opts_heightFalloff_given ? static_cast<float>(opts_heightFalloff) : 0.0f;
    // `height` is the fog layer's base: the shader thins density by
    // exp(-falloff * worldY), and exp(-falloff * (worldY - height)) is that
    // times exp(falloff * height), so the base folds into the density.
    if (opts_height_given && hFalloff > 0.0f && density > 0.0f && std::isfinite(opts_height)) {
        const double scaled = static_cast<double>(density) * std::exp(static_cast<double>(hFalloff) * opts_height);
        density = std::isfinite(scaled) ? static_cast<float>(std::min(scaled, 1e30)) : 1e30f;
    }
    // mode: "linear" uses start/end only, "none" turns fog off; any other
    // mode (or none given) picks exponential when density > 0, else linear.
    const std::string mode = (opts_mode_given && opts_mode) ? opts_mode : "";
    if (mode == "linear") density = 0.0f;
    if (mode == "none" || mode == "off") { density = 0.0f; end = 0.0f; }
    g->setFog(start, end, r, gr, b);
    g->setFogExp(density, hFalloff, start);
}

void bro_scene_SceneGraph_setAtmosphere(void* self, const double* params, uint32_t params_len) {
    auto* g = graphOf(self);
    if (!g) return;
    // Every call replaces the whole atmosphere; a field left out (NaN) keeps
    // Earth's default (scene/world/atmosphere.h). The layout is
    // js/scene_graph.js's setAtmosphere.
    scene::AtmosphereParams a;
    auto given = [&](uint32_t i) { return params && i < params_len && std::isfinite(params[i]); };
    auto at = [&](uint32_t i, float& field, float lo, float hi) {
        if (given(i)) field = std::clamp(static_cast<float>(params[i]), lo, hi);
    };
    auto rgb = [&](uint32_t i, float* field, float lo, float hi) {
        if (given(i) && given(i + 1) && given(i + 2))
            for (uint32_t k = 0; k < 3; ++k) field[k] = std::clamp(static_cast<float>(params[i + k]), lo, hi);
    };
    a.enabled = !given(0) || params[0] > 0.5;
    rgb(1, a.rayleigh, 0.0f, 1e-2f);
    at(4, a.scaleHeightR, 1.0f, 1e6f);
    at(5, a.mieScattering, 0.0f, 1e-2f);
    at(6, a.mieG, -0.999f, 0.999f);
    at(7, a.mieAbsorption, 0.0f, 1e-2f);
    at(8, a.scaleHeightM, 1.0f, 1e6f);
    // Turbidity: haze as a multiple of the clear-air Mie coefficients.
    if (given(9) && params[9] > 0.0) {
        a.mieScattering *= static_cast<float>(params[9]);
        a.mieAbsorption *= static_cast<float>(params[9]);
    }
    rgb(10, a.ozone, 0.0f, 1e-2f);
    at(13, a.ozoneCenter, 0.0f, 1e7f);
    at(14, a.ozoneWidth, 1.0f, 1e7f);
    rgb(15, a.groundAlbedo, 0.0f, 1.0f);
    at(18, a.planetRadius, 1.0f, 1e10f);
    at(19, a.thickness, 1.0f, 1e9f);
    at(20, a.seaLevel, -1e9f, 1e9f);
    if (given(25) && given(26) && given(27)) {
        rgb(25, a.sunDir, -1.0f, 1.0f);
        a.sunDirExplicit = true;
    }
    if (given(28) && given(29) && given(30)) {
        rgb(28, a.sunColor, 0.0f, 1e12f);
        a.sunColorExplicit = true;
    }
    at(31, a.sunAngularRadius, 1e-4f, 0.5f);
    g->setAtmosphere(a);
}

void bro_scene_SceneGraph_setStarfield(void* self, bool opts_starCount_given, int32_t opts_starCount,
                                      bool opts_starSize_given, double opts_starSize,
                                      bool opts_twinkleSpeed_given, double opts_twinkleSpeed,
                                      const double* opts_tint, uint32_t opts_tint_len) {
    auto* g = graphOf(self);
    if (!g) return;
    scene::StarfieldParams s;
    s.enabled = true;
    if (opts_starCount_given) s.density = static_cast<float>(opts_starCount) / 1000.0f;
    if (opts_starSize_given) s.intensity = static_cast<float>(opts_starSize);
    g->setStarfield(s);
}

}  // extern "C"

namespace bro::bronze_host {

// The agent members on the scene prototypes registerSceneNatives
// published (registerBroNatives runs it after every table).
void installSceneAgents() {
    ev::GlobalValue root = ev::globalValue("__bro_native");
    if (root.found && ev::isObject(root.value)) {
        ev::Persistent rootSlot(root.value);
        ev::Persistent sceneSlot(ev::getProperty(rootSlot.get(), "scene"));
        if (ev::isObject(sceneSlot.get())) {
            Value sgProto = ev::getProperty(sceneSlot.get(), "SceneGraphProto");
            if (ev::isObject(sgProto)) {
                ObjectBuilder b(sgProto);
                installSceneGraphAgent(b);
            }
            Value snProto = ev::getProperty(sceneSlot.get(), "SceneNodeProto");
            if (ev::isObject(snProto)) {
                ObjectBuilder b(snProto);
                installSceneNodeAgent(b);
                b.def("attachAudioEmitter", 2, [](Value self_, std::span<const Value> a) {
                    if (a.empty()) return ev::undefined();
                    // Convert first: toDouble runs a script's valueOf, which
                    // may destroy this node, and allocates (self_ is a copy).
                    const Rooted self(self_);
                    int handle = satCast<int>(ev::toDouble(a[0]));
                    bool isVoice = a.size() >= 2 && ev::toBool(a[1]);
                    auto* cell = sceneNodeCellOf(self);
                    if (!cell) return ev::undefined();
                    auto* n = cell->node();
                    if (!n) return ev::undefined();
                    bro::engine::SceneAudioSync::attachAudioEmitter(cell->token, n, handle, isVoice);
                    return ev::undefined();
                });
                b.def("detachAudioEmitter", 0, [](Value self_, std::span<const Value>) {
                    auto* cell = sceneNodeCellOf(self_);
                    if (!cell) return ev::undefined();
                    bro::engine::SceneAudioSync::detachAudioEmitter(cell->id);
                    return ev::undefined();
                });
            }
        }
    }
}


}  // namespace bro::bronze_host
