#pragma once

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/mesh_node.h"
#include "scene/skinned_mesh_node.h"
#include "scene/instanced_mesh_node.h"
#include "scene/light_node.h"
#include "scene/camera_node.h"
#include "scene/physics_node.h"
#include "scene/shape_node.h"
#include "scene/sprite_node.h"
#include "scene/html_node.h"
#include "scene/particle_node.h"
#include "scene/particles3d_node.h"
#include "scene/gaussian_splat_node.h"
#include "scene/decal_node.h"
#include "scene/reflection_probe_node.h"
#include "scene/terrain_manager.h"
#include "scene/clipmap_terrain.h"
#include "scene/tile_world.h"
#include "scene/tween.h"
#include "scene/clip_player.h"
#include "dom/element.h"
#include "canvas/canvas2d.h"
#include "abi/bronze_native_type.h"

#include "util/asset_path.h"

#include <memory>
#include <string>
#include <vector>
#include <span>
#include <unordered_map>
#include <cmath>
#include <algorithm>

namespace bro::bronze_host {
namespace scene = bro::scene;
namespace engine = bro::engine;
namespace tile = bro::tile;

inline constexpr uint32_t kHostSceneGraphTag   = 0x53434E47u;  // 'SCNG'
inline constexpr uint32_t kHostSceneNodeTag    = 0x534E4F44u;  // 'SNOD'
inline constexpr uint32_t kHostTerrainTag      = 0x54455252u;  // 'TERR'
inline constexpr uint32_t kHostTweenTag        = 0x5457454Eu;  // 'TWEN'
inline constexpr uint32_t kHostClipPlayerTag   = 0x434C4950u;  // 'CLIP'
inline constexpr uint32_t kHostTileWorldTag    = 0x54494C45u;  // 'TILE'
inline constexpr uint32_t kHostClipmapTag      = 0x434C494Du;  // 'CLIM'

struct HostSceneGraphCell {
    uint32_t tag = kHostSceneGraphTag;
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;
    dom::Element* canvas = nullptr;

    scene::SceneGraph* graph() const {
        auto t = token.lock();
        return t ? t->graph : nullptr;
    }
};

struct HostSceneNodeCell {
    uint32_t tag = kHostSceneNodeTag;
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;
    uint32_t id = 0;

    scene::SceneGraph* graph() const {
        auto t = token.lock();
        return t ? t->graph : nullptr;
    }

    scene::SceneNode* node() const {
        auto t = token.lock();
        return (t && t->graph) ? t->graph->resolveNode(id) : nullptr;
    }
};

struct HostTerrainCell {
    uint32_t tag = kHostTerrainTag;
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;
    std::unique_ptr<scene::TerrainManager> manager;
    ev::Persistent heightSource;
    bool hasHeightSource = false;

    scene::SceneGraph* graph() const {
        auto t = token.lock();
        return t ? t->graph : nullptr;
    }

    scene::TerrainManager* mgr() const { return manager.get(); }
};

struct HostClipmapCell {
    uint32_t tag = kHostClipmapTag;
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;
    std::unique_ptr<scene::ClipmapTerrain> terrain;

    scene::SceneGraph* graph() const {
        auto t = token.lock();
        return t ? t->graph : nullptr;
    }

    scene::ClipmapTerrain* clipmap() const { return terrain.get(); }
};

struct HostTileWorldCell {
    uint32_t tag = kHostTileWorldTag;
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;
    std::unique_ptr<scene::TileWorld> world;

    scene::SceneGraph* graph() const {
        auto t = token.lock();
        return t ? t->graph : nullptr;
    }

    scene::TileWorld* tileWorld() const { return world.get(); }
};

struct HostTweenCell {
    uint32_t tag = kHostTweenTag;
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;
    uint32_t id = 0;

    scene::SceneGraph* graph() const {
        auto t = token.lock();
        return t ? t->graph : nullptr;
    }

    scene::Tween* tween() const {
        auto t = token.lock();
        return (t && t->graph) ? t->graph->findTween(id) : nullptr;
    }
};

struct HostClipPlayerCell {
    uint32_t tag = kHostClipPlayerTag;
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;
    uint32_t id = 0;

    scene::SceneGraph* graph() const {
        auto t = token.lock();
        return t ? t->graph : nullptr;
    }

