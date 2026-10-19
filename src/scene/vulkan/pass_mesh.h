#pragma once

// The scene's meshes in the HDR scope, from this frame's draw lists
// (scene_draw_list.h), drawn by SceneMeshDrawer:
//
//   PassForestBatches  splits the opaque draws into OpaqueBatches: a
//                    forest's clumps and crowns, nearest first, into a
//                    batch a run of their entries (the nearest in the
//                    first), the rest in the first; and clears farDepth.
//   PassOpaque       a batch of the opaque meshes. The first always runs,
//                    so the frame's first HDR scope (which clears) always
//                    exists. Carries the indirect-light attachment while
//                    SSAO is on.
//   PassFarDepth     before each later batch: the depth drawn so far into
//                    farDepth, each pixel's farthest sample, so the batch's
//                    sprites behind it are dropped before they place their
//                    leaves (mesh.frag's forestFarRejected). A forest
//                    sprite's stage can discard, which takes the hardware's
//                    early depth test away from it, and its leaves are most
//                    of a forest's fragment time: the wilds' far crowns
//                    shaded 3.9 layers a pixel and the walk's clumps 6.7.
//                    What it drops would have failed the depth test, so
//                    the frame is the same.
//   PassTranslucent  alpha-blended meshes, far to near, after decals.
//   PassOverlay      after tonemapping, into the LDR frame: the unlit meshes
//                    (so their authored colours are not tonemapped), depth
//                    tested against the scene without writing it, alpha
//                    blended, unfogged; then the editor's transform handles
//                    (SceneGraph's gizmo provider) on top of everything.
//                    Handles that stop being provided have their GPU meshes
//                    released.

#include "scene/vulkan/scene_mesh_drawer.h"
#include "scene/vulkan/scene_pass.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace bro::scene::vk {

/// The frame's opaque draws as PassForestBatches splits them, in order
/// within each batch: each a draw of the frame's lists, or a run of its
/// entries (read at record time: passes between, PassReflectionProbe, set
/// the draws' lighting).
struct OpaqueBatches {
    static constexpr int kBatches = 3;
    struct Draw {
        uint32_t mesh;               // frame.lists.meshes
        bool split = false;          // only entries [first, first + count)
        uint32_t first = 0, count = 0;
        int batch = -1;              // a GPU-planned draw: its batch's own command (MeshDraw::indirectBatches)
    };
    std::vector<Draw> draws[kBatches];
    bool sprites = false;   // a forest sprite draws (farDepth is read)
};

class PassForestBatches final : public ScenePass {
public:
    explicit PassForestBatches(OpaqueBatches& batches) : batches_(batches) {}
    const char* name() const override { return "forestBatches"; }
    bool setup(SceneGpu&) override { return true; }
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}

private:
    OpaqueBatches& batches_;   // (filled by declare: what the later passes are)
};

class PassOpaque final : public ScenePass {
public:
    PassOpaque(const OpaqueBatches& batches, int batch) : batches_(batches), batch_(batch) {}
    const char* name() const override { return batch_ == 0 ? "opaque" : batch_ == 1 ? "opaque1" : "opaque2"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}

private:
    const OpaqueBatches& batches_;
    int batch_;
};

class PassFarDepth final : public ScenePass {
public:
    PassFarDepth(const OpaqueBatches& batches, int batch) : batches_(batches), batch_(batch) {}
    const char* name() const override { return "farDepth"; }
    bool setup(SceneGpu& gpu) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    const OpaqueBatches& batches_;
    int batch_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipelines_[2] = {};   // single-sample depth, MSAA depth
    VkSampler sampler_ = VK_NULL_HANDLE;
};

class PassTranslucent final : public ScenePass {
public:
    const char* name() const override { return "translucent"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

class PassOverlay final : public ScenePass {
public:
    const char* name() const override { return "overlay"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}

private:
    std::vector<uint32_t> gizmoIds_;   // the handles drawn last frame, sorted
};

}  // namespace bro::scene::vk
