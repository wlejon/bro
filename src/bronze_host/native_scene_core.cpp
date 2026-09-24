// native_scene_core.cpp — SceneGraph lifecycle, rendering, settings, camera, and raycasting.

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "natives/scene/native_scene_decl.h"
#include "util/asset_path.h"
#include <bromesh/analysis/raycast.h>
#include <bromesh/analysis/bvh.h>
#include <bromesh/primitives/primitives.h>
#include <bromesh/manipulation/normals.h>
#include <broimage/decode.h>
#include <glad/gl.h>
#include "engine/scene_audio_sync.h"
#include <string_view>

namespace bro::bronze_host {

bool registerNatives_scene(std::string* error);

Value createSceneGraphValue(scene::SceneGraph* sg, dom::Element* canvas) {
    if (!sg) return ev::null();
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

bool bro_scene_SceneGraph_shadowCache_get(void* self) {
    auto* g = graphOf(self);
    return g ? g->shadowCache() : false;
}

void bro_scene_SceneGraph_shadowCache_set(void* self, bool v) {
    auto* g = graphOf(self);
    if (g) g->setShadowCache(v);
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

void* bro_scene_SceneGraph_createClipmapTerrain(void* self, bool opts_levels_given, int32_t opts_levels,
                                                bool opts_resolution_given, int32_t opts_resolution,
                                                bool opts_cellSize_given, double opts_cellSize,
                                                bool opts_heightScale_given, double opts_heightScale,
                                                bool opts_seaLevel_given, double opts_seaLevel,
                                                bool opts_snowLine_given, double opts_snowLine,
                                                bool opts_maxCellScale_given, double opts_maxCellScale,
                                                bool opts_planetRadius_given, double opts_planetRadius,
                                                bool opts_layerFade_given, bool opts_layerFade,
                                                bool opts_coverageFloor_given, bool opts_coverageFloor,
                                                bool opts_cubicSurface_given, bool opts_cubicSurface,
                                                bool opts_cubicHeight_given, bool opts_cubicHeight,
                                                bool opts_detailWavelength_given, double opts_detailWavelength,
                                                bool opts_detailRelief_given, double opts_detailRelief,
                                                bool opts_detailGain_given, double opts_detailGain,
                                                bool opts_detailOctaves_given, int32_t opts_detailOctaves) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    scene::ClipmapConfig cfg;
    if (opts_levels_given) cfg.levels = opts_levels;
    if (opts_resolution_given) cfg.resolution = opts_resolution;
    if (opts_cellSize_given) cfg.cellSize = static_cast<float>(opts_cellSize);
    if (opts_heightScale_given) cfg.heightScale = static_cast<float>(opts_heightScale);
    if (opts_seaLevel_given) cfg.seaLevel = static_cast<float>(opts_seaLevel);
    if (opts_snowLine_given) cfg.snowLine = static_cast<float>(opts_snowLine);
    if (opts_maxCellScale_given) cfg.maxCellScale = static_cast<float>(opts_maxCellScale);
    if (opts_planetRadius_given) cfg.planetRadius = static_cast<float>(opts_planetRadius);
    if (opts_layerFade_given) cfg.layerFade = opts_layerFade;
    if (opts_coverageFloor_given) cfg.coverageFloor = opts_coverageFloor;
    if (opts_cubicSurface_given) cfg.cubicSurface = opts_cubicSurface;
    if (opts_cubicHeight_given) cfg.cubicHeight = opts_cubicHeight;
    if (opts_detailWavelength_given) cfg.detailWavelength = static_cast<float>(opts_detailWavelength);
    if (opts_detailRelief_given) cfg.detailRelief = static_cast<float>(opts_detailRelief);
    if (opts_detailGain_given) cfg.detailGain = static_cast<float>(opts_detailGain);
    if (opts_detailOctaves_given) cfg.detailOctaves = opts_detailOctaves;

    auto* cell = new HostClipmapCell();
    cell->token = g->livenessToken();
    cell->terrain = std::make_unique<scene::ClipmapTerrain>(*g, cfg);
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
                                    bool opts_exposure_given, double opts_exposure,
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
    float exp = opts_exposure_given ? static_cast<float>(opts_exposure) : 1.0f;
    float gamma = opts_whitePoint_given ? static_cast<float>(opts_whitePoint) : 2.2f;
    g->setToneMap(mode, exp, gamma);
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

// setShadowQuality({atlasSize, pcfTaps}) — atlas side length and PCF grid
// side (1, 3 or 5). A non-positive value keeps the default.
void bro_scene_SceneGraph_setShadowQuality(void* self, bool opts_atlasSize_given, int32_t opts_atlasSize,
                                          bool opts_pcfTaps_given, int32_t opts_pcfTaps) {
    auto* g = graphOf(self);
    if (!g) return;
    const int atlasSize = (opts_atlasSize_given && opts_atlasSize > 0) ? opts_atlasSize : 4096;
    const int pcfTaps = (opts_pcfTaps_given && opts_pcfTaps > 0) ? opts_pcfTaps : 3;
    g->setShadowQuality(atlasSize, pcfTaps);
}

void bro_scene_SceneGraph_setShadowCache(void* self, bool opts_enabled_given, bool opts_enabled,
                                        bool opts_staticResolution_given, int32_t opts_staticResolution) {
    auto* g = graphOf(self);
    if (!g) return;
    if (opts_enabled_given) g->setShadowCache(opts_enabled);
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

void bro_scene_SceneGraph_setAtmosphere(void* self, const double* opts_rayleigh, uint32_t opts_rayleigh_len,
                                       const double* opts_mie, uint32_t opts_mie_len,
                                       bool opts_turbidity_given, double opts_turbidity,
                                       const double* opts_sunPosition, uint32_t opts_sunPosition_len,
                                       bool opts_sunIntensity_given, double opts_sunIntensity) {
    auto* g = graphOf(self);
    if (!g) return;
    scene::AtmosphereParams a;
    a.enabled = true;
    // A coefficient a script gives is taken when it is a finite, non-negative
    // number; anything else keeps the physical default.
    const auto coeff = [](double v, float& out) {
        if (std::isfinite(v) && v >= 0.0) out = static_cast<float>(v);
    };
    if (opts_rayleigh && opts_rayleigh_len >= 3) {
        for (int i = 0; i < 3; ++i) coeff(opts_rayleigh[i], a.betaR[i]);
    }
    if (opts_mie && opts_mie_len >= 1) coeff(opts_mie[0], a.betaM);
    if (opts_mie && opts_mie_len >= 2 && std::isfinite(opts_mie[1])) {
        a.mieG = std::clamp(static_cast<float>(opts_mie[1]), -0.999f, 0.999f);
    }
    // Turbidity: haze as a multiple of the clear-air Mie coefficient.
    if (opts_turbidity_given && std::isfinite(opts_turbidity) && opts_turbidity > 0.0) {
        a.betaM *= static_cast<float>(opts_turbidity);
    }
    if (opts_sunPosition && opts_sunPosition_len >= 3) {
        a.sunDir[0] = static_cast<float>(opts_sunPosition[0]);
        a.sunDir[1] = static_cast<float>(opts_sunPosition[1]);
        a.sunDir[2] = static_cast<float>(opts_sunPosition[2]);
    }
    // An explicit intensity pins the sun's radiance; without the flag the
    // renderer tracks the brightest directional light and the value given
    // here was silently replaced.
    if (opts_sunIntensity_given && std::isfinite(opts_sunIntensity)) {
        a.sunColor[0] = a.sunColor[1] = a.sunColor[2] = static_cast<float>(opts_sunIntensity) * 20.0f;
        a.sunColorExplicit = true;
    }
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

bool registerSceneNatives(std::string* error) {
    if (!registerNatives_scene(error)) return false;

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

    using namespace natives;
    return fn("__bro_native.scene.SceneGraph_render", (void*)&bro_scene_SceneGraph_render, "void", {"__bro_native.scene.SceneGraph"}, error) &&
           fn("__bro_native.scene.SceneGraph_canvasWidth", (void*)&bro_scene_SceneGraph_canvasWidth, "f64", {"__bro_native.scene.SceneGraph"}, error) &&
           fn("__bro_native.scene.SceneGraph_canvasHeight", (void*)&bro_scene_SceneGraph_canvasHeight, "f64", {"__bro_native.scene.SceneGraph"}, error) &&
           fn("__bro_native.scene.SceneGraph_setCanvasSize", (void*)&bro_scene_SceneGraph_setCanvasSize, "void", {"__bro_native.scene.SceneGraph", "f64", "f64"}, error) &&
           fn("__bro_native.scene.SceneGraph_readTonemapPixels", (void*)&bro_scene_SceneGraph_readTonemapPixels, "u8[]", {"__bro_native.scene.SceneGraph"}, error) &&
           fn("__bro_native.scene.SceneGraph_createMesh", (void*)&bro_scene_SceneGraph_createMesh, "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "dynamic", "dynamic"}, error) &&
           fn("__bro_native.scene.SceneGraph_createSkinnedMesh", (void*)&bro_scene_SceneGraph_createSkinnedMesh, "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "dynamic", "dynamic"}, error) &&
           fn("__bro_native.scene.SceneGraph_createInstancedMesh", (void*)&bro_scene_SceneGraph_createInstancedMesh, "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "dynamic", "dynamic"}, error) &&
           fn("__bro_native.scene.SceneGraph_createShape", (void*)&bro_scene_SceneGraph_createShape, "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "str"}, error) &&
           fn("__bro_native.scene.SceneGraph_createSprite", (void*)&bro_scene_SceneGraph_createSprite, "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "str"}, error) &&
           fn("__bro_native.scene.SceneGraph_createPhysicsNode", (void*)&bro_scene_SceneGraph_createPhysicsNode, "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "str"}, error) &&
           fn("__bro_native.scene.SceneGraph_createParticles3D", (void*)&bro_scene_SceneGraph_createParticles3D, "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "str"}, error) &&
           fn("__bro_native.scene.SceneGraph_createGaussianSplat", (void*)&bro_scene_SceneGraph_createGaussianSplat, "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "str"}, error) &&
           fn("__bro_native.scene.SceneNode_type_get", (void*)&bro_scene_SceneNode_type_get, "str", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_kind_get", (void*)&bro_scene_SceneNode_kind_get, "str", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_childCount_get", (void*)&bro_scene_SceneNode_childCount_get, "i32", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_castsShadow_get", (void*)&bro_scene_SceneNode_castsShadow_get, "bool", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_castsShadow_set", (void*)&bro_scene_SceneNode_castsShadow_set, "void", {"__bro_native.scene.SceneNode", "bool"}, error) &&
           fn("__bro_native.scene.SceneNode_receivesShadow_get", (void*)&bro_scene_SceneNode_receivesShadow_get, "bool", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_receivesShadow_set", (void*)&bro_scene_SceneNode_receivesShadow_set, "void", {"__bro_native.scene.SceneNode", "bool"}, error) &&
           fn("__bro_native.scene.SceneNode_metallic_get", (void*)&bro_scene_SceneNode_metallic_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_metallic_set", (void*)&bro_scene_SceneNode_metallic_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_roughness_get", (void*)&bro_scene_SceneNode_roughness_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_roughness_set", (void*)&bro_scene_SceneNode_roughness_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_emissive_get", (void*)&bro_scene_SceneNode_emissive_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_emissive_set", (void*)&bro_scene_SceneNode_emissive_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_direction_get", (void*)&bro_scene_SceneNode_direction_get, "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_direction_set", (void*)&bro_scene_SceneNode_direction_set, "void", {"__bro_native.scene.SceneNode", "f64[]"}, error) &&
           fn("__bro_native.scene.SceneNode_color_get", (void*)&bro_scene_SceneNode_color_get, "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_color_set", (void*)&bro_scene_SceneNode_color_set, "void", {"__bro_native.scene.SceneNode", "f64[]"}, error) &&
           fn("__bro_native.scene.SceneNode_intensity_get", (void*)&bro_scene_SceneNode_intensity_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_intensity_set", (void*)&bro_scene_SceneNode_intensity_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_range_get", (void*)&bro_scene_SceneNode_range_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_range_set", (void*)&bro_scene_SceneNode_range_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_innerAngle_get", (void*)&bro_scene_SceneNode_innerAngle_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_innerAngle_set", (void*)&bro_scene_SceneNode_innerAngle_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_outerAngle_get", (void*)&bro_scene_SceneNode_outerAngle_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_outerAngle_set", (void*)&bro_scene_SceneNode_outerAngle_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_shadowBias_get", (void*)&bro_scene_SceneNode_shadowBias_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_shadowBias_set", (void*)&bro_scene_SceneNode_shadowBias_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_shadowNormalBias_get", (void*)&bro_scene_SceneNode_shadowNormalBias_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_shadowNormalBias_set", (void*)&bro_scene_SceneNode_shadowNormalBias_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_cascadeCount_get", (void*)&bro_scene_SceneNode_cascadeCount_get, "i32", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_cascadeCount_set", (void*)&bro_scene_SceneNode_cascadeCount_set, "void", {"__bro_native.scene.SceneNode", "i32"}, error) &&
           fn("__bro_native.scene.SceneNode_cascadeSplitLambda_get", (void*)&bro_scene_SceneNode_cascadeSplitLambda_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_cascadeSplitLambda_set", (void*)&bro_scene_SceneNode_cascadeSplitLambda_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_setSkeleton", (void*)&bro_scene_SceneNode_setSkeleton, "void", {"__bro_native.scene.SceneNode", "dynamic"}, error) &&
           fn("__bro_native.scene.SceneNode_addClip", (void*)&bro_scene_SceneNode_addClip, "void", {"__bro_native.scene.SceneNode", "str", "dynamic"}, error) &&
           fn("__bro_native.scene.SceneNode_addBlendSpace1D", (void*)&bro_scene_SceneNode_addBlendSpace1D, "void", {"__bro_native.scene.SceneNode", "str", "str"}, error) &&
           fn("__bro_native.scene.SceneNode_addBlendSpace2D", (void*)&bro_scene_SceneNode_addBlendSpace2D, "void", {"__bro_native.scene.SceneNode", "str", "str"}, error) &&
           fn("__bro_native.scene.SceneNode_setBlendPos", (void*)&bro_scene_SceneNode_setBlendPos, "void", {"__bro_native.scene.SceneNode", "str", "f64", "bool", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_playLayer", (void*)&bro_scene_SceneNode_playLayer, "void", {"__bro_native.scene.SceneNode", "i32", "str", "str"}, error) &&
           fn("__bro_native.scene.SceneNode_stopLayer", (void*)&bro_scene_SceneNode_stopLayer, "void", {"__bro_native.scene.SceneNode", "i32", "bool", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_setLayerWeight", (void*)&bro_scene_SceneNode_setLayerWeight, "void", {"__bro_native.scene.SceneNode", "i32", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_addStateMachine", (void*)&bro_scene_SceneNode_addStateMachine, "void", {"__bro_native.scene.SceneNode", "str"}, error) &&
           fn("__bro_native.scene.SceneNode_travel", (void*)&bro_scene_SceneNode_travel, "void", {"__bro_native.scene.SceneNode", "str"}, error) &&
           fn("__bro_native.scene.SceneNode_setRootMotion", (void*)&bro_scene_SceneNode_setRootMotion, "void", {"__bro_native.scene.SceneNode", "str"}, error) &&
           fn("__bro_native.scene.SceneNode_consumeRootMotion", (void*)&bro_scene_SceneNode_consumeRootMotion, "str", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_play", (void*)&bro_scene_SceneNode_play, "void", {"__bro_native.scene.SceneNode", "str", "str"}, error) &&
           fn("__bro_native.scene.SceneNode_stop", (void*)&bro_scene_SceneNode_stop, "void", {"__bro_native.scene.SceneNode", "str"}, error) &&
           fn("__bro_native.scene.SceneNode_pause", (void*)&bro_scene_SceneNode_pause, "void", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_resume", (void*)&bro_scene_SceneNode_resume, "void", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_setSkinningMatrices", (void*)&bro_scene_SceneNode_setSkinningMatrices, "i32", {"__bro_native.scene.SceneNode", "dynamic"}, error) &&
           fn("__bro_native.scene.SceneNode_getBoneWorldMatrix", (void*)&bro_scene_SceneNode_getBoneWorldMatrix, "dynamic", {"__bro_native.scene.SceneNode", "dynamic"}, error) &&
           fn("__bro_native.scene.SceneNode_blendState", (void*)&bro_scene_SceneNode_blendState, "str", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_onAnimationFinished_set", (void*)&bro_scene_SceneNode_onAnimationFinished_set, "void", {"__bro_native.scene.SceneNode", "dynamic"}, error) &&
           fn("__bro_native.scene.SceneNode_onStateChanged_set", (void*)&bro_scene_SceneNode_onStateChanged_set, "void", {"__bro_native.scene.SceneNode", "dynamic"}, error) &&
           fn("__bro_native.scene.SceneNode_hasShader", (void*)&bro_scene_SceneNode_hasShader, "bool", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_setShader", (void*)&bro_scene_SceneNode_setShader, "str", {"__bro_native.scene.SceneNode", "str", "str", "str"}, error) &&
           fn("__bro_native.scene.SceneNode_clearShader", (void*)&bro_scene_SceneNode_clearShader, "void", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_setShaderUniform", (void*)&bro_scene_SceneNode_setShaderUniform, "void", {"__bro_native.scene.SceneNode", "str", "f64[]"}, error) &&
           fn("__bro_native.scene.SceneNode_setLodMeshes", (void*)&bro_scene_SceneNode_setLodMeshes, "void", {"__bro_native.scene.SceneNode", "str"}, error) &&
           fn("__bro_native.scene.SceneNode_updateMesh", (void*)&bro_scene_SceneNode_updateMesh, "void", {"__bro_native.scene.SceneNode", "dynamic", "bool"}, error) &&
           fn("__bro_native.scene.SceneNode_lodCount", (void*)&bro_scene_SceneNode_lodCount, "i32", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_lodLevel", (void*)&bro_scene_SceneNode_lodLevel, "i32", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_visibilityRange_set", (void*)&bro_scene_SceneNode_visibilityRange_set, "void", {"__bro_native.scene.SceneNode", "f64", "f64", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_visibilityRange_clear", (void*)&bro_scene_SceneNode_visibilityRange_clear, "void", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_visibilityRange_get", (void*)&bro_scene_SceneNode_visibilityRange_get, "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneGraph_isValid", (void*)&bro_scene_SceneGraph_isValid, "bool", {"__bro_native.scene.SceneGraph"}, error) &&
           fn("__bro_native.scene.SceneNode_setBaseColorTextureFromScene", (void*)&bro_scene_SceneNode_setBaseColorTextureFromScene, "void", {"__bro_native.scene.SceneNode", "__bro_native.scene.SceneGraph"}, error) &&
           fn("__bro_native.scene.SceneGraph_cullStatsJson", (void*)&bro_scene_SceneGraph_cullStatsJson, "str", {"__bro_native.scene.SceneGraph"}, error) &&
           fn("__bro_native.scene.SceneNode_setInstances", (void*)&bro_scene_SceneNode_setInstances, "void", {"__bro_native.scene.SceneNode", "f32[]"}, error) &&
           fn("__bro_native.scene.SceneNode_setInstancesFromTransforms", (void*)&bro_scene_SceneNode_setInstancesFromTransforms, "void", {"__bro_native.scene.SceneNode", "f32[]"}, error) &&
           fn("__bro_native.scene.SceneNode_instanceCount_get", (void*)&bro_scene_SceneNode_instanceCount_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_updateMode_get", (void*)&bro_scene_SceneNode_updateMode_get, "str", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_updateMode_set", (void*)&bro_scene_SceneNode_updateMode_set, "void", {"__bro_native.scene.SceneNode", "str"}, error) &&
           fn("__bro_native.scene.SceneNode_resolution_get", (void*)&bro_scene_SceneNode_resolution_get, "i32", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_resolution_set", (void*)&bro_scene_SceneNode_resolution_set, "void", {"__bro_native.scene.SceneNode", "i32"}, error) &&
           fn("__bro_native.scene.SceneNode_boxProjection_get", (void*)&bro_scene_SceneNode_boxProjection_get, "bool", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_boxProjection_set", (void*)&bro_scene_SceneNode_boxProjection_set, "void", {"__bro_native.scene.SceneNode", "bool"}, error) &&
           fn("__bro_native.scene.SceneNode_cullMargin_get", (void*)&bro_scene_SceneNode_cullMargin_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_cullMargin_set", (void*)&bro_scene_SceneNode_cullMargin_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_splatCount_get", (void*)&bro_scene_SceneNode_splatCount_get, "i32", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_setCloud", (void*)&bro_scene_SceneNode_setCloud, "void", {"__bro_native.scene.SceneNode", "f32[]", "f32[]", "f32[]", "f32[]", "f32[]", "i32"}, error) &&
           fn("__bro_native.scene.SceneNode_loadSplatPly", (void*)&bro_scene_SceneNode_loadSplatPly, "bool", {"__bro_native.scene.SceneNode", "str"}, error) &&
           fn("__bro_native.scene.SceneNode_worldAnchor_get", (void*)&bro_scene_SceneNode_worldAnchor_get, "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_worldAnchor_set", (void*)&bro_scene_SceneNode_worldAnchor_set, "void", {"__bro_native.scene.SceneNode", "f64[]"}, error) &&
           registerSceneShaderNatives(error) &&
           registerSceneCameraNatives(error) &&
           registerSceneExtraNatives(error);
}

}  // namespace bro::bronze_host
