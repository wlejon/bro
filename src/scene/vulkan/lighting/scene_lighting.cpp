#include "scene/vulkan/lighting/scene_lighting.h"

#include "scene/depth_policy.h"
#include "scene/nodes/instanced_mesh_node.h"
#include "scene/nodes/light_node.h"
#include "scene/nodes/mesh_node.h"
#include "scene/nodes/reflection_probe_node.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/lighting/scene_atmosphere.h"
#include "scene/vulkan/lighting/scene_environment.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_gpu_resources.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/shadow/scene_shadow_pages.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

void setLight(SceneLightUniform& out, const LightNode& l, const bromath::Vec3& eye) {
    const auto& m = l.worldMatrix();
    out.position[0] = m.at(0, 3) - eye.x;
    out.position[1] = m.at(1, 3) - eye.y;
    out.position[2] = m.at(2, 3) - eye.z;
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

void shadowMapUniform(const ShadowPlan& plan, const ShadowMapPlan& map, const bromath::Vec3& eye,
                      SceneShadowMapUniform& out) {
    // Clip -> (map uv, depth): x,y from [-1,1] to [0,1]; the clip y flip of
    // toVulkanClip matches the flip the shadow pass drew the pages with.
    // The maps are relative to the plan's eye; positions here to `eye`.
    bromath::Mat4 toUv = bromath::midentity();
    toUv.at(0, 0) = 0.5f;
    toUv.at(1, 1) = 0.5f;
    toUv.at(0, 3) = 0.5f;
    toUv.at(1, 3) = 0.5f;
    const bromath::Mat4 m = bromath::mmul(toUv, toVulkanClip(rebased(map.relViewProj, eye - plan.origin)));
    std::memcpy(out.matrix, m.data, sizeof(out.matrix));
    out.info[0] = static_cast<float>(map.firstEntry);
    out.info[1] = static_cast<float>(map.kind);
    out.info[2] = static_cast<float>(map.originX);
    out.info[3] = static_cast<float>(map.originY);
    out.bias[0] = map.bias;
    out.bias[1] = map.normalBias;
    out.bias[2] = map.texel;
    out.bias[3] = map.texelPerMetre;
    out.depth[0] = map.zNear;
    out.depth[1] = map.zFar;
    out.depth[2] = map.ortho ? 1.0f : 0.0f;
    out.depth[3] = static_cast<float>(map.level);
}

SceneLightingUniforms sceneLighting(const SceneRenderer& renderer, const SceneGpu& gpu, const bromath::Vec3& eye) {
    SceneLightingUniforms light{};
    const ShadowPlan& plan = renderer.shadowPlan();
    const std::vector<LightNode*>& lights = renderer.frameLights();
    const int count = std::min(static_cast<int>(lights.size()), kSceneMaxLights);

    // The sun (decals light by it alone): the first directional light,
    // unless a later one casts shadows and it does not.
    const LightNode* sun = nullptr;
    for (int i = 0; i < count; ++i) {
        const LightNode& l = *lights[i];
        setLight(light.lights[i], l, eye);
        if (i < ShadowPlan::kMaxLights && plan.lightMap[i] >= 0 && plan.lightMapCount[i] > 0 && gpu.shadows.ready()) {
            light.lights[i].shadow[2] = static_cast<float>(plan.lightMap[i]);
            light.lights[i].shadow[3] = static_cast<float>(plan.lightMapCount[i]);
            light.lights[i].shadowReach[0] = plan.lightReach[i];
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

    const float* amb = renderer.ambient();
    light.ambientColor[0] = amb[0];
    light.ambientColor[1] = amb[1];
    light.ambientColor[2] = amb[2];
    light.ambientColor[3] = 1.0f;

    light.params[0] = static_cast<float>(count);
    light.params[1] = static_cast<float>(plan.pcfTaps);
    light.params[2] = gpu.shadows.ready() ? 1.0f / static_cast<float>(gpu.shadows.pool.width) : 0.0f;
    light.params[3] = static_cast<float>(plan.poolColumns);
    light.shadowView[0] = plan.pixelAngle;
    light.shadowView[1] = plan.orthoPixel;
    light.shadowView[2] = plan.finestTexel;
    // The sway level, or -1 when the frame draws no overlay (lit shaders
    // then read none).
    bool overlays = false;
    for (int v = 0; v < plan.viewCount; ++v) overlays = overlays || plan.views[v].dynamic;
    light.shadowView[3] = overlays ? static_cast<float>(plan.swayLevel) : -1.0f;
    for (int m = 0; m < plan.mapCount && m < kSceneMaxShadowMaps; ++m)
        shadowMapUniform(plan, plan.maps[m], eye, light.shadowMaps[m]);
    gpu.environment.fillLighting(light, renderer);
    gpu.atmosphere.fill(light.atm, eye);
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
    // The shadow pages' pool and overlays, through binding 16's compare sampler.
    writer.writeImage(1, gpu.shadows.poolView(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    writer.writeImage(15, gpu.shadows.scratchView(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    writer.writeImage(2, probeView ? probeView : d.cube.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    writer.writeImage(3, shadeMap ? shadeMap->view : d.white.view, VK_NULL_HANDLE,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    gpu.environment.writeBindings(writer, d);
    gpu.atmosphere.writeBindings(writer);
    const VkDescriptorBufferInfo table = gpu.shadows.table();
    writer.writeBuffer(14, table.buffer, table.range, table.offset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.updateSet(gpu.device.device(), set);
    return set;
}

namespace {

void setProbe(SceneLightingUniforms& light, const ReflectionProbeNode& probe, uint32_t mipLevels,
              const bromath::Vec3& eye) {
    const bromath::Mat4 pw = eyeRelative(probe.worldMatrix(), eye);
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

void setShade(SceneLightingUniforms& light, const ShadeMapBinding& binding, const bromath::Vec3& eye) {
    light.shadeOrigin[0] = binding.origin.x - eye.x;
    light.shadeOrigin[1] = binding.origin.y - eye.y;
    light.shadeOrigin[2] = binding.origin.z - eye.z;
    light.shadeOrigin[3] = 1.0f;
    light.shadeParams[0] = binding.cellSize;
    light.shadeParams[1] = binding.hex ? 1.0f : 0.0f;
    light.shadeParams[2] = static_cast<float>(binding.width);
    light.shadeParams[3] = static_cast<float>(binding.height);
}

}  // namespace

VkDescriptorSet lightingSetFor(SceneFrame& frame, const ProbeLighting* probe, const ShadeMapBinding* shade) {
    if (!probe && !shade) return frame.lightingSet;
    const auto key = std::make_pair(probe ? probe->node->id() : 0u, shade ? shade->pixels : nullptr);
    if (auto it = frame.lightingSets.find(key); it != frame.lightingSets.end()) return it->second;

    SceneLightingUniforms light = frame.lighting;
    if (probe) setProbe(light, *probe->node, probe->mipLevels, frame.view.eye);
    const SceneVkImage* shadeImage = shade ? frame.gpu.resources.shadeMap(*shade) : nullptr;
    if (shadeImage) setShade(light, *shade, frame.view.eye);
    VkDescriptorSet set = writeLightingSet(frame.gpu, light, probe ? probe->view : VK_NULL_HANDLE, shadeImage);
    frame.lightingSets.emplace(key, set);
    return set;
}

VkDescriptorSet shadedLightingSet(SceneGpu& gpu, const SceneLightingUniforms& base, const ShadeMapBinding& shade,
                                  const bromath::Vec3& eye) {
    SceneLightingUniforms light = base;
    const SceneVkImage* shadeImage = gpu.resources.shadeMap(shade);
    if (shadeImage) setShade(light, shade, eye);
    return writeLightingSet(gpu, light, VK_NULL_HANDLE, shadeImage);
}

void PassFrameUniforms::record(SceneFrame& frame) {
    frame.lightingSet = writeLightingSet(frame.gpu, frame.lighting, VK_NULL_HANDLE, nullptr);

    // Every TileWorld hands its nodes one map: each draw sampling one gets a
    // set carrying it (shared by the draws of that map).
    std::vector<const uint8_t*> live;
    for (MeshDraw& draw : frame.lists.meshes) {
        if (!(static_cast<uint32_t>(draw.push.pbrParams[3]) & mesh_flags::kShadeMap)) continue;
        draw.lightingSet = lightingSetFor(frame, nullptr, &draw.shade);
        if (std::find(live.begin(), live.end(), draw.shade.pixels) == live.end()) live.push_back(draw.shade.pixels);
    }
    frame.gpu.resources.retainShadeMaps(live);
}

}  // namespace bro::scene::vk