    scene::ClipPlayer* player() const {
        auto t = token.lock();
        return (t && t->graph) ? t->graph->findClipPlayer(id) : nullptr;
    }
};

inline scene::SceneGraph* graphOf(void* self) {
    auto* c = static_cast<HostSceneGraphCell*>(self);
    return (c && c->tag == kHostSceneGraphTag) ? c->graph() : nullptr;
}

inline HostSceneGraphCell* graphCellOf(void* self) {
    auto* c = static_cast<HostSceneGraphCell*>(self);
    return (c && c->tag == kHostSceneGraphTag) ? c : nullptr;
}

inline scene::SceneNode* nodeOf(void* self) {
    auto* c = static_cast<HostSceneNodeCell*>(self);
    return (c && c->tag == kHostSceneNodeTag) ? c->node() : nullptr;
}

inline HostSceneNodeCell* nodeCellOf(void* self) {
    auto* c = static_cast<HostSceneNodeCell*>(self);
    return (c && c->tag == kHostSceneNodeTag) ? c : nullptr;
}

inline scene::TerrainManager* terrainOf(void* self) {
    auto* c = static_cast<HostTerrainCell*>(self);
    return (c && c->tag == kHostTerrainTag) ? c->manager.get() : nullptr;
}

inline HostTerrainCell* terrainCellOf(void* self) {
    auto* c = static_cast<HostTerrainCell*>(self);
    return (c && c->tag == kHostTerrainTag) ? c : nullptr;
}

inline scene::ClipmapTerrain* clipmapOf(void* self) {
    auto* c = static_cast<HostClipmapCell*>(self);
    return (c && c->tag == kHostClipmapTag) ? c->terrain.get() : nullptr;
}

inline HostClipmapCell* clipmapCellOf(void* self) {
    auto* c = static_cast<HostClipmapCell*>(self);
    return (c && c->tag == kHostClipmapTag) ? c : nullptr;
}

inline scene::TileWorld* tileWorldOf(void* self) {
    auto* c = static_cast<HostTileWorldCell*>(self);
    return (c && c->tag == kHostTileWorldTag) ? c->world.get() : nullptr;
}

inline HostTileWorldCell* tileWorldCellOf(void* self) {
    auto* c = static_cast<HostTileWorldCell*>(self);
    return (c && c->tag == kHostTileWorldTag) ? c : nullptr;
}

inline scene::Tween* tweenOf(void* self) {
    auto* c = static_cast<HostTweenCell*>(self);
    return (c && c->tag == kHostTweenTag) ? c->tween() : nullptr;
}

inline HostTweenCell* tweenCellOf(void* self) {
    auto* c = static_cast<HostTweenCell*>(self);
    return (c && c->tag == kHostTweenTag) ? c : nullptr;
}

inline scene::ClipPlayer* clipPlayerOf(void* self) {
    auto* c = static_cast<HostClipPlayerCell*>(self);
    return (c && c->tag == kHostClipPlayerTag) ? c->player() : nullptr;
}

inline HostClipPlayerCell* clipPlayerCellOf(void* self) {
    auto* c = static_cast<HostClipPlayerCell*>(self);
    return (c && c->tag == kHostClipPlayerTag) ? c : nullptr;
}

inline void* wrapNode(scene::SceneNode* node, scene::SceneGraph* graph) {
    if (!node || !graph) return nullptr;
    auto tok = graph->livenessToken();
    if (!tok) return nullptr;
    return new HostSceneNodeCell{kHostSceneNodeTag, tok, node->id()};
}

inline HostSceneGraphCell* sceneGraphCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* h = static_cast<HostSceneGraphCell*>(ev::handleData(v));
    return (h && h->tag == kHostSceneGraphTag) ? h : nullptr;
}

inline scene::SceneGraph* sceneGraphOf(Value v) {
    auto* c = sceneGraphCellOf(v);
    return c ? c->graph() : nullptr;
}

Value createSceneGraphValue(scene::SceneGraph* sg, dom::Element* canvas);

template <typename T>
inline void copyBuffer(const T* data, uint32_t len, bronze_native_buffer* out) {
    out->data = const_cast<T*>(data);
    out->length = len;
    out->release = nullptr;
    out->ctx = nullptr;
}

template <typename T>
inline void transferBuffer(std::vector<T>&& v, bronze_native_buffer* out) {
    if (v.empty()) {
        out->data = nullptr;
        out->length = 0;
        out->release = nullptr;
        out->ctx = nullptr;
        return;
    }
    auto* owned = new std::vector<T>(std::move(v));
    out->data = owned->data();
    out->length = static_cast<uint32_t>(owned->size());
    out->release = [](void* ctx) { delete static_cast<std::vector<T>*>(ctx); };
    out->ctx = owned;
}

