#pragma once

// The two things every ScenePass is handed.
//
// SceneGpu is the renderer's long-lived GPU state: the frame core, the
// shared layouts and fallbacks, the frame targets, the per-node resource
// cache and the mesh drawer. SceneFrame is one render: the command buffer,
// the graph and its settings, the camera view, what is drawn, the lighting
// and the counters the passes add to.

#include "scene/scene_renderer.h"
#include "scene/vulkan/scene_draw_list.h"
#include "scene/vulkan/scene_view.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace bro::scene {
class SceneGraph;
class ReflectionProbeNode;
}

namespace bro::scene::vk {

class SceneVkDevice;
class SceneVkAllocator;
class SceneDefaults;
class SceneTargets;
class SceneGpuResources;
class SceneMeshDrawer;

struct SceneGpu {
    SceneVkDevice& device;
    SceneVkAllocator& allocator;
    SceneDefaults& defaults;
    SceneTargets& targets;
    SceneGpuResources& resources;
    SceneMeshDrawer& meshes;
};

struct SceneFrame {
    SceneFrame(VkCommandBuffer c, SceneGpu& g, SceneGraph& gr, SceneRenderer& r)
        : cmd(c), gpu(g), graph(gr), renderer(r) {}

    VkCommandBuffer cmd;
    SceneGpu& gpu;
    SceneGraph& graph;
    SceneRenderer& renderer;

    SceneView view;
    SceneDrawLists lists;
    CullStats stats;
    /// Anything visible was drawn (the compositor shows the layer).
    bool drewContent = false;

    // Effects that change the pass structure, decided once per frame.
    bool ssao = false;   // opaque scope carries the indirect attachment; AO applies
    bool dof = false;
    bool lut = false;

    /// Sun, lights, ambient and shadow projection (scene_lighting.h). The
    /// probe and shade-map fields are filled by the frame-uniforms pass.
    SceneLightingUniforms lighting{};
    bool shadowed = false;   // the sun casts a shadow this frame

    /// The reflection probe the lit passes sample (set by the probe pass).
    struct Probe {
        const ReflectionProbeNode* node = nullptr;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t mipLevels = 1;
    } probe;

    /// This frame's camera and lighting sets (written by the frame-uniforms pass).
    VkDescriptorSet cameraSet = VK_NULL_HANDLE;
    VkDescriptorSet lightingSet = VK_NULL_HANDLE;

    /// The attachments of the open HDR scope (set by the frame graph).
    TargetFormat hdrTarget;
};

}  // namespace bro::scene::vk
