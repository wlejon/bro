#pragma once
// The shaders a plants node is drawn with (a ModuleShaders set): the
// plants' shaders with its world's plant module in the place of
// shaders/foliage/plant_module.glsl, the module of no plants they are built
// with (shaders/foliage/scene_plant_layer.glsl says what a module defines;
// a node with none draws nothing). Every shader that calls the module is in the
// set: the element poses, the plants' vertex and fragment stages and their
// casters. The plan (plants_plan.comp) reads only the engine's share of the
// records and is the same for every world.

#include "scene/vulkan/frame/scene_module_shaders.h"

#include <string>

namespace bro::scene::vk {

class PlantShaders : public ModuleShaders {
public:
    enum Stage : int {
        Pose,           // plants_pose.comp
        Vert,           // plants.vert
        Frag,           // plants.frag
        IndirectFrag,
        ShadowVert,     // shadow_plants.vert
        kStages
    };
    static constexpr BuiltinSceneShader kBuiltin[kStages] = {
        BuiltinSceneShader::PlantsPoseComp, BuiltinSceneShader::PlantsVert,       BuiltinSceneShader::PlantsFrag,
        BuiltinSceneShader::PlantsIndirectFrag, BuiltinSceneShader::ShadowPlantsVert,
    };

    /// The set with `module` as plant_module.glsl, compiled off the frame
    /// thread (ModuleShaders::poll).
    void start(const std::string& module) { ModuleShaders::start(kBuiltin, {{"foliage/plant_module.glsl", module}}); }

    VkShaderModule operator[](Stage s) const { return module(s); }
};

}  // namespace bro::scene::vk
