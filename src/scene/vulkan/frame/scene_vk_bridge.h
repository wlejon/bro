#pragma once

// The Vulkan scene renderer: the GPU state a SceneRenderer draws with and the
// ordered list of passes one render runs (scene_frame_graph.h). Owns the
// frame core view, the shared layouts and fallbacks, the frame targets, the
// per-node resource cache and the mesh drawer; builds each frame's
// SceneFrame (camera view, draw lists, lighting) and hands it to the graph.

#include "render/layer_image.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/lighting/scene_atmosphere.h"
#include "scene/vulkan/lighting/scene_environment.h"
#include "scene/vulkan/lighting/scene_probe_grid.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_frame_graph.h"
#include "scene/vulkan/frame/scene_gpu_resources.h"
#include "scene/vulkan/frame/scene_gpu_timer.h"
#include "scene/vulkan/draw/scene_mesh_drawer.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_view_inspect.h"
#include "scene/vulkan/shadow/scene_caster_drawer.h"
#include "scene/vulkan/shadow/scene_shadow_pages.h"
#include "scene/vulkan/sky/scene_clouds.h"
#include "scene/vulkan/world/scene_world_service.h"

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
    /// The shadow pages' marking passes whose requests the GPU has finished
    /// since the last call, handed to `pages` (oldest first).
    void shadowRequests(ShadowPages& pages) {
        if (ready_) shadows_.takeRequests(device_, pages);
    }

    /// Drop everything held for these destroyed nodes (deferred).
    void releaseNodes(std::span<const uint32_t> ids);

    /// The atmosphere's tables of the last render (SceneAtmosphere::read).
    bool readAtmosphereTables(AtmosphereTables& out) { return ready_ && atmosphere_.read(gpu_, out); }
    /// The probe grid's world of the last render (SceneProbeGrid::read).
    bool readProbeGrid(ProbeGridTables& out) { return ready_ && probes_.read(gpu_, out); }
    /// The clouds' cover cube (SceneClouds::readCover).
    bool readCloudCover(std::vector<float>& out) { return ready_ && clouds_.readCover(gpu_, out); }

    /// The last render's tonemapped result as top-down RGBA. Waits for that
    /// render (its own ticket, never the device); the copy is recorded into
    /// the render itself once a caller has asked in the previous frame, so a
    /// compositor reading every frame pays no extra submission.
    std::vector<uint8_t> readTonemapPixelsRGBA(int& outW, int& outH);
    /// Copy each render's depth and LDR result out with it, for inspect().
    /// Turning it on or off forgets the render compared against.
    void setInspect(bool on);
    bool inspecting() const { return inspect_; }
    /// The last render against the one inspected before it
    /// (scene_view_inspect.h): its stats (ViewInspector::Stat) and, unless
    /// `image` is None, that image of it (top-down RGBA, the render's size).
    /// Waits for the render; false when inspection is off, nothing was
    /// rendered since the last call, or the copy failed.
    bool inspect(ViewInspector::Image image, std::vector<double>& stats, std::vector<uint8_t>& pixels);
    /// GPU milliseconds of the last render, waiting on its ticket; -1 when
    /// nothing was rendered (or timed) since the previous call. Not `wait`:
    /// the oldest render finished and not read yet (SceneGpuTimer::takeMs).
    double takeGpuMs(bool wait = true) { return timer_.takeMs(wait); }
    /// The last timed render's passes (SceneGpuTimer::lastPasses).
    bool gpuPasses(SceneGpuTimer::Passes& passes, SceneGpuTimer::Passes& compute, bool& computed) {
        return timer_.lastPasses(passes, compute, computed);
    }

private:
    bool ensureReadbackBuffer();
    void recordReadback(VkCommandBuffer cmd);
    bool ensureInspectBuffer();
    void releaseInspectBuffer();
    void recordInspectCopy(VkCommandBuffer cmd);

    SceneVkDevice device_;
    SceneVkAllocator allocator_;
    SceneDefaults defaults_;
    SceneTargets targets_;
    SceneGpuResources resources_;
    SceneMeshDrawer meshes_;
    SceneEnvironment environment_;
    SceneWorldService world_;
    SceneAtmosphere atmosphere_;
    SceneShadowPages shadows_;
    SceneCasterDrawer casters_;
    SceneProbeGrid probes_;
    SceneClouds clouds_;
    SceneGpu gpu_;
    SceneFrameGraph graph_;
    SceneGpuTimer timer_;
    bool ready_ = false;

    // CPU readback of targets_.ldr (see readTonemapPixelsRGBA).
    SceneVkBuffer readbackBuffer_;
    uint64_t renderSerial_ = 0;
    uint64_t readbackRecordedSerial_ = 0;   // render whose submission includes the copy
    uint64_t readbackTicket_ = 0;
    bool readSinceRender_ = false;
    bool skippedLastFrame_ = false;   // the last render skipped a draw for a pipeline not yet in
    uint64_t pixelsSerial_ = 0;
    std::vector<uint8_t> pixels_;

    // Inspection (setInspect): the LDR result then the depth, each render's.
    bool inspect_ = false;
    SceneVkBuffer inspectBuffer_;
    uint64_t inspectSerial_ = 0;     // the render whose copy the buffer holds
    uint64_t inspectTicket_ = 0;
    uint64_t inspectedSerial_ = 0;   // the render inspect() last compared
    uint32_t inspectW_ = 0, inspectH_ = 0;
    InspectCamera inspectCamera_;
    ViewInspector inspector_;
};

}  // namespace bro::scene::vk
