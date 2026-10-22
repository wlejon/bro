#pragma once
// The shaders a land node's tiles are made with (a ModuleShaders set): the
// fill with its world's land module in the place of
// shaders/ground/land_module.glsl, the module of no land it is built with
// (shaders/ground/scene_land_layer.glsl says what a module defines). The
// fill is the only shader that calls the module: the rings
// (mesh_land.vert) read its tiles, the same for every world.

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

    /// The set with `module` as land_module.glsl. False, with the
    /// compiler's diagnostics in `err`, when it does not compile.
    bool create(VkDevice device, const std::string& module, std::string& err) {
        return ModuleShaders::create(device, kBuiltin, {{"ground/land_module.glsl", module}}, err);
    }

    VkShaderModule operator[](Stage s) const { return module(s); }
};

}  // namespace bro::scene::vk
