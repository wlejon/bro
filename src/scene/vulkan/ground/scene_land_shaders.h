#pragma once
// The shaders a land node's tiles are made with (a ModuleShaders set): the
// fill with its world's land module in the place of
// shaders/ground/land_module.glsl, the module of no land it is built with
// (shaders/ground/scene_land_layer.glsl says what a module defines), and
// the canopy of the plants standing on it in the place of
// shaders/ground/land_canopy.glsl (world/plant_canopy.h). The fill is the
// only shader that calls either: the rings (mesh_land.vert) read its
// tiles, the same for every world.

#include "scene/vulkan/frame/scene_module_shaders.h"

#include <string>

namespace bro::scene::vk {

class LandShaders : public ModuleShaders {
public:
    enum Stage : int {
        Fill,   // land_fill.comp
        kStages
    };
    static constexpr BuiltinSceneShader kBuiltin[kStages] = {BuiltinSceneShader::LandFillComp};

    /// The set with `module` as land_module.glsl and `canopy` (or none, "")
    /// as land_canopy.glsl, for a round body's land when `round`
    /// (land_body.glsl). False, with the compiler's diagnostics in `err`,
    /// when it does not compile.
    bool create(VkDevice device, const std::string& module, const std::string& canopy, bool round,
                std::string& err) {
        Overrides overrides{{"ground/land_module.glsl", module}};
        if (!canopy.empty()) overrides.push_back({"ground/land_canopy.glsl", canopy});
        if (round) overrides.push_back({"ground/land_body.glsl", "#define LAND_ROUND\n"});
        return ModuleShaders::create(device, kBuiltin, overrides, err);
    }

    VkShaderModule operator[](Stage s) const { return module(s); }
};

}  // namespace bro::scene::vk
