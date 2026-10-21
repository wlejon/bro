#pragma once

// The two things every ScenePass is handed.
//
// SceneGpu is the renderer's long-lived GPU state: the frame core, the
// shared layouts and fallbacks, the frame targets, the per-node resource
// cache, the mesh drawer, the environment (IBL maps and the sky), the
// world service, the atmosphere's tables, the shadow pages' pool and the
// depth-only caster pipelines. SceneFrame is one render: the command buffer,
// the graph and its settings, the camera view, what is drawn, the lighting
// and the counters the passes add to.

#include "scene/scene_renderer.h"
#include "scene/vulkan/draw/scene_draw_list.h"
#include "scene/vulkan/frame/scene_view.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

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
class SceneGpuTimer;
class SceneWorldService;
class SceneAtmosphere;
class SceneShadowPages;
class SceneCasterDrawer;
struct SceneVkImage;

struct SceneGpu {
    SceneVkDevice& device;
    SceneVkAllocator& allocator;
    SceneDefaults& defaults;
    SceneTargets& targets;
    SceneGpuResources& resources;
    SceneMeshDrawer& meshes;
    SceneEnvironment& environment;
    SceneWorldService& world;
    SceneAtmosphere& atmosphere;
    SceneShadowPages& shadows;
    SceneCasterDrawer& casters;
};

struct SceneFrame {
    SceneFrame(VkCommandBuffer c, SceneGpu& g, SceneGraph& gr, SceneRenderer& r)
        : cmd(c), gpu(g), graph(gr), renderer(r) {}

    /// The frame's graphics command buffer. It changes once, at the join
    /// (joinCompute): passes read it as they record.
    VkCommandBuffer cmd;
    SceneGpu& gpu;
    SceneGraph& graph;
    SceneRenderer& renderer;

    /// This frame's compute stream (SceneVkDevice::computeCommands), begun on
    /// first use: work there runs on the async compute queue, submitted
    /// ahead of the frame's graphics; without one it is `cmd` itself, so the
    /// work records at this point of the graphics stream.
    VkCommandBuffer computeStream();
    /// The compute stream runs on a queue of its own this frame.
    bool computeApart() const { return computeCmd != VK_NULL_HANDLE && computeCmd != cmd; }
    /// Right before the frame's first consumer of the compute results (the
    /// frame graph calls it for a pass that declares PassIO::readsCompute,
    /// outside any rendering): from here on `cmd` is the part of the frame
    /// that waits for the compute. Once a frame; a no-op without a separate
    /// compute stream.
    void joinCompute();
    VkCommandBuffer computeCmd = VK_NULL_HANDLE;   // computeStream's (null until used)
    bool computeJoined = false;

    SceneView view;
    SceneDrawLists lists;
    CullStats stats;
    /// Anything visible was drawn (the compositor shows the layer).
    bool drewContent = false;
    /// Stamps the end of each pass (SceneGpuTimer::mark); null: untimed.
    SceneGpuTimer* timer = nullptr;

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
