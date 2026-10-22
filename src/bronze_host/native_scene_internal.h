#pragma once

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_builder.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/nodes/mesh_node.h"
#include "scene/nodes/skinned_mesh_node.h"
#include "scene/nodes/instanced_mesh_node.h"
#include "scene/nodes/light_node.h"
#include "scene/nodes/camera_node.h"
#include "scene/nodes/physics_node.h"
#include "scene/nodes/shape_node.h"
#include "scene/nodes/sprite_node.h"
#include "scene/nodes/html_node.h"
#include "scene/nodes/particle_node.h"
#include "scene/nodes/particles3d_node.h"
#include "scene/nodes/gaussian_splat_node.h"
#include "scene/nodes/decal_node.h"
#include "scene/nodes/reflection_probe_node.h"
#include "scene/nodes/terrain_manager.h"
#include "scene/nodes/tile_world.h"
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
    mutable scene::SceneNode* cachedNode = nullptr;
    mutable uint64_t cachedDestroyGen = 0;

    scene::SceneGraph* graph() const {
        auto t = token.lock();
        return t ? t->graph : nullptr;
    }

    scene::SceneNode* node() const {
        auto t = token.lock();
        if (!t || !t->graph) return nullptr;
        if (cachedDestroyGen == t->destroyGeneration) return cachedNode;
        cachedNode = t->graph->resolveNode(id);
        cachedDestroyGen = t->destroyGeneration;
        return cachedNode;
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
    return new HostSceneNodeCell{kHostSceneNodeTag, tok, node->id(), node, tok->destroyGeneration};
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
bool bro_scene_SceneNode_savePly(void* self, const char* path);
void bro_scene_SceneNode_play(void* self, const char* clipName, const char* jsonOpts);
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
