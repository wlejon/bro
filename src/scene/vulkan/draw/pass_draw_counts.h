#pragma once

// The frame's first pass: instanced draws whose instances a kernel decides.
//
// GPU-counted draws (InstancedMeshNode::setInstanceCountBuffer): each such
// draw of the frame's lists gets two VkDrawIndexedIndirectCommands in a
// buffer of its own (the draw's, then its shadow prefix's), written on the
// GPU (shaders/draw/draw_counts.bk): the count a kernel left in the node's
// buffer, clamped to its instance rows, so a count past them draws no row
// that is not there. Every later pass issues the draw from them
// (SceneMeshDrawer::bindGeometryAndDraw), so a frame whose instances a
// kernel chose (a crowd's levels of detail) reads nothing back.
//
// Culled draws (InstancedMeshNode::setInstanceCullSpheres): for each view
// that draws one (the camera, and each of the frame's shadow views it casts
// into and whose frustum reaches its bounds), the instances whose sphere
// reaches the view's planes are compacted into a region of their own: their
// rows, their own indices (a vertex hook's instance_index) and two commands
// counting them (shaders/draw/instance_cull.bk). That view draws the region
// (MeshDraw::inView); a view with none (a probe face) draws the rows whole.
//
// The camera's instances of an opaque culled draw are those in sight, by
// this frame's depth. The first cull keeps the ones last frame's test found
// in sight (a word an instance, `seen_`), and PassOpaque draws them with
// everything else opaque. PassInstanceOcclusion then reduces the depth they
// left to a pyramid of farthest depths (shaders/draw/depth_pyramid.bk) and
// tests every instance in the view against it: the test's answers are next
// frame's `seen_`, and the instances in sight that the first cull left out
// make a second region (InstanceCullViews::kLate), which PassOpaqueLate
// draws into the same targets. An instance buried in a heap, or behind a
// wall, is not drawn; one that comes into sight is drawn the frame it does.
//
// One small dispatch a counted draw and one a culled draw's view; past
// kMaxDraws counted draws, or kMaxRegions views, the rest draw all their
// rows.

#include "scene/vulkan/draw/scene_mesh_drawer.h"
#include "scene/vulkan/frame/scene_pass.h"

#include <vulkan/vulkan.h>

#include <vector>

namespace bro::scene::vk {

class PassDrawCounts final : public ScenePass {
public:
    static constexpr uint32_t kMaxDraws = 1024;
    static constexpr uint32_t kMaxRegions = 512;

    const char* name() const override { return "draw-counts"; }
    bool setup(SceneGpu& gpu) override;
    bool active(const SceneFrame& frame) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

    /// The depth's pyramid and the late cull of this frame's opaque culled
    /// draws (PassInstanceOcclusion, once the opaque surfaces are drawn).
    void recordLate(SceneFrame& frame);

private:
    struct Region {
        MeshDraw* draw;
        int view;
        const bromath::Frustum* frustum;
        uint32_t rowsWord, idsWord;
        uint32_t seenWord;   // kNone: every instance in the view is kept
    };
    struct Late {
        Region region;
        size_t index;        // of the region, in the commands
    };

    void recordCounts(SceneFrame& frame);
    void recordCulls(SceneFrame& frame);
    bool dispatchCull(SceneFrame& frame, const Region& rg, size_t index, VkDeviceSize outBytes,
                      VkDeviceSize cmdBytes);
    bool buildPyramid(SceneFrame& frame);

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    SceneVkBuffer commands_;
    bool warned_ = false;

    VkDescriptorSetLayout cullSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout cullLayout_ = VK_NULL_HANDLE;
    VkPipeline cullPipeline_ = VK_NULL_HANDLE;
    SceneVkBuffer cullCommands_;   // two commands a region
    SceneVkBuffer cullOut_;        // the regions' rows and indices, grown as needed
    std::vector<InstanceCullViews> views_;   // this frame's, one a culled draw
    bool cullWarned_ = false;

    VkPipeline latePipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout pyramidSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pyramidLayout_ = VK_NULL_HANDLE;
    VkPipeline pyramidFirst_ = VK_NULL_HANDLE;
    VkPipeline pyramidNext_ = VK_NULL_HANDLE;
    VkSampler depthSampler_ = VK_NULL_HANDLE;
    SceneVkBuffer seen_;      // a word an instance of the frame's opaque culled draws, in their order
    SceneVkBuffer pyramid_;   // the levels, one after another
    uint32_t pyramidW_ = 0, pyramidH_ = 0, pyramidLevels_ = 0;
    std::vector<Late> late_;  // this frame's second regions
    VkDeviceSize outBytes_ = 0, cmdBytes_ = 0;   // this frame's regions
};

/// The late cull (PassDrawCounts::recordLate), after the opaque surfaces.
class PassInstanceOcclusion final : public ScenePass {
public:
    explicit PassInstanceOcclusion(PassDrawCounts& counts) : counts_(counts) {}
    const char* name() const override { return "instance-occlusion"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override { counts_.recordLate(frame); }
    void cleanup(SceneGpu&) override {}

private:
    PassDrawCounts& counts_;
};

}  // namespace bro::scene::vk