inline const char* strResult(std::string s) {
    thread_local std::string scratch;
    scratch = std::move(s);
    return scratch.c_str();
}

inline bool parseHexOrCssColor(const std::string& s, float& r, float& g, float& b, float& a) {
    uint8_t u8r, u8g, u8b, u8a;
    if (canvas::parseCSSColor(s, u8r, u8g, u8b, u8a)) {
        r = u8r / 255.0f;
        g = u8g / 255.0f;
        b = u8b / 255.0f;
        a = u8a / 255.0f;
        return true;
    }
    return false;
}

inline bool parseColorValue(Value v, float& r, float& g, float& b, float& a) {
    if (ev::isString(v)) {
        return parseHexOrCssColor(ev::toUtf8(v), r, g, b, a);
    }
    if (ev::isObject(v)) {
        const Rooted arr(v);
        Value lenV = ev::getProperty(arr, "length");
        if (ev::isNumber(lenV)) {
            int len = satCast<int>(ev::toDouble(lenV));
            if (len >= 3) {
                r = static_cast<float>(ev::toDouble(ev::getElement(arr, 0)));
                g = static_cast<float>(ev::toDouble(ev::getElement(arr, 1)));
                b = static_cast<float>(ev::toDouble(ev::getElement(arr, 2)));
                if (len >= 4) {
                    Value va = ev::getElement(arr, 3);
                    if (!ev::isUndefined(va)) a = static_cast<float>(ev::toDouble(va));
                }
                return true;
            }
        }
    }
    return false;
}



inline HostSceneNodeCell* sceneNodeCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* h = static_cast<HostSceneNodeCell*>(ev::handleData(v));
    return (h && h->tag == kHostSceneNodeTag) ? h : nullptr;
}

inline scene::SceneNode* sceneNodeOf(Value v) {
    auto* c = sceneNodeCellOf(v);
    return c ? c->node() : nullptr;
}

inline HostTerrainCell* terrainCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* h = static_cast<HostTerrainCell*>(ev::handleData(v));
    return (h && h->tag == kHostTerrainTag) ? h : nullptr;
}

inline bool terrainSampleHeight(HostTerrainCell* cell, float x, float z,
                                float rayStartY, float rayLength, float& outY) {
    if (!cell || !cell->mgr()) return false;
    const bromath::Vec3 origin{x, rayStartY, z};
    const bromath::Vec3 dir{0.0f, -1.0f, 0.0f};
    auto hit = cell->mgr()->raycast(origin, dir, rayLength);
    if (!hit.hit) return false;
    outY = hit.worldPos[1];
    return true;
}

void installSceneGraphAgent(ObjectBuilder& b);
void installSceneNodeAgent(ObjectBuilder& b);

}  // namespace bro::bronze_host

extern "C" {
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
                                                bool opts_detailOctaves_given, int32_t opts_detailOctaves);

void* bro_scene_SceneGraph_createTileWorld(void* self, bool opts_chunkSize_given, int32_t opts_chunkSize,
                                          bool opts_tileSize_given, double opts_tileSize,
                                          const char* opts_layers, bool opts_tileAtlas_given, const char* opts_tileAtlas,
                                          bool opts_atlasTileWidth_given, int32_t opts_atlasTileWidth,
                                          bool opts_atlasTileHeight_given, int32_t opts_atlasTileHeight);

void bro_scene_SceneGraph_clearActiveCamera(void* self);
double bro_scene_SceneNode_fov_get(void* self);
void bro_scene_SceneNode_fov_set(void* self, double v);
double bro_scene_SceneNode_near_get(void* self);
void bro_scene_SceneNode_near_set(void* self, double v);
double bro_scene_SceneNode_far_get(void* self);
void bro_scene_SceneNode_far_set(void* self, double v);
const char* bro_scene_SceneNode_projection_get(void* self);
void bro_scene_SceneNode_projection_set(void* self, const char* v);
double bro_scene_SceneNode_aspect_get(void* self);
void bro_scene_SceneNode_aspect_set(void* self, double v);
double bro_scene_SceneNode_orthoHeight_get(void* self);
void bro_scene_SceneNode_play(void* self, const char* clipName, const char* jsonOpts);

