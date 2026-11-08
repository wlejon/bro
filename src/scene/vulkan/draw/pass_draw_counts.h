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

private:
    void recordCounts(SceneFrame& frame);
    void recordCulls(SceneFrame& frame);

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
};

}  // namespace bro::scene::vk
