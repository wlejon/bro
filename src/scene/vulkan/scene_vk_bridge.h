#pragma once

// The Vulkan scene renderer: the GPU state a SceneRenderer draws with and the
// ordered list of passes one render runs (scene_frame_graph.h). Owns the
// frame core view, the shared layouts and fallbacks, the frame targets, the
// per-node resource cache and the mesh drawer; builds each frame's
// SceneFrame (camera view, draw lists, lighting) and hands it to the graph.

#include "render/layer_image.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_frame_graph.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_mesh_drawer.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_device.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bro::render {
class VulkanContext;
}

namespace bro::scene {
class SceneGraph;
class SceneRenderer;
}

namespace bro::scene::vk {

class PassColorLut;

class SceneVkBridge {
public:
    explicit SceneVkBridge(render::VulkanContext& context);
    ~SceneVkBridge();

    SceneVkBridge(const SceneVkBridge&) = delete;
    SceneVkBridge& operator=(const SceneVkBridge&) = delete;

    bool init();

    /// Render the graph into the LDR output. Fills `stats` with the passes'
    /// counters; returns whether anything visible was drawn.
    bool render3D(SceneGraph& graph, SceneRenderer& renderer, CullStats& stats);

    /// The last render's LDR output (shader-read once its submission completes).
    render::LayerImage outputImage() const;

    /// Drop everything held for these destroyed nodes (deferred).
    void releaseNodes(std::span<const uint32_t> ids);

    /// The last render's tonemapped result as top-down RGBA. Waits for that
    /// render (its own ticket, never the device); the copy is recorded into
    /// the render itself once a caller has asked in the previous frame, so a
    /// compositor reading every frame pays no extra submission.
    std::vector<uint8_t> readTonemapPixelsRGBA(int& outW, int& outH);

private:
    bool ensureReadbackBuffer();
    void recordReadback(VkCommandBuffer cmd);

    SceneVkDevice device_;
    SceneVkAllocator allocator_;
    SceneDefaults defaults_;
    SceneTargets targets_;
    SceneGpuResources resources_;
    SceneMeshDrawer meshes_;
    SceneGpu gpu_;
    SceneFrameGraph graph_;
    PassColorLut* colorLut_ = nullptr;   // owned by graph_
    bool ready_ = false;

    // CPU readback of targets_.ldr (see readTonemapPixelsRGBA).
    SceneVkBuffer readbackBuffer_;
    uint64_t renderSerial_ = 0;
    uint64_t readbackRecordedSerial_ = 0;   // render whose submission includes the copy
    uint64_t readbackTicket_ = 0;
    bool readSinceRender_ = false;
    uint64_t pixelsSerial_ = 0;
    std::vector<uint8_t> pixels_;
};

}  // namespace bro::scene::vk
