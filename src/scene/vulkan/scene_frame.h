#pragma once

// The two things every ScenePass is handed.
//
// SceneGpu is the renderer's long-lived GPU state: the frame core, the
// shared layouts and fallbacks, the frame targets, the per-node resource
// cache, the mesh drawer and the environment (IBL maps and the sky). SceneFrame is one render: the command buffer,
// the graph and its settings, the camera view, what is drawn, the lighting
// and the counters the passes add to.

#include "scene/scene_renderer.h"
#include "scene/vulkan/scene_draw_list.h"
#include "scene/vulkan/scene_view.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <utility>

namespace bro::scene {
class SceneGraph;
}

namespace bro::scene::vk {

class SceneVkDevice;
class SceneVkAllocator;
class SceneDefaults;
class SceneTargets;
class SceneGpuResources;
class SceneMeshDrawer;
class SceneEnvironment;
struct SceneVkImage;

struct SceneGpu {
    SceneVkDevice& device;
    SceneVkAllocator& allocator;
    SceneDefaults& defaults;
    SceneTargets& targets;
    SceneGpuResources& resources;
    SceneMeshDrawer& meshes;
    SceneEnvironment& environment;
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
    bool ssr = false;    // opaque draws write the reflectance mask; SSR consumes it
    bool dof = false;
    bool tilt = false;   // tilt-shift after the overlay
    bool fxaa = false;   // FXAA last

    /// The LDR image holding the frame so far: written by the tonemap, drawn
    /// over by the overlay, replaced by tilt-shift and FXAA. Whichever post
    /// pass runs last writes targets.ldr.
    SceneVkImage* ldrResult = nullptr;

    /// Sun, lights, ambient, shadow projection, environment and atmosphere
    /// (scene_lighting.h). No probe and no shade map: a draw's own lighting
    /// set adds those.
    SceneLightingUniforms lighting{};
    /// The lighting sets written this frame per (probe node id, shade map
    /// pixels), beyond the frame's own (lightingSetFor).
    std::map<std::pair<uint32_t, const uint8_t*>, VkDescriptorSet> lightingSets;

    /// This frame's camera and lighting sets (written by the frame-uniforms
    /// pass). A draw inside a reflection probe's box carries that probe's
    /// lighting set instead (MeshDraw::lightingSet).
    VkDescriptorSet cameraSet = VK_NULL_HANDLE;
    VkDescriptorSet lightingSet = VK_NULL_HANDLE;

    /// The attachments of the open HDR scope (set by the frame graph).
    TargetFormat hdrTarget;
};

}  // namespace bro::scene::vk
