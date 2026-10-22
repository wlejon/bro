#pragma once
// The shaders a grass node's cover is planned and drawn with (a
// ModuleShaders set): the built-in ones, compiled with the modules of no
// blades and no herbs (shaders/ground/sward_module.glsl, herb_module.glsl),
// or the same shaders with a world's sward and herb modules in those files'
// places (shaders/ground/scene_sward_layer.glsl and scene_herb_layer.glsl
// say what a module defines). Every shader that calls a module is in the
// set: the plan, the blades and their casters, the herb plants, the far
// tier, the turf and its surface layer's fill.

#include "scene/vulkan/frame/scene_module_shaders.h"

#include <string>

namespace bro::scene::vk {

class CoverShaders : public ModuleShaders {
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
        GroundChartFill,      // its GROUND_LAYER_CHART build (a round body's tiles)
        kStages
    };
    static constexpr BuiltinSceneShader kBuiltin[kStages] = {
        BuiltinSceneShader::CoverPlanComp,     BuiltinSceneShader::MeshGrassVert,
        BuiltinSceneShader::MeshGrassFrag,     BuiltinSceneShader::MeshGrassIndirectFrag,
        BuiltinSceneShader::ShadowGrassVert,
        BuiltinSceneShader::MeshHerbVert,      BuiltinSceneShader::MeshHerbFrag,
        BuiltinSceneShader::MeshHerbIndirectFrag, BuiltinSceneShader::MeshCoverFarVert,
        BuiltinSceneShader::MeshGroundFrag,    BuiltinSceneShader::MeshGroundIndirectFrag,
        BuiltinSceneShader::GroundLayerFillFrag, BuiltinSceneShader::GroundChartFillFrag,
    };

    /// The built-in set.
    bool createBuiltin(VkDevice device) { return ModuleShaders::createBuiltin(device, kBuiltin); }
    /// The set with `sward` as sward_module.glsl and `herbs` as
    /// herb_module.glsl (an empty one keeps the built-in file). False, with
    /// the compiler's diagnostics in `err`, when it does not compile.
    bool create(VkDevice device, const std::string& sward, const std::string& herbs, std::string& err) {
        Overrides overrides;
        if (!sward.empty()) overrides.emplace_back("ground/sward_module.glsl", sward);
        if (!herbs.empty()) overrides.emplace_back("ground/herb_module.glsl", herbs);
        return ModuleShaders::create(device, kBuiltin, overrides, err);
    }

    VkShaderModule operator[](Stage s) const { return module(s); }
};

}  // namespace bro::scene::vk
