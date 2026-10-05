#include "scene/vulkan/scene_lighting.h"

#include "scene/depth_policy.h"
#include "scene/instanced_mesh_node.h"
#include "scene/light_node.h"
#include "scene/mesh_node.h"
#include "scene/reflection_probe_node.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_environment.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_device.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

void setLight(SceneLightUniform& out, const LightNode& l) {
    const auto& m = l.worldMatrix();
    out.position[0] = m.at(0, 3);
    out.position[1] = m.at(1, 3);
    out.position[2] = m.at(2, 3);
    out.position[3] = static_cast<float>(l.kind());
    bromath::Vec3 d = l.direction();
    const float len = std::sqrt(bromath::vdot(d, d));
    if (len > 1e-6f) d = d * (1.0f / len);
    out.direction[0] = d.x;
    out.direction[1] = d.y;
    out.direction[2] = d.z;
    out.direction[3] = l.range();
    const auto& c = l.color();
    out.color[0] = c.x;
    out.color[1] = c.y;
    out.color[2] = c.z;
    out.color[3] = l.intensity();
    out.shadow[0] = std::cos(l.innerAngle());
    out.shadow[1] = std::cos(l.outerAngle());
    out.shadow[2] = -1.0f;
}

}  // namespace

SceneLightingUniforms sceneLighting(const SceneRenderer& renderer, const SceneEnvironment& environment) {
    SceneLightingUniforms light{};
    const ShadowPlan& plan = renderer.shadowPlan();
    const std::vector<LightNode*>& lights = renderer.frameLights();
    const int count = std::min(static_cast<int>(lights.size()), kSceneMaxLights);

    // The sun (decals light by it alone): the first directional light,
    // unless a later one casts shadows and it does not.
    const LightNode* sun = nullptr;
    for (int i = 0; i < count; ++i) {
        const LightNode& l = *lights[i];
        setLight(light.lights[i], l);
        if (i < ShadowPlan::kMaxLights && plan.lightSlot[i] >= 0 && plan.lightSlotCount[i] > 0) {
            light.lights[i].shadow[2] = static_cast<float>(plan.lightSlot[i]);
            light.lights[i].shadow[3] = static_cast<float>(plan.lightSlotCount[i]);
            std::memcpy(light.lights[i].cascadeSplit, plan.cascadeSplit[i], sizeof(light.lights[i].cascadeSplit));
        }
        if (l.kind() != LightNode::Kind::Directional) continue;
        if (!sun || (l.castsShadow() && !sun->castsShadow())) sun = &l;
    }
    if (sun) {
        bromath::Vec3 dir = sun->direction();
        const float len = std::sqrt(bromath::vdot(dir, dir));
        if (len > 1e-6f) dir = dir * (1.0f / len);
        light.sunDirection[0] = dir.x;
        light.sunDirection[1] = dir.y;
        light.sunDirection[2] = dir.z;
        light.sunDirection[3] = 1.0f;
        const auto& c = sun->color();
        light.sunColor[0] = c.x;
        light.sunColor[1] = c.y;
        light.sunColor[2] = c.z;
        light.sunColor[3] = sun->intensity();
    }

    const float* amb = renderer.effectiveAmbient();
    light.ambientColor[0] = amb[0];
    light.ambientColor[1] = amb[1];
    light.ambientColor[2] = amb[2];
    light.ambientColor[3] = 1.0f;

    const int tiles = std::min(plan.tileCount, kSceneMaxShadowTiles);
    light.params[0] = static_cast<float>(count);
    light.params[1] = static_cast<float>(plan.pcfTaps);
    light.params[2] = plan.atlasSize > 0 ? 1.0f / static_cast<float>(plan.atlasSize) : 0.0f;
    light.params[3] = static_cast<float>(tiles);

    // Clip -> (tile uv, depth): x,y from [-1,1] to [0,1]; the clip y flip of
    // toVulkanClip matches the flip the shadow pass rendered the tile with.
    bromath::Mat4 toUv = bromath::midentity();
    toUv.at(0, 0) = 0.5f;
    toUv.at(1, 1) = 0.5f;
    toUv.at(0, 3) = 0.5f;
    toUv.at(1, 3) = 0.5f;
    for (int t = 0; t < tiles; ++t) {
        const ShadowTilePlan& tile = plan.tiles[t];
        SceneShadowTileUniform& out = light.shadows[t];
        const bromath::Mat4 m = bromath::mmul(toUv, toVulkanClip(tile.viewProj));
        std::memcpy(out.matrix, m.data, sizeof(out.matrix));
        std::memcpy(out.rect, tile.rect, sizeof(out.rect));
        out.bias[0] = tile.bias;
        out.bias[1] = tile.normalBias;
        out.bias[2] = tile.texelConst;
        out.bias[3] = tile.texelPerMetre;
        out.depth[0] = tile.zNear;
        out.depth[1] = tile.zFar;
        out.depth[2] = tile.ortho ? 1.0f : 0.0f;
    }
    environment.fillLighting(light, renderer);
    return light;
}

