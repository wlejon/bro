#include "scene/vulkan/pass_mesh.h"

#include "scene/mesh_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_mesh_drawer.h"

#include <cstring>

namespace bro::scene::vk {

void PassOpaque::declare(const SceneFrame& frame, PassIO& io) const {
    io.hdr({.indirect = frame.ssao});
}

void PassOpaque::record(SceneFrame& frame) {
    for (uint32_t i : frame.lists.opaque) {
        frame.gpu.meshes.record(frame.cmd, frame.hdrTarget, frame.cameraSet, frame.lightingSet,
                                frame.lists.meshes[i]);
    }
}

bool PassTranslucent::active(const SceneFrame& frame) const {
    return !frame.lists.translucent.empty();
}

void PassTranslucent::declare(const SceneFrame&, PassIO& io) const {
    io.hdr();
}

void PassTranslucent::record(SceneFrame& frame) {
    for (uint32_t i : frame.lists.translucent) {
        frame.gpu.meshes.record(frame.cmd, frame.hdrTarget, frame.cameraSet, frame.lightingSet,
                                frame.lists.meshes[i]);
    }
}

bool PassGizmo::active(const SceneFrame& frame) const {
    return frame.graph.hasGizmoProvider();
}

void PassGizmo::declare(const SceneFrame&, PassIO& io) const {
    io.hdr();
}

void PassGizmo::record(SceneFrame& frame) {
    for (MeshNode* gizmo : frame.graph.gizmoMeshes()) {
        if (!gizmo || gizmo->mesh().empty()) continue;
        const GpuMesh* gm = frame.gpu.resources.mesh(gizmo->id(), 0, gizmo->geometryGeneration(), gizmo->mesh());
        if (!gm) continue;

        MeshDraw draw;
        draw.kind = MeshKind::Static;
        draw.nodeId = gizmo->id();
        draw.vertices = gm->vertices.buffer;
        draw.indices = gm->indices.buffer;
        draw.indexCount = gm->indexCount;
        std::memcpy(draw.push.model, gizmo->worldMatrix().data, sizeof(draw.push.model));
        std::memcpy(draw.push.baseColor, gizmo->color(), sizeof(draw.push.baseColor));
        std::memcpy(draw.push.emissive, gizmo->emissiveColor(), 3 * sizeof(float));
        draw.push.emissive[3] = gizmo->emissive();
        draw.push.pbrParams[0] = 0.0f;   // metallic
        draw.push.pbrParams[1] = 1.0f;   // roughness
        draw.push.pbrParams[3] = static_cast<float>(mesh_flags::kUnlit);
        frame.gpu.meshes.record(frame.cmd, frame.hdrTarget, frame.cameraSet, frame.lightingSet, draw);
        frame.drewContent = true;
    }
}

}  // namespace bro::scene::vk
