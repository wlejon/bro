#pragma once

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/light_node.h"
#include "scene/mesh_node.h"

#include <memory>
#include <string>
#include <vector>
#include <span>
#include <unordered_map>

namespace bro::scene {
class TerrainManager;
class Tween;
class ClipPlayer;
class TileWorld;
class ClipmapTerrain;
}

namespace bro::bronze_host {

inline constexpr uint32_t kHostSceneGraphTag   = 0x53434E47u;  // 'SCNG'
inline constexpr uint32_t kHostSceneNodeTag    = 0x534E4F44u;  // 'SNOD'
inline constexpr uint32_t kHostSceneTextureTag = 0x53544558u;  // 'STEX'
inline constexpr uint32_t kHostTerrainTag      = 0x54455252u;  // 'TERR'
inline constexpr uint32_t kHostTweenTag        = 0x5457454Eu;  // 'TWEN'
inline constexpr uint32_t kHostClipPlayerTag   = 0x434C4950u;  // 'CLIP'
inline constexpr uint32_t kHostTileWorldTag    = 0x54494C45u;  // 'TILE'
inline constexpr uint32_t kHostClipmapTag      = 0x434C494Du;  // 'CLIM'

struct HostTerrainCell {
    uint32_t tag = kHostTerrainTag;
    std::unique_ptr<bro::scene::TerrainManager> manager;
    ev::Persistent heightSource;
    bool hasHeightSource = false;
};

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

struct HostSceneTextureCell {
    uint32_t tag = kHostSceneTextureTag;
    std::weak_ptr<scene::SceneGraph::OutputTextureSource> src;
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

struct HostTileWorldCell {
    uint32_t tag = kHostTileWorldTag;
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;
    std::unique_ptr<scene::TileWorld> world;

    scene::SceneGraph* graph() const {
        auto t = token.lock();
        return t ? t->graph : nullptr;
    }
};

struct HostClipmapCell {
    uint32_t tag = kHostClipmapTag;
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;
    std::unique_ptr<scene::ClipmapTerrain> terrain;

    scene::SceneGraph* graph() const {
        auto t = token.lock();
        return t ? t->graph : nullptr;
    }
};

inline HostSceneGraphCell* sceneGraphCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* h = static_cast<HostSceneGraphCell*>(ev::handleData(v));
    return (h && h->tag == kHostSceneGraphTag) ? h : nullptr;
}

inline scene::SceneGraph* sceneGraphOf(Value v) {
    auto* c = sceneGraphCellOf(v);
    return c ? c->graph() : nullptr;
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

inline HostSceneTextureCell* sceneTextureCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* h = static_cast<HostSceneTextureCell*>(ev::handleData(v));
    return (h && h->tag == kHostSceneTextureTag) ? h : nullptr;
}

inline HostTerrainCell* terrainCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* h = static_cast<HostTerrainCell*>(ev::handleData(v));
    return (h && h->tag == kHostTerrainTag) ? h : nullptr;
}

inline HostTweenCell* tweenCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* h = static_cast<HostTweenCell*>(ev::handleData(v));
    return (h && h->tag == kHostTweenTag) ? h : nullptr;
}

inline scene::Tween* tweenOf(Value v) {
    auto* c = tweenCellOf(v);
    return c ? c->tween() : nullptr;
}

inline HostClipPlayerCell* clipPlayerCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* h = static_cast<HostClipPlayerCell*>(ev::handleData(v));
    return (h && h->tag == kHostClipPlayerTag) ? h : nullptr;
}

inline scene::ClipPlayer* clipPlayerOf(Value v) {
    auto* c = clipPlayerCellOf(v);
    return c ? c->player() : nullptr;
}

inline HostTileWorldCell* tileWorldCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* h = static_cast<HostTileWorldCell*>(ev::handleData(v));
    return (h && h->tag == kHostTileWorldTag) ? h : nullptr;
}