bool bro_scene_SceneNode_hasShader(void* self);
const char* bro_scene_SceneNode_setShader(void* self, const char* vertex, const char* fragment,
                                          const char* uniformsJson);
void bro_scene_SceneNode_clearShader(void* self);
void bro_scene_SceneNode_setShaderUniform(void* self, const char* name, const double* vals, uint32_t count);
void bro_scene_SceneNode_setLodMeshes(void* self, const char* jsonLods);
int32_t bro_scene_SceneNode_lodCount(void* self);
int32_t bro_scene_SceneNode_lodLevel(void* self);
void bro_scene_SceneNode_visibilityRange_set(void* self, double begin, double end, double margin);
void bro_scene_SceneNode_visibilityRange_clear(void* self);
void bro_scene_SceneNode_visibilityRange_get(void* self, bronze_native_buffer* out);
bool bro_scene_SceneGraph_isValid(void* self);
void bro_scene_SceneNode_setBaseColorTextureFromScene(void* self, void* sourceScene);
int32_t bro_scene_SceneGraph_raycast_instance(void);
const char* bro_scene_SceneGraph_cullStatsJson(void* self);
void bro_scene_SceneNode_setInstances(void* self, const float* data, uint32_t count);
void bro_scene_SceneNode_setInstancesFromTransforms(void* self, const float* data, uint32_t count);
double bro_scene_SceneNode_instanceCount_get(void* self);
const char* bro_scene_SceneNode_updateMode_get(void* self);
void bro_scene_SceneNode_updateMode_set(void* self, const char* mode);
int32_t bro_scene_SceneNode_resolution_get(void* self);
void bro_scene_SceneNode_resolution_set(void* self, int32_t res);
bool bro_scene_SceneNode_boxProjection_get(void* self);
void bro_scene_SceneNode_boxProjection_set(void* self, bool bp);
double bro_scene_SceneNode_cullMargin_get(void* self);
void bro_scene_SceneNode_cullMargin_set(void* self, double v);
int32_t bro_scene_SceneNode_splatCount_get(void* self);
void bro_scene_SceneNode_setCloud(void* self, const float* pos, uint32_t posCount, const float* scales, uint32_t scaleCount, const float* rots, uint32_t rotCount, const float* opacities, uint32_t opCount, const float* sh, uint32_t shCount, int32_t shDegree);
bool bro_scene_SceneNode_loadSplatPly(void* self, const char* path);
bool bro_scene_SceneNode_savePly(void* self, const char* path);
void bro_scene_SceneNode_worldAnchor_get(void* self, bronze_native_buffer* out);
void bro_scene_SceneNode_worldAnchor_set(void* self, const double* v, uint32_t len);
void bro_scene_SceneNode_addStateMachine(void* self, const char* jsonDef);
void bro_scene_SceneNode_travel(void* self, const char* targetState);
void bro_scene_SceneNode_setRootMotion(void* self, const char* jsonOpts);
const char* bro_scene_SceneNode_consumeRootMotion(void* self);
void bro_scene_SceneNode_play(void* self, const char* clipName, const char* jsonOpts);
void bro_scene_SceneNode_stop(void* self, const char* jsonOpts);
void bro_scene_SceneNode_pause(void* self);
void bro_scene_SceneNode_resume(void* self);
int32_t bro_scene_SceneNode_setSkinningMatrices(void* self, uint64_t matsBits);
void bro_scene_SceneNode_updateMesh(void* self, uint64_t meshBits, bool recomputeNormals);
uint64_t bro_scene_SceneNode_getBoneWorldMatrix(void* self, uint64_t argBits);
const char* bro_scene_SceneNode_blendState(void* self);
void bro_scene_SceneNode_onAnimationFinished_set(void* self, uint64_t cbBits);
const char* bro_scene_SceneNode_type_get(void* self);
const char* bro_scene_SceneNode_kind_get(void* self);
int32_t bro_scene_SceneNode_childCount_get(void* self);
bool bro_scene_SceneNode_castsShadow_get(void* self);
void bro_scene_SceneNode_castsShadow_set(void* self, bool v);
bool bro_scene_SceneNode_receivesShadow_get(void* self);
void bro_scene_SceneNode_receivesShadow_set(void* self, bool v);
double bro_scene_SceneNode_metallic_get(void* self);
void bro_scene_SceneNode_metallic_set(void* self, double v);
double bro_scene_SceneNode_roughness_get(void* self);
void bro_scene_SceneNode_roughness_set(void* self, double v);
double bro_scene_SceneNode_emissive_get(void* self);
void bro_scene_SceneNode_emissive_set(void* self, double v);
void bro_scene_SceneNode_direction_get(void* self, bronze_native_buffer* out);
void bro_scene_SceneNode_direction_set(void* self, const double* v, uint32_t len);
void bro_scene_SceneNode_color_get(void* self, bronze_native_buffer* out);
void bro_scene_SceneNode_color_set(void* self, const double* v, uint32_t len);
double bro_scene_SceneNode_intensity_get(void* self);
void bro_scene_SceneNode_intensity_set(void* self, double v);
double bro_scene_SceneNode_range_get(void* self);
void bro_scene_SceneNode_range_set(void* self, double v);
double bro_scene_SceneNode_innerAngle_get(void* self);
void bro_scene_SceneNode_innerAngle_set(void* self, double v);
double bro_scene_SceneNode_outerAngle_get(void* self);
void bro_scene_SceneNode_outerAngle_set(void* self, double v);
double bro_scene_SceneNode_shadowBias_get(void* self);
void bro_scene_SceneNode_shadowBias_set(void* self, double v);
double bro_scene_SceneNode_shadowNormalBias_get(void* self);
void bro_scene_SceneNode_shadowNormalBias_set(void* self, double v);
int32_t bro_scene_SceneNode_cascadeCount_get(void* self);
void bro_scene_SceneNode_cascadeCount_set(void* self, int32_t v);
double bro_scene_SceneNode_cascadeSplitLambda_get(void* self);
void bro_scene_SceneNode_cascadeSplitLambda_set(void* self, double v);
void bro_scene_SceneGraph_render(void* self);
double bro_scene_SceneGraph_canvasWidth(void* self);
double bro_scene_SceneGraph_canvasHeight(void* self);
void bro_scene_SceneGraph_setCanvasSize(void* self, double w, double h);
void bro_scene_SceneGraph_readTonemapPixels(void* self, bronze_native_buffer* out);
int32_t bro_scene_SceneGraph_readTonemapWidth(void);
int32_t bro_scene_SceneGraph_readTonemapHeight(void);
void* bro_scene_SceneGraph_createMesh(void* self, uint64_t optsBits, uint64_t meshHandle);
void* bro_scene_SceneGraph_createSkinnedMesh(void* self, uint64_t optsBits, uint64_t meshHandle);
void* bro_scene_SceneGraph_createInstancedMesh(void* self, uint64_t optsBits, uint64_t meshHandle);
void* bro_scene_SceneGraph_createShape(void* self, const char* jsonOpts);
void* bro_scene_SceneGraph_createSprite(void* self, const char* jsonOpts);
void* bro_scene_SceneGraph_createPhysicsNode(void* self, const char* jsonOpts);
void* bro_scene_SceneGraph_createParticles3D(void* self, const char* jsonOpts);
void* bro_scene_SceneGraph_createGaussianSplat(void* self, const char* jsonOpts);
void bro_scene_SceneNode_setSkeleton(void* self, uint64_t skelVal);
void bro_scene_SceneNode_addClip(void* self, const char* name, uint64_t clipVal);
void bro_scene_SceneNode_addBlendSpace1D(void* self, const char* name, const char* jsonPoints);
void bro_scene_SceneNode_addBlendSpace2D(void* self, const char* name, const char* jsonPoints);
void bro_scene_SceneNode_setBlendPos(void* self, const char* name, double x, bool hasY, double y);
void bro_scene_SceneNode_playLayer(void* self, int32_t layerIndex, const char* clipName, const char* jsonOpts);
void bro_scene_SceneNode_stopLayer(void* self, int32_t layerIndex, bool hasFade, double fadeTime);
void bro_scene_SceneNode_setLayerWeight(void* self, int32_t layerIndex, double weight);
void bro_scene_SceneNode_onStateChanged_set(void* self, uint64_t cbBits);
}

namespace bro::bronze_host {
bool registerSceneShaderNatives(std::string* error);
bool registerSceneCameraNatives(std::string* error);
// native_scene_extras.cpp: the per-type accessors and instanced-mesh
// operations js/scene_extras.js wraps.
bool registerSceneExtraNatives(std::string* error);
}

namespace scene = bro::scene;
namespace engine = bro::engine;
namespace tile = bro::tile;