void writeCameraSet(SceneFrame& frame) {
    const SceneCameraUniforms cam = frame.view.uniforms(&frame.renderer);
    const VkDescriptorBufferInfo camInfo = frame.gpu.device.frameUniform(&cam, sizeof(cam));
    frame.cameraSet = frame.gpu.device.frameSet(frame.gpu.defaults.cameraLayout);
    SceneVkDescriptorWriter camWriter;
    camWriter.writeBuffer(0, camInfo.buffer, camInfo.range, camInfo.offset);
    camWriter.updateSet(frame.gpu.device.device(), frame.cameraSet);
}

VkDescriptorSet writeLightingSet(SceneGpu& gpu, const SceneLightingUniforms& uniforms, VkImageView probeView,
                                 const SceneVkImage* shadeMap) {
    const SceneDefaults& d = gpu.defaults;
    const VkDescriptorBufferInfo ubo = gpu.device.frameUniform(&uniforms, sizeof(uniforms));
    VkDescriptorSet set = gpu.device.frameSet(d.lightingLayout);
    SceneVkDescriptorWriter writer;
    writer.writeBuffer(0, ubo.buffer, ubo.range, ubo.offset);
    writer.writeImage(1, gpu.targets.shadowAtlas.view, VK_NULL_HANDLE);   // immutable compare sampler
    writer.writeImage(2, probeView ? probeView : d.cube.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    writer.writeImage(3, shadeMap ? shadeMap->view : d.white.view, VK_NULL_HANDLE,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    gpu.environment.writeBindings(writer, d);
    writer.updateSet(gpu.device.device(), set);
    return set;
}

void PassFrameUniforms::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(frame.gpu.targets.shadowAtlas);
}

void setProbe(SceneLightingUniforms& light, const ReflectionProbeNode& probe, uint32_t mipLevels) {
    const auto& pw = probe.worldMatrix();
    const bromath::Mat4 invPw = bromath::minverse(pw);
    std::memcpy(light.probeWorldToLocal, invPw.data, sizeof(light.probeWorldToLocal));
    std::memcpy(light.probeLocalToWorld, pw.data, sizeof(light.probeLocalToWorld));
    light.probePos[0] = pw.at(0, 3);
    light.probePos[1] = pw.at(1, 3);
    light.probePos[2] = pw.at(2, 3);
    light.probePos[3] = 1.0f;
    auto axisLength = [&](int c) {
        return std::sqrt(pw.at(0, c) * pw.at(0, c) + pw.at(1, c) * pw.at(1, c) + pw.at(2, c) * pw.at(2, c));
    };
    light.probeBoxSize[0] = axisLength(0);
    light.probeBoxSize[1] = axisLength(1);
    light.probeBoxSize[2] = axisLength(2);
    light.probeBoxSize[3] = probe.boxProjection() ? 1.0f : 0.0f;
    light.probeParams[0] = probe.intensity();
    light.probeParams[1] = probe.interior();
    light.probeParams[2] = static_cast<float>(mipLevels - 1);
}

void PassFrameUniforms::record(SceneFrame& frame) {
    SceneLightingUniforms& light = frame.lighting;

    // The first visible node with a shade map provides it for the scene.
    ShadeMapBinding binding{};
    bool hasShade = false;
    for (const auto& [id, node] : frame.graph.nodes()) {
        if (!node->renderVisible()) continue;
        const ShadeMapProvider* provider = nullptr;
        if (node->type() == SceneNode::Type::Mesh) {
            provider = static_cast<MeshNode*>(node.get())->shadeMap();
        } else if (node->type() == SceneNode::Type::InstancedMesh) {
            provider = static_cast<InstancedMeshNode*>(node.get())->shadeMap();
        }
        if (provider && (*provider)(binding) && binding.pixels && binding.width > 0 && binding.height > 0) {
            hasShade = true;
            break;
        }
    }
    const SceneVkImage* shade = hasShade ? frame.gpu.resources.shadeMap(binding) : nullptr;
    if (shade) {
        light.shadeOrigin[0] = binding.origin.x;
        light.shadeOrigin[1] = binding.origin.y;
        light.shadeOrigin[2] = binding.origin.z;
        light.shadeOrigin[3] = 1.0f;
        light.shadeParams[0] = binding.cellSize;
        light.shadeParams[1] = binding.hex ? 1.0f : 0.0f;
        light.shadeParams[2] = static_cast<float>(binding.width);
        light.shadeParams[3] = static_cast<float>(binding.height);
    }

    frame.shadeMap = shade;
    frame.lightingSet = writeLightingSet(frame.gpu, light, VK_NULL_HANDLE, shade);
}

}  // namespace bro::scene::vk