inline scene::TileWorld* tileWorldOf(Value v) {
    auto* c = tileWorldCellOf(v);
    return c ? c->world.get() : nullptr;
}

inline HostClipmapCell* clipmapCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* h = static_cast<HostClipmapCell*>(ev::handleData(v));
    return (h && h->tag == kHostClipmapTag) ? h : nullptr;
}

inline scene::ClipmapTerrain* clipmapOf(Value v) {
    auto* c = clipmapCellOf(v);
    return c ? c->terrain.get() : nullptr;
}

extern HostClass g_sceneGraphClass;
extern HostClass g_sceneNodeClass;
extern HostClass g_sceneTextureClass;
extern HostClass g_terrainClass;
extern HostClass g_tweenClass;
extern HostClass g_clipPlayerClass;
extern HostClass g_tileWorldClass;
extern HostClass g_clipmapClass;

void ensureSceneClassesInstalled();
void ensureTerrainClassInstalled();
void ensureTweenClassInstalled();
void ensureClipPlayerClassInstalled();
void ensureTileWorldClassInstalled();
void ensureClipmapClassInstalled();

bool terrainSampleHeight(void* handle, float x, float z,
                         float rayStartY, float rayLength, float& outY);
Value createSceneGraphValue(scene::SceneGraph* sg, dom::Element* canvas);
Value wrapSceneNode(scene::SceneNode* node, scene::SceneGraph* graph);
Value wrapSceneTexture(std::shared_ptr<scene::SceneGraph::OutputTextureSource> src);
Value wrapTween(scene::Tween* tween, scene::SceneGraph* graph);
Value wrapClipPlayer(scene::ClipPlayer* player, scene::SceneGraph* graph);
Value wrapTileWorld(std::unique_ptr<scene::TileWorld> world, scene::SceneGraph* graph);
Value wrapClipmap(std::unique_ptr<scene::ClipmapTerrain> terrain, scene::SceneGraph* graph);
void pruneSceneNodeWrapper(const scene::SceneGraph::LivenessToken* tok, uint32_t id);

Value vec3ToValue(const bromath::Vec3& v);
Value quatToValue(const bromath::Quat& q);
Value mat4ToValue(const bromath::Mat4& m);
bool readVec3FromValue(Value v, bromath::Vec3& out);
bool readQuatFromValue(Value v, bromath::Quat& out);
bool parseColorValue(Value v, float& r, float& g, float& b, float& a);
Value buildImageDataValue(int w, int h, const std::vector<uint8_t>& pixels);

void installSceneGraphCore(ObjectBuilder& b);
void installSceneGraphEnv(ObjectBuilder& b);
void installSceneGraphMesh(ObjectBuilder& b);
void installSceneGraphLights(ObjectBuilder& b);
void installSceneGraphFx(ObjectBuilder& b);
void installSceneGraph2D(ObjectBuilder& b);
void installSceneGraphTerrain(ObjectBuilder& b);
void installSceneGraphAgent(ObjectBuilder& b);

void installSceneNodeCore(ObjectBuilder& b);
void installSceneNodeMesh(ObjectBuilder& b);
void installSceneNodeLights(ObjectBuilder& b);
void installSceneNodeFx(ObjectBuilder& b);
void installSceneNode2D(ObjectBuilder& b);
void installSceneNodeAgent(ObjectBuilder& b);
void installSceneNodeAnim(ObjectBuilder& b);

void installSceneShader(ObjectBuilder& bNode, ObjectBuilder& bGraph);
void installSceneParticles(ObjectBuilder& bNode, ObjectBuilder& bGraph);
void installSceneTileWorld(ObjectBuilder& bGraph);
void installTileWorldExtra(ObjectBuilder& b);
void installSceneClipmap(ObjectBuilder& bGraph);
Value makeBroGizmoValue();

void installRegisterCapability(ObjectBuilder& b);

}  // namespace bro::bronze_host
