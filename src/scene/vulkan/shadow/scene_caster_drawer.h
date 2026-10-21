#pragma once

// Depth-only casters: a MeshDraw drawn into the open rendering's depth
// through a view-projection of its own, with the colour pass's wind sway,
// vegetation sway and custom vertex chunks so the silhouette matches what is
// drawn, and a textured static or instanced mesh with an alpha cutoff cut
// by its base colour's alpha (scene_shadow_cutout.glsl). The shadow pass
// draws its views' casters with it (pass_shadow.h); the probe grid captures
// the ground's height from above with it (scene_probe_grid.h). One set of
// pipelines, built on first use, shared by every user.
//
// A caster culls the faces turned toward its view, so the depth stored is
// its far side and a closed mesh does not shadow itself; a two-sided one
// draws both. A capture (Mode::Capture) draws both faces of everything and
// none of its sway: what stands still, as it stands.

#include "scene/vulkan/draw/scene_mesh_drawer.h"

#include <bromath/frustum.h>
#include <bromath/mat.h>
#include <vulkan/vulkan.h>

#include <string>
#include <unordered_map>

namespace bro::scene::vk {

struct SceneGpu;
struct SceneFrame;
class SceneVkDevice;

class SceneCasterDrawer {
public:
    enum class Mode : uint8_t {
        Caster,    // a shadow caster: its far side, swaying as drawn
        Capture,   // both faces, still
    };

    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);

    /// Record `draw` into `cmd`, inside a rendering with a SceneTargets::
    /// kDepthFormat depth attachment, through `viewProj` (camera-relative
    /// positions to Vulkan clip space). `frustum`: a chunked mesh draws the
    /// chunks inside it (null: all of them).
    void draw(SceneFrame& frame, VkCommandBuffer cmd, const bromath::Mat4& viewProj, const MeshDraw& draw,
              const bromath::Frustum* frustum, Mode mode = Mode::Caster);

private:
    /// shaders/common/scene_shadow_push.glsl.
    struct alignas(16) Push {
        float lightViewProj[16];
        float modelRows[12];
        float params[4];   // x wind mask (a plants caster's mode); yzw a cutout caster's
    };
    static_assert(sizeof(Push) == 128);

    VkPipeline builtinPipeline(MeshKind kind, bool twoSided);
    VkPipeline cutoutPipeline(MeshKind kind, bool twoSided);
    VkPipeline customPipeline(const CustomShaderState& cs, MeshKind kind, bool twoSided, bool cutout);
    VkPipeline buildPipeline(VkShaderModule vs, MeshKind kind, bool custom, bool twoSided, bool cutout);

    SceneVkDevice* device_ = nullptr;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_[kMeshKindCount] = {};
    VkShaderModule fs_ = VK_NULL_HANDLE;
    VkPipeline builtin_[kMeshKindCount][2] = {};   // [kind][two-sided]
    // Cutout casters (a textured static or instanced mesh with an alpha
    // cutoff): their base colour's alpha cut, scene_shadow_cutout.glsl.
    VkShaderModule vsCutout_[2] = {};              // [instanced]
    VkShaderModule fsCutout_ = VK_NULL_HANDLE;
    VkPipeline cutout_[2][2] = {};                 // [instanced][two-sided]
    // Custom-vertex casters by (chunk key, kind, sidedness); VK_NULL_HANDLE
    // remembers a failed compile, so it is reported once and the caster
    // falls back to its undisplaced silhouette.
    std::unordered_map<std::string, VkPipeline> custom_;
};

}  // namespace bro::scene::vk
