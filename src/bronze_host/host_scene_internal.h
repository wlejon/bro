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

namespace bro::scene { class TerrainManager; }

namespace bro::bronze_host {

inline constexpr uint32_t kHostSceneGraphTag   = 0x53434E47u;  // 'SCNG'
inline constexpr uint32_t kHostSceneNodeTag    = 0x534E4F44u;  // 'SNOD'
inline constexpr uint32_t kHostSceneTextureTag = 0x53544558u;  // 'STEX'
inline constexpr uint32_t kHostTerrainTag      = 0x54455252u;  // 'TERR'

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

extern HostClass g_sceneGraphClass;
extern HostClass g_sceneNodeClass;
extern HostClass g_sceneTextureClass;
extern HostClass g_terrainClass;

void ensureSceneClassesInstalled();
void ensureTerrainClassInstalled();
bool terrainSampleHeight(void* handle, float x, float z,
                         float rayStartY, float rayLength, float& outY);
Value createSceneGraphValue(scene::SceneGraph* sg, dom::Element* canvas);
Value wrapSceneNode(scene::SceneNode* node, scene::SceneGraph* graph);
Value wrapSceneTexture(std::shared_ptr<scene::SceneGraph::OutputTextureSource> src);
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

void installRegisterCapability(ObjectBuilder& b);

}  // namespace bro::bronze_host
