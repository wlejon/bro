#pragma once
// The shaders a grass node's cover is planned and drawn with: the built-in
// ones, compiled at build time with the modules of no blades and no herbs
// (shaders/ground/sward_module.glsl, herb_module.glsl), or the same shaders
// with a world's sward and herb modules in those files' places, compiled at
// run time (shaders/ground/scene_sward_layer.glsl and scene_herb_layer.glsl
// say what a module defines). Every shader that calls a module is in the
// set: the plan, the blades and their casters, the herb plants, the far
// tier, the turf and its surface layer's fill.
//
// A world's set compiles its shaders in parallel, each through the scene's
// SPIR-V cache (docs/code-cache.md), so a second run of a world compiles
// nothing.

#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/vulkan/frame/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace bro::scene::vk {

class CoverShaders {
public:
    enum Stage : int {
        Plan,                 // cover_plan.comp
        GrassVert,            // mesh_grass.vert
        GrassFrag,            // mesh.frag's SCENE_GRASS build
        GrassIndirectFrag,
        GrassShadowVert,      // shadow_grass.vert
        HerbVert,             // mesh_herb.vert
        HerbFrag,             // mesh.frag's SCENE_HERB build
        HerbIndirectFrag,
        FarVert,              // mesh_cover_far.vert
        GroundFrag,           // mesh.frag's SCENE_GROUND build
        GroundIndirectFrag,
        GroundFill,           // ground_layer_fill.frag
        kStages
    };
    static constexpr BuiltinSceneShader kBuiltin[kStages] = {
        BuiltinSceneShader::CoverPlanComp,     BuiltinSceneShader::MeshGrassVert,
        BuiltinSceneShader::MeshGrassFrag,     BuiltinSceneShader::MeshGrassIndirectFrag,
        BuiltinSceneShader::ShadowGrassVert,
        BuiltinSceneShader::MeshHerbVert,      BuiltinSceneShader::MeshHerbFrag,
        BuiltinSceneShader::MeshHerbIndirectFrag, BuiltinSceneShader::MeshCoverFarVert,
        BuiltinSceneShader::MeshGroundFrag,    BuiltinSceneShader::MeshGroundIndirectFrag,
        BuiltinSceneShader::GroundLayerFillFrag,
    };

    /// The built-in set.
    bool createBuiltin(VkDevice device);
    /// The set with `sward` as sward_module.glsl and `herbs` as
    /// herb_module.glsl (an empty one keeps the built-in file). False, with
    /// the compiler's diagnostics in `err`, when it does not compile.
    bool create(VkDevice device, const std::string& sward, const std::string& herbs, std::string& err);
    void destroy(VkDevice device);

    VkShaderModule operator[](Stage s) const { return modules_[s]; }
    /// Which set it is among the drawer's (0 the built-in one).
    uint32_t index = 0;
    /// The drawer's pipelines of these shaders (the cover plan keeps its
    /// own, by index).
    PipelineVariants pipelines;

private:
    VkShaderModule modules_[kStages] = {};
};

}  // namespace bro::scene::vk
