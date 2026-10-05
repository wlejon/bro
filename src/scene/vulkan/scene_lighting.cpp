#include "scene/vulkan/scene_lighting.h"

#include "scene/depth_policy.h"
#include "scene/instanced_mesh_node.h"
#include "scene/light_node.h"
#include "scene/mesh_node.h"
#include "scene/reflection_probe_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_device.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

constexpr size_t kMaxPointLights = 16;

// The directional shadow: one orthographic map centred a little ahead of the
// camera (the cascade planner's tiles are not drawn yet; see SceneRenderer).
constexpr float kShadowCenterAhead = 8.0f;
constexpr float kShadowLightDistance = 25.0f;
constexpr float kShadowHalfExtent = 12.0f;
constexpr float kShadowNear = 1.0f;
constexpr float kShadowFar = 50.0f;

void setLight(ScenePointLight& out, const LightNode& l) {
    if (l.kind() == LightNode::Kind::Directional) {
        const bromath::Vec3 dir = bromath::vnorm(l.direction());
        out.position[0] = dir.x;
        out.position[1] = dir.y;
        out.position[2] = dir.z;
        out.position[3] = -1.0f;
    } else {
        const auto& m = l.worldMatrix();
        out.position[0] = m.at(0, 3);
        out.position[1] = m.at(1, 3);
        out.position[2] = m.at(2, 3);
        out.position[3] = l.range();
    }
    const auto& c = l.color();
    out.color[0] = c.x;
    out.color[1] = c.y;
    out.color[2] = c.z;
    out.color[3] = l.intensity();
}

}  // namespace

SceneLightingUniforms sceneLighting(const SceneGraph& graph, const SceneRenderer& renderer, const SceneView& view,
                                    bool& shadowed) {
    SceneLightingUniforms light{};
    const LightNode* sun = nullptr;
    std::vector<const LightNode*> others;
    for (const auto& [id, node] : graph.nodes()) {
        if (!node->renderVisible() || node->type() != SceneNode::Type::Light) continue;
        const auto* l = static_cast<const LightNode*>(node.get());
        if (l->kind() == LightNode::Kind::Directional) {
            // The first directional light is the sun, unless a later one
            // casts shadows and it does not.
            if (!sun) {
                sun = l;
            } else if (l->castsShadow() && !sun->castsShadow()) {
                others.push_back(sun);
                sun = l;
            } else {
                others.push_back(l);
            }
        } else if (l->kind() == LightNode::Kind::Point || l->kind() == LightNode::Kind::Spot) {
            others.push_back(l);
        }
    }

    shadowed = false;
    if (sun) {
        const bromath::Vec3 dir = bromath::vnorm(sun->direction());
        light.sunDirection[0] = dir.x;
        light.sunDirection[1] = dir.y;
        light.sunDirection[2] = dir.z;
        light.sunDirection[3] = 1.0f;
        const auto& c = sun->color();
        light.sunColor[0] = c.x;
        light.sunColor[1] = c.y;
        light.sunColor[2] = c.z;
        light.sunColor[3] = sun->intensity();
        light.numLights[0] = 1.0f;
        shadowed = sun->castsShadow();
    } else if (others.empty()) {
        // No lights at all: an implicit sun, so meshes are never black.
        const bromath::Vec3 dir = bromath::vnorm(bromath::Vec3{-0.3f, -1.0f, -0.5f});
        light.sunDirection[0] = dir.x;
        light.sunDirection[1] = dir.y;
        light.sunDirection[2] = dir.z;
        light.sunDirection[3] = 1.0f;
        light.sunColor[0] = 1.0f;
        light.sunColor[1] = 0.98f;
        light.sunColor[2] = 0.95f;
        light.sunColor[3] = 3.0f;
        light.numLights[0] = 1.0f;
    }
    light.numLights[2] = shadowed ? 1.0f : 0.0f;

    const size_t count = std::min(others.size(), kMaxPointLights);
    light.numLights[1] = static_cast<float>(count);
    for (size_t i = 0; i < count; ++i) setLight(light.pointLights[i], *others[i]);

    const float* amb = renderer.effectiveAmbient();
    light.ambientColor[0] = amb[0];
    light.ambientColor[1] = amb[1];
    light.ambientColor[2] = amb[2];
    light.ambientColor[3] = 1.0f;

    const bromath::Vec3 lightDir{light.sunDirection[0], light.sunDirection[1], light.sunDirection[2]};
    const bromath::Vec3 center = view.eye + view.forward() * kShadowCenterAhead;
    const bromath::Vec3 lightEye = center - lightDir * kShadowLightDistance;
    const bromath::Vec3 up = std::abs(lightDir.y) > 0.99f ? bromath::Vec3{0, 0, 1} : bromath::Vec3{0, 1, 0};
    const bromath::Mat4 lightView = bromath::mlookAt(lightEye, center, up);
    const bromath::Mat4 lightProj = toVulkanClip(makeOrthoZeroToOne(-kShadowHalfExtent, kShadowHalfExtent,
                                                                    -kShadowHalfExtent, kShadowHalfExtent,
                                                                    kShadowNear, kShadowFar));
    const bromath::Mat4 lightVP = bromath::mmul(lightProj, lightView);
    std::memcpy(light.shadowCascadeProj, lightVP.data, sizeof(light.shadowCascadeProj));
    return light;
}

VkDescriptorSet writeLightingSet(SceneGpu& gpu, const SceneLightingUniforms& uniforms, VkImageView probeView,
                                 const SceneVkImage* shadeMap) {
    const SceneDefaults& d = gpu.defaults;
    const VkDescriptorBufferInfo ubo = gpu.device.frameUniform(&uniforms, sizeof(uniforms));
    VkDescriptorSet set = gpu.device.frameSet(d.lightingLayout);
    SceneVkDescriptorWriter writer;
    writer.writeBuffer(0, ubo.buffer, ubo.range, ubo.offset);
    writer.writeImage(1, gpu.targets.shadow.view, VK_NULL_HANDLE);   // immutable compare sampler
    writer.writeImage(2, probeView ? probeView : d.cube.view, d.cubeSampler);
    writer.writeImage(3, shadeMap ? shadeMap->view : d.white.view, shadeMap ? shadeMap->sampler : d.sampler);
    writer.updateSet(gpu.device.device(), set);
    return set;
}

void PassFrameUniforms::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(frame.gpu.targets.shadow);
}

void PassFrameUniforms::record(SceneFrame& frame) {
    SceneLightingUniforms& light = frame.lighting;

    if (const ReflectionProbeNode* probe = frame.probe.node) {
        const auto& pw = probe->worldMatrix();
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
        light.probeBoxSize[3] = probe->boxProjection() ? 1.0f : 0.0f;
        light.probeParams[0] = probe->intensity();
        light.probeParams[1] = probe->interior();
        light.probeParams[2] = static_cast<float>(frame.probe.mipLevels - 1);
    }

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

    const SceneCameraUniforms cam = frame.view.uniforms(&frame.renderer);
    const VkDescriptorBufferInfo camInfo = frame.gpu.device.frameUniform(&cam, sizeof(cam));
    frame.cameraSet = frame.gpu.device.frameSet(frame.gpu.defaults.cameraLayout);
    SceneVkDescriptorWriter camWriter;
    camWriter.writeBuffer(0, camInfo.buffer, camInfo.range, camInfo.offset);
    camWriter.updateSet(frame.gpu.device.device(), frame.cameraSet);

    frame.lightingSet = writeLightingSet(frame.gpu, light, frame.probe.view, shade);
}

}  // namespace bro::scene::vk
